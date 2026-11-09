#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "daemon.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define EXIT_OK 0
#define EXIT_RUNTIME 3
static int pidfile_fd = -1;
static int startup_fd = -1;

int lock_pidfile(const char *pidfile_path) {
    int fd = open(pidfile_path, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        fprintf(stderr, "failed to open pidfile '%s': %s\n", pidfile_path, strerror(errno));
        return -1;
    }

    if (fd <= STDERR_FILENO) {
        int duplicate = fcntl(fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
        close(fd);
        if (duplicate < 0) {
            fprintf(stderr, "failed to reserve pidfile descriptor: %s\n", strerror(errno));
            return -1;
        }
        fd = duplicate;
    }

    struct stat pidfile_stat;
    if (fstat(fd, &pidfile_stat) != 0 || !S_ISREG(pidfile_stat.st_mode)) {
        fprintf(stderr, "pidfile is not a regular file: %s\n", pidfile_path);
        close(fd);
        return -1;
    }

    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr, "fileward is already running or pidfile is locked: %s\n", pidfile_path);
        close(fd);
        return -1;
    }

    pidfile_fd = fd;
    return 0;
}

int write_pidfile(pid_t pid) {
    if (pidfile_fd < 0) {
        return 0;
    }

    char contents[32];
    int len = snprintf(contents, sizeof(contents), "%ld\n", (long)pid);
    if (len < 0 || (size_t)len >= sizeof(contents) || ftruncate(pidfile_fd, 0) != 0 ||
        lseek(pidfile_fd, 0, SEEK_SET) < 0) {
        fprintf(stderr, "failed to write pidfile: %s\n", strerror(errno));
        return -1;
    }

    size_t written = 0;
    while (written < (size_t)len) {
        ssize_t count = write(pidfile_fd, contents + written, (size_t)len - written);
        if (count <= 0) {
            fprintf(stderr, "failed to write pidfile: %s\n", strerror(errno));
            return -1;
        }
        written += (size_t)count;
    }

    return 0;
}

void release_pidfile(void) {
    if (pidfile_fd >= 0) {
        close(pidfile_fd);
        pidfile_fd = -1;
    }
}

static int is_pid_running(pid_t pid);

int read_running_pid(const char *pidfile_path, pid_t *pid_out) {
    int fd = open(pidfile_path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
        close(fd);
        return -1;
    }
    if (errno != EWOULDBLOCK) {
        close(fd);
        return -1;
    }

    FILE *fp = fdopen(fd, "r");
    if (fp == NULL) {
        close(fd);
        return -1;
    }
    long pid_value = 0;
    int parsed = fscanf(fp, "%ld", &pid_value);
    fclose(fp);
    if (parsed != 1 || pid_value <= 0) {
        return -1;
    }
    *pid_out = (pid_t)pid_value;
    return is_pid_running(*pid_out) ? 0 : -1;
}

static int is_pid_running(pid_t pid) {
    if (pid <= 0) {
        return 0;
    }

    return kill(pid, 0) == 0 || errno == EPERM;
}

int redirect_output(const char *log_file_path) {
    if (log_file_path == NULL || log_file_path[0] == '\0') {
        int devnull = open("/dev/null", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (devnull < 0) {
            return -1;
        }
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        close(devnull);
        return 0;
    }

    int log_fd = open(log_file_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (log_fd < 0) {
        return -1;
    }

    dup2(log_fd, STDOUT_FILENO);
    dup2(log_fd, STDERR_FILENO);
    close(log_fd);
    return 0;
}

int daemonize_process(const char *log_file_path) {
    int startup_pipe[2];
    if (pipe(startup_pipe) != 0) {
        fprintf(stderr, "startup pipe failed: %s\n", strerror(errno));
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "fork failed: %s\n", strerror(errno));
        close(startup_pipe[0]);
        close(startup_pipe[1]);
        return -1;
    }

    if (pid > 0) {
        close(startup_pipe[1]);
        char result = 0;
        ssize_t received;
        do {
            received = read(startup_pipe[0], &result, 1);
        } while (received < 0 && errno == EINTR);
        close(startup_pipe[0]);
        _exit(received == 1 && result == '1' ? EXIT_OK : EXIT_RUNTIME);
    }

    close(startup_pipe[0]);
    startup_fd = startup_pipe[1];

    if (setsid() < 0) {
        fprintf(stderr, "setsid failed: %s\n", strerror(errno));
        return -1;
    }

    signal(SIGHUP, SIG_IGN);

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "second fork failed: %s\n", strerror(errno));
        return -1;
    }

    if (pid > 0) {
        _exit(EXIT_OK);
    }

    /* Preserve the launch directory for relative config, log and action paths. */

    close(STDIN_FILENO);
    if (redirect_output(log_file_path) != 0) {
        fprintf(stderr, "failed to redirect output: %s\n", strerror(errno));
        return -1;
    }

    umask(0);
    if (write_pidfile(getpid()) != 0) {
        return -1;
    }

    return 0;
}

void notify_startup(int success) {
    if (startup_fd >= 0) {
        char result = success ? '1' : '0';
        (void)write(startup_fd, &result, 1);
        close(startup_fd);
        startup_fd = -1;
    }
}
