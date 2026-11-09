#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <fileward/control.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define CLIENTS 16
struct client {
    int fd;
    size_t length;
    char request[64];
    long long deadline;
};
struct control_server {
    int fd, lock;
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    dev_t device;
    ino_t inode;
    struct client clients[CLIENTS];
};
static long long milliseconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}
int control_default_path(char *out, size_t size) {
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    char fallback[64];
    if (!runtime || !*runtime) {
        snprintf(fallback, sizeof(fallback), "/run/user/%ld", (long)getuid());
        if (access(fallback, W_OK) == 0) {
            runtime = fallback;
        }
    }
    struct stat directory;
    if (runtime && *runtime &&
        (stat(runtime, &directory) || !S_ISDIR(directory.st_mode) || access(runtime, W_OK))) {
        runtime = NULL;
    }
    int length;
    if (runtime && *runtime) {
        length = snprintf(out, size, "%s/fileward/control.sock", runtime);
    } else {
        const char *state = getenv("XDG_STATE_HOME"), *home = getenv("HOME");
        length = state && *state
                     ? snprintf(out, size, "%s/fileward/control.sock", state)
                     : snprintf(out, size, "%s/.local/state/fileward/control.sock", home ? home : ".");
    }
    if (length < 0 || (size_t)length >= size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}
static int parents(const char *path) {
    char buffer[4096];
    if (strlen(path) >= sizeof(buffer)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(buffer, path);
    for (char *p = buffer + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(buffer, 0700) && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }
    return 0;
}
control_server_t *control_open(const char *path) {
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    if (!*path || strlen(path) >= sizeof(address.sun_path)) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    if (parents(path)) {
        return NULL;
    }
    control_server_t *server = calloc(1, sizeof(*server));
    if (!server) {
        return NULL;
    }
    server->fd = server->lock = -1;
    for (int i = 0; i < CLIENTS; i++) {
        server->clients[i].fd = -1;
    }
    strcpy(server->path, path);
    strcpy(address.sun_path, path);
    char lock[sizeof(address.sun_path) + 8];
    snprintf(lock, sizeof(lock), "%s.lock", path);
    server->lock = open(lock, O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    struct stat st;
    if (server->lock < 0 || fstat(server->lock, &st) || !S_ISREG(st.st_mode) ||
        flock(server->lock, LOCK_EX | LOCK_NB)) {
        goto fail;
    }
    if (lstat(path, &st) == 0) {
        if (!S_ISSOCK(st.st_mode)) {
            errno = EEXIST;
            goto fail;
        }
        int probe = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (probe < 0) {
            goto fail;
        }
        int result = connect(probe, (struct sockaddr *)&address, sizeof(address)), error = errno;
        close(probe);
        if (result == 0 || error != ECONNREFUSED) {
            errno = EADDRINUSE;
            goto fail;
        }
        if (unlink(path)) {
            goto fail;
        }
    } else if (errno != ENOENT) {
        goto fail;
    }
    server->fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (server->fd < 0 || bind(server->fd, (struct sockaddr *)&address, sizeof(address))) {
        goto fail;
    }
    if (lstat(path, &st)) {
        goto fail;
    }
    server->device = st.st_dev;
    server->inode = st.st_ino;
    if (chmod(path, 0600) || listen(server->fd, CLIENTS)) {
        goto fail;
    }
    return server;
fail: {
    int saved = errno ? errno : EINVAL;
    control_close(server);
    errno = saved;
    return NULL;
}
}
void control_close(control_server_t *server) {
    if (!server) {
        return;
    }
    for (int i = 0; i < CLIENTS; i++) {
        if (server->clients[i].fd >= 0) {
            close(server->clients[i].fd);
        }
    }
    if (server->fd >= 0) {
        close(server->fd);
    }
    struct stat st;
    if (server->inode && lstat(server->path, &st) == 0 && st.st_ino == server->inode &&
        st.st_dev == server->device) {
        unlink(server->path);
    }
    if (server->lock >= 0) {
        close(server->lock);
    }
    free(server);
}
void control_poll(control_server_t *server, control_handler_t handler, void *context) {
    for (int budget = 0; budget < CLIENTS; budget++) {
        int fd = accept4(server->fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (fd < 0) {
            break;
        }
        struct ucred peer;
        socklen_t size = sizeof(peer);
        if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) ||
            (peer.uid != getuid() && peer.uid != 0)) {
            close(fd);
            continue;
        }
        int slot;
        for (slot = 0; slot < CLIENTS; slot++) {
            if (server->clients[slot].fd < 0) {
                break;
            }
        }
        if (slot == CLIENTS) {
            close(fd);
            continue;
        }
        server->clients[slot] = (struct client){.fd = fd, .deadline = milliseconds() + 1000};
    }
    for (int i = 0; i < CLIENTS; i++) {
        struct client *client = server->clients + i;
        if (client->fd < 0) {
            continue;
        }
        int done = milliseconds() >= client->deadline;
        if (!done) {
            ssize_t count = recv(client->fd, client->request + client->length,
                                 sizeof(client->request) - 1 - client->length, 0);
            if (count > 0) {
                client->length += (size_t)count;
                client->request[client->length] = 0;
                char *newline = strchr(client->request, '\n');
                if (newline) {
                    *newline = 0;
                    char response[4096];
                    handler(context, client->request, response, sizeof(response));
                    (void)send(client->fd, response, strlen(response), MSG_NOSIGNAL | MSG_DONTWAIT);
                    done = 1;
                } else if (client->length == sizeof(client->request) - 1) {
                    done = 1;
                }
            } else if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                done = 1;
            }
        }
        if (done) {
            close(client->fd);
            client->fd = -1;
        }
    }
}
int control_request(const char *path, const char *request, char *response, size_t size) {
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    if (!*path || strlen(path) >= sizeof(address.sun_path) || size < 2) {
        errno = EINVAL;
        return -1;
    }
    strcpy(address.sun_path, path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        return -1;
    }
    long long deadline = milliseconds() + 3000;
    int result = connect(fd, (struct sockaddr *)&address, sizeof(address));
    if (result && errno != EINPROGRESS && errno != EAGAIN) {
        goto fail;
    }
    struct pollfd pollfd = {.fd = fd, .events = POLLOUT};
    if (poll(&pollfd, 1, 3000) <= 0) {
        errno = ETIMEDOUT;
        goto fail;
    }
    int error = 0;
    socklen_t length = sizeof(error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) || error) {
        if (error) {
            errno = error;
        }
        goto fail;
    }
    char command[64];
    int bytes = snprintf(command, sizeof(command), "%s\n", request);
    if (bytes < 0 || bytes >= (int)sizeof(command) ||
        send(fd, command, (size_t)bytes, MSG_NOSIGNAL) != bytes) {
        goto fail;
    }
    size_t used = 0;
    pollfd.events = POLLIN;
    while (milliseconds() < deadline && used + 1 < size) {
        long long left = deadline - milliseconds();
        if (left <= 0) {
            errno = ETIMEDOUT;
            goto fail;
        }
        int waited = poll(&pollfd, 1, (int)left);
        if (waited < 0 && errno == EINTR) {
            continue;
        }
        if (waited <= 0) {
            errno = ETIMEDOUT;
            goto fail;
        }
        ssize_t count = recv(fd, response + used, size - used - 1, 0);
        if (count > 0) {
            used += (size_t)count;
            response[used] = 0;
            if (strchr(response, '\n')) {
                close(fd);
                return 0;
            }
        } else if (!count) {
            break;
        } else if (errno != EAGAIN && errno != EINTR) {
            goto fail;
        }
    }
    errno = EPROTO;
fail: {
    int saved = errno;
    close(fd);
    errno = saved;
    return -1;
}
}
