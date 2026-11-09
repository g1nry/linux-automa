#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "handlers.h"
#include <fileward/log.h>
#include <fileward/audit.h>
#include <spawn.h>
#include <pthread.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

static const volatile sig_atomic_t *running_flag;
static const atomic_int *cancel_flag;
static _Thread_local const char *last_code;
static _Thread_local int attempts;
const char *action_last_error_code(int error) {
    return last_code ? last_code : audit_error_code(error);
}
int action_attempts(void) {
    return attempts;
}
void action_set_cancel_flag(const atomic_int *flag) {
    cancel_flag = flag;
}

void action_set_running_flag(const volatile sig_atomic_t *flag) {
    running_flag = flag;
}

int action_cancelled(void) {
    if (audit_failed() || (cancel_flag && atomic_load(cancel_flag)) || (running_flag && !*running_flag)) {
        errno = ECANCELED;
        return 1;
    }
    return 0;
}

static int pause_ms(int milliseconds) {
    while (milliseconds > 0) {
        if (action_cancelled()) {
            return -1;
        }
        int chunk = milliseconds > 20 ? 20 : milliseconds;
        struct timespec delay = {0, chunk * 1000000L};
        while (nanosleep(&delay, &delay) != 0) {
            if (errno != EINTR || action_cancelled()) {
                return -1;
            }
        }
        milliseconds -= chunk;
    }
    return 0;
}

static int format_path(char *out, size_t size, const char *directory, const char *name) {
    int length = snprintf(out, size, "%s/%s", directory, name);
    if (length < 0 || (size_t)length >= size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int safe_relative(const char *name) {
    if (!*name || *name == '/') {
        return 0;
    }
    for (const char *p = name; *p;) {
        const char *end = strchr(p, '/');
        size_t length = end ? (size_t)(end - p) : strlen(p);
        if (length == 0 || (length == 2 && p[0] == '.' && p[1] == '.')) {
            return 0;
        }
        if (!end) {
            break;
        }
        p = end + 1;
    }
    return 1;
}

int action_expand_target(const char *text, const char *root, const char *filename, event_type_t event,
                         char out[PATH_MAX]) {
    if (text[0] == '~' && (text[1] == '/' || text[1] == '\0')) {
        const char *home = getenv("HOME");
        if (!home) {
            errno = EINVAL;
            return -1;
        }
        char expanded[PATH_MAX];
        if (action_expand_template(text + 1, root, filename, event, expanded, sizeof(expanded)) != 0) {
            return -1;
        }
        int length = snprintf(out, PATH_MAX, "%s%s", home, expanded);
        if (length < 0 || length >= PATH_MAX) {
            errno = ENAMETOOLONG;
            return -1;
        }
    } else if (action_expand_template(text, root, filename, event, out, PATH_MAX) != 0) {
        return -1;
    }
    if (!out[0]) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static int ensure_directory(const char *path) {
    char buffer[PATH_MAX];
    if (!*path || strlen(path) >= sizeof(buffer)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(buffer, path);
    size_t length = strlen(buffer);
    while (length > 1 && buffer[length - 1] == '/') {
        buffer[--length] = '\0';
    }
    for (char *p = buffer + 1;; p++) {
        if (*p != '/' && *p != '\0') {
            continue;
        }
        char saved = *p;
        *p = '\0';
        if (mkdir(buffer, 0755) != 0) {
            struct stat st;
            if (errno != EEXIST || stat(buffer, &st) != 0) {
                return -1;
            }
            if (!S_ISDIR(st.st_mode)) {
                errno = ENOTDIR;
                return -1;
            }
        }
        *p = saved;
        if (!saved) {
            break;
        }
    }
    return 0;
}

static int same_file(const char *source, const char *destination) {
    struct stat first, second;
    return lstat(source, &first) == 0 && lstat(destination, &second) == 0 && first.st_dev == second.st_dev &&
           first.st_ino == second.st_ino;
}

/* Publish by atomic rename. No check-then-overwrite window for fail/skip/rename. */
static int publish(const char *source, const char *destination, conflict_policy_t conflict,
                   char actual[PATH_MAX]) {
    for (unsigned suffix = 0; suffix <= 1000; suffix++) {
        int length = suffix ? snprintf(actual, PATH_MAX, "%s.%u", destination, suffix)
                            : snprintf(actual, PATH_MAX, "%s", destination);
        if (length < 0 || length >= PATH_MAX) {
            errno = ENAMETOOLONG;
            return -1;
        }
        if (same_file(source, actual)) {
            errno = EINVAL;
            return -1;
        }
        unsigned flags = conflict == CONFLICT_OVERWRITE ? 0 : RENAME_NOREPLACE;
        if (renameat2(AT_FDCWD, source, AT_FDCWD, actual, flags) == 0) {
            audit_destination(actual);
            return 0;
        }
        if (errno != EEXIST) {
            return -1;
        }
        if (conflict == CONFLICT_SKIP) {
            return ACTION_SKIPPED;
        }
        if (conflict != CONFLICT_RENAME) {
            return -1;
        }
    }
    errno = EEXIST;
    return -1;
}

static int source_unchanged(const struct stat *before, const struct stat *after) {
    return before->st_dev == after->st_dev && before->st_ino == after->st_ino &&
           before->st_size == after->st_size && before->st_mtim.tv_sec == after->st_mtim.tv_sec &&
           before->st_mtim.tv_nsec == after->st_mtim.tv_nsec &&
           before->st_ctim.tv_sec == after->st_ctim.tv_sec &&
           before->st_ctim.tv_nsec == after->st_ctim.tv_nsec;
}

static int write_all(int fd, const char *data, size_t length) {
    while (length) {
        ssize_t written = write(fd, data, length);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            if (written == 0) {
                errno = EIO;
            }
            return -1;
        }
        data += written;
        length -= (size_t)written;
    }
    return 0;
}

/* Bounded to the initial length; changing sources are rejected before publication. */
static int stream_file(int source, int destination, off_t length, int compress) {
    gzFile gzip = NULL;
    if (compress) {
        int fd = fcntl(destination, F_DUPFD_CLOEXEC, 3);
        if (fd < 0) {
            return -1;
        }
        gzip = gzdopen(fd, "wb");
        if (!gzip) {
            close(fd);
            errno = ENOMEM;
            return -1;
        }
    }
    char buffer[65536];
    int result = 0;
    while (length > 0) {
        if (action_cancelled()) {
            result = -1;
            break;
        }
        size_t wanted = length > (off_t)sizeof(buffer) ? sizeof(buffer) : (size_t)length;
        ssize_t count = read(source, buffer, wanted);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            if (count == 0) {
                errno = EAGAIN;
            }
            result = -1;
            break;
        }
        if (compress) {
            if (gzwrite(gzip, buffer, (unsigned)count) != count) {
                errno = EIO;
                result = -1;
                break;
            }
        } else if (write_all(destination, buffer, (size_t)count) != 0) {
            result = -1;
            break;
        }
        length -= count;
    }
    int saved = errno;
    if (gzip && gzclose(gzip) != Z_OK && result == 0) {
        result = -1;
        saved = EIO;
    }
    errno = saved;
    return result;
}

static int copy_or_archive(const char *source, const char *directory, const char *destination,
                           const action_t *action, int remove_source) {
    int input = open(source, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (input < 0) {
        return -1;
    }
    struct stat before, after;
    if (fstat(input, &before) != 0) {
        close(input);
        return -1;
    }
    if (!S_ISREG(before.st_mode)) {
        close(input);
        errno = ENOTSUP;
        return -1;
    }
    if (same_file(source, destination)) {
        close(input);
        errno = EINVAL;
        return -1;
    }
    char temporary[PATH_MAX];
    if (format_path(temporary, sizeof(temporary), directory, ".fileward-XXXXXX") != 0) {
        close(input);
        return -1;
    }
    int output = mkostemp(temporary, O_CLOEXEC);
    if (output < 0) {
        close(input);
        return -1;
    }
    int result = -1;
    char actual[PATH_MAX];
    if (stream_file(input, output, before.st_size, action->type == ACTION_ARCHIVE) != 0) {
        goto done;
    }
    if (fstat(input, &after) != 0) {
        goto done;
    }
    if (!source_unchanged(&before, &after)) {
        errno = EAGAIN;
        goto done;
    }
    if (fchmod(output, before.st_mode & 0777) != 0) {
        goto done;
    }
    struct timespec times[2] = {before.st_atim, before.st_mtim};
    if (futimens(output, times) != 0 || fsync(output) != 0) {
        goto done;
    }
    if (close(output) != 0) {
        output = -1;
        goto done;
    }
    output = -1;
    if (action_cancelled()) {
        goto done;
    }
    result = publish(temporary, destination, action->conflict, actual);
    if (result != 0) {
        goto done;
    }
    if (remove_source) {
        /* Never remove a replacement that arrived while the file was copied. */
        if (lstat(source, &after) != 0 || !source_unchanged(&before, &after)) {
            errno = ESTALE;
            result = -1;
            log_error("copy published at '%s', but source changed; source retained", actual);
            goto done;
        }
        if (unlink(source) != 0) {
            result = -1;
            log_error("copy published at '%s', but source could not be removed: %s", actual, strerror(errno));
            errno = EIO; /* Publication already happened: never retry the entire action. */
            goto done;
        }
    }
    log_info("%s '%s' -> '%s'",
             remove_source                    ? "moved"
             : action->type == ACTION_ARCHIVE ? "archived"
                                              : "copied",
             source, actual);
done: {
    int saved = errno;
    if (output >= 0) {
        close(output);
    }
    close(input);
    unlink(temporary);
    errno = saved;
    return result;
}
}

static long long monotonic_ms(void) {
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time) != 0) {
        return -1;
    }
    return (long long)time.tv_sec * 1000 + time.tv_nsec / 1000000;
}

static void terminate_child(pid_t child) {
    /* The whole process group belongs to this exec action. */
    kill(-child, SIGTERM);
    struct timespec grace = {0, 200000000L};
    while (nanosleep(&grace, &grace) != 0 && errno == EINTR) {
    }
    kill(-child, SIGKILL);
    while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {
    }
}

int action_handle_exec(const action_t *action, const char *root, const char *filename, event_type_t event,
                       int dry_run) {
    char storage[1024];
    char *arguments[FILEWARD_MAX_ARGS + 1];
    int count = action_split_command(action->command, storage, sizeof(storage), arguments);
    if (count < 0) {
        errno = EINVAL;
        return -1;
    }
    char expanded[FILEWARD_MAX_ARGS][PATH_MAX];
    for (int i = 0; i < count; i++) {
        if (action_expand_template(arguments[i], root, filename, event, expanded[i], sizeof(expanded[i])) !=
            0) {
            return -1;
        }
        arguments[i] = expanded[i];
    }
    if (arguments[0][0] != '/') {
        errno = EINVAL;
        return -1;
    }
    if (dry_run) {
        log_info("dry-run: would exec '%s' with %d arguments (timeout %ds)", arguments[0], count - 1,
                 action->timeout_seconds);
        for (int i = 0; i < count; i++) {
            log_info("dry-run: exec argv[%d]: %s", i, arguments[i]);
        }
        return 0;
    }
    long long start = monotonic_ms();
    if (start < 0) {
        return -1;
    }
    posix_spawn_file_actions_t files;
    posix_spawnattr_t attributes;
    int error = posix_spawn_file_actions_init(&files);
    if (error) {
        errno = error;
        return -1;
    }
    error = posix_spawnattr_init(&attributes);
    if (error) {
        posix_spawn_file_actions_destroy(&files);
        errno = error;
        return -1;
    }
    sigset_t empty, defaults;
    sigemptyset(&empty);
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGINT);
    sigaddset(&defaults, SIGTERM);
    sigaddset(&defaults, SIGHUP);
    sigaddset(&defaults, SIGPIPE);
    if (!error) {
        error = posix_spawn_file_actions_addopen(&files, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    }
    if (!error) {
        error = posix_spawn_file_actions_addclosefrom_np(&files, 3);
    }
    if (!error) {
        error = posix_spawnattr_setpgroup(&attributes, 0);
    }
    if (!error) {
        error = posix_spawnattr_setsigmask(&attributes, &empty);
    }
    if (!error) {
        error = posix_spawnattr_setsigdefault(&attributes, &defaults);
    }
    if (!error) {
        error = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK |
                                                          POSIX_SPAWN_SETSIGDEF);
    }
    pid_t child = -1;
    extern char **environ;
    if (!error) {
        error = posix_spawn(&child, arguments[0], &files, &attributes, arguments, environ);
    }
    posix_spawn_file_actions_destroy(&files);
    posix_spawnattr_destroy(&attributes);
    if (error) {
        last_code = "FW_EXEC_START";
        audit_emit("exec_status", "failure", "FW_EXEC_START", error, "executable could not be started", 0, 0,
                   NULL);
        errno = error;
        return -1;
    }
    int status;
    for (;;) {
        siginfo_t info;
        memset(&info, 0, sizeof(info));
        if (waitid(P_PID, (id_t)child, &info, WEXITED | WNOHANG | WNOWAIT) != 0) {
            if (errno == EINTR) {
                continue;
            }
            terminate_child(child);
            return -1;
        }
        if (info.si_pid == child) {
            /* Keep the leader unreaped until group cleanup: its PID cannot be reused. */
            kill(-child, SIGKILL);
            pid_t waited;
            do {
                waited = waitpid(child, &status, 0);
            } while (waited < 0 && errno == EINTR);
            if (waited != child) {
                return -1;
            }
            break;
        }
        long long now = monotonic_ms();
        if (action_cancelled() || now < 0 || now - start >= (long long)action->timeout_seconds * 1000) {
            int saved = action_cancelled() ? ECANCELED : ETIMEDOUT;
            last_code = saved == ECANCELED ? "FW_CANCELLED" : "FW_EXEC_TIMEOUT";
            terminate_child(child);
            errno = saved;
            if (saved == ECANCELED) {
                log_info("exec '%s' cancelled", arguments[0]);
            } else {
                log_error("exec '%s' timed out", arguments[0]);
            }
            return -1;
        }
        if (pause_ms(20) != 0) {
            int saved = errno;
            terminate_child(child);
            errno = saved;
            return -1;
        }
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        audit_exec_status(WIFEXITED(status) ? WEXITSTATUS(status) : -1,
                          WIFSIGNALED(status) ? WTERMSIG(status) : 0);
        last_code = WIFSIGNALED(status) ? "FW_EXEC_SIGNAL" : "FW_EXEC_EXIT";
        char detail[128];
        snprintf(detail, sizeof(detail), "exit_status=%d signal=%d",
                 WIFEXITED(status) ? WEXITSTATUS(status) : -1, WIFSIGNALED(status) ? WTERMSIG(status) : 0);
        audit_emit("exec_status", "failure", WIFSIGNALED(status) ? "FW_EXEC_SIGNAL" : "FW_EXEC_EXIT", 0,
                   detail, 0, 0, NULL);
        log_error("exec '%s' failed (exit %d, signal %d)", arguments[0],
                  WIFEXITED(status) ? WEXITSTATUS(status) : -1, WIFSIGNALED(status) ? WTERMSIG(status) : 0);
        errno = ECHILD;
        return -1;
    }
    audit_exec_status(0, 0);
    log_info("executed '%s'", arguments[0]);
    return 0;
}

static int transient_error(int error) {
    return error == EINTR || error == EAGAIN || error == EBUSY || error == ETXTBSY || error == EMFILE ||
           error == ENFILE;
}

static int file_action(const action_t *action, const char *source, const char *directory,
                       const char *destination) {
    if (ensure_directory(directory) != 0) {
        return -1;
    }
    if (action->type == ACTION_MOVE) {
        struct stat info;
        if (lstat(source, &info) != 0) {
            return -1;
        }
        if (!S_ISREG(info.st_mode) && !S_ISLNK(info.st_mode)) {
            errno = ENOTSUP;
            return -1;
        }
        char actual[PATH_MAX];
        int result = publish(source, destination, action->conflict, actual);
        if (result == 0) {
            log_info("moved '%s' -> '%s'", source, actual);
        }
        if (result >= 0 || errno != EXDEV) {
            return result;
        }
        return copy_or_archive(source, directory, destination, action, 1);
    }
    return copy_or_archive(source, directory, destination, action, 0);
}

int action_handle_log(const action_t *action, const char *watch_path, const char *filename,
                      event_type_t event, int dry_run) {
    if (action->message[0]) {
        log_info("%s: %s", dry_run ? "dry-run: would log" : "action log", action->message);
    } else {
        log_info("%s: %s %s/%s", dry_run ? "dry-run: would log" : "action log",
                 event == EVENT_DELETED      ? "deleted"
                 : event == EVENT_MODIFIED   ? "modified"
                 : event == EVENT_MOVED_TO   ? "moved_to"
                 : event == EVENT_MOVED_FROM ? "moved_from"
                                             : "created",
                 watch_path, filename);
    }
    return 0;
}

int action_handle_file(const action_t *action, const char *watch_path, const char *filename,
                       event_type_t event, int dry_run) {
    char source[PATH_MAX], directory[PATH_MAX], destination[PATH_MAX];
    if (format_path(source, sizeof(source), watch_path, filename) != 0 ||
        action_expand_target(action->target, watch_path, filename, event, directory) != 0) {
        return -1;
    }
    const char *base = strrchr(filename, '/');
    base = base ? base + 1 : filename;
    char archive_name[PATH_MAX];
    if (action->type == ACTION_ARCHIVE) {
        int length = snprintf(archive_name, sizeof(archive_name), "%s.gz", base);
        if (length < 0 || (size_t)length >= sizeof(archive_name)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        base = archive_name;
    }
    if (format_path(destination, sizeof(destination), directory, base) != 0) {
        return -1;
    }
    audit_destination(destination);
    if (dry_run) {
        log_info("dry-run: would %s '%s' -> '%s' (conflict=%s)", action_type_name(action->type), source,
                 destination, conflict_policy_name(action->conflict));
        return 0;
    }
    int result = -1;
    int delay = action->backoff_ms;
    for (int attempt = 0; attempt <= action->retries; attempt++) {
        attempts = attempt + 1;
        result = file_action(action, source, directory, destination);
        int saved = errno;
        if (result >= 0 || !transient_error(saved) || attempt == action->retries || action_cancelled()) {
            break;
        }
        audit_emit("action_retry", "retry", audit_error_code(saved), saved, strerror(saved), attempt + 1, 0,
                   NULL);
        log_warn("%s failed for '%s': %s; retry %d/%d in %dms", action_type_name(action->type), source,
                 strerror(saved), attempt + 1, action->retries, delay);
        if (pause_ms(delay) != 0) {
            return -1;
        }
        delay = delay > 2500 ? 5000 : delay * 2;
    }
    if (result == ACTION_SKIPPED) {
        log_info("skipped '%s': destination exists", source);
    } else if (result < 0 && errno == ECANCELED) {
        log_info("%s cancelled for '%s'", action_type_name(action->type), source);
    } else if (result < 0) {
        log_error("cannot %s '%s' to '%s': %s", action_type_name(action->type), source, destination,
                  strerror(errno));
    }
    return result;
}

int execute_action(const action_t *action, const char *watch_path, const char *filename, event_type_t event,
                   int dry_run) {
    last_code = NULL;
    attempts = 1;
    if (!action || !watch_path || !filename || !safe_relative(filename)) {
        errno = EINVAL;
        return -1;
    }
    if (action_cancelled()) {
        return -1;
    }
    const action_handler_t *handler = action_handler_find(action->type);
    if (!handler) {
        errno = EINVAL;
        return -1;
    }
    return handler->execute(action, watch_path, filename, event, dry_run);
}
