#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <fileward/state.h>
#include <pthread.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

struct state_store {
    pthread_mutex_t mutex;
    char *path;
};
state_store_t *state_store_create(const char *path) {
    state_store_t *store = calloc(1, sizeof(*store));
    if (!store) {
        return NULL;
    }
    store->path = path ? strdup(path) : NULL;
    if (path && !store->path) {
        free(store);
        return NULL;
    }
    int error = pthread_mutex_init(&store->mutex, NULL);
    if (error) {
        free(store->path);
        free(store);
        errno = error;
        return NULL;
    }
    return store;
}
int state_store_append(state_store_t *store, const char *root, const char *path, const char *event,
                       const char *action) {
    if (!store->path) {
        return 0;
    }
    pthread_mutex_lock(&store->mutex);
    int fd = open(store->path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    int error = fd < 0 ? errno : 0;
    struct stat st;
    if (!error && fstat(fd, &st)) {
        error = errno;
    }
    if (!error && !S_ISREG(st.st_mode)) {
        error = EINVAL;
    }
    char *line = NULL;
    if (!error) {
        char stamp[32];
        struct tm local;
        time_t now = time(NULL);
        localtime_r(&now, &local);
        strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);
        int size = asprintf(&line, "%s\t%s\t%s\t%s\t%s\n", stamp, event, root, path, action);
        if (size < 0) {
            error = ENOMEM;
        } else {
            size_t done = 0;
            while (done < (size_t)size) {
                ssize_t count = write(fd, line + done, (size_t)size - done);
                if (count < 0 && errno == EINTR) {
                    continue;
                }
                if (count <= 0) {
                    error = count == 0 ? EIO : errno;
                    break;
                }
                done += (size_t)count;
            }
        }
    }
    free(line);
    if (fd >= 0 && close(fd) && !error) {
        error = errno;
    }
    pthread_mutex_unlock(&store->mutex);
    errno = error;
    return error ? -1 : 0;
}
void state_store_destroy(state_store_t *store) {
    if (!store) {
        return;
    }
    pthread_mutex_destroy(&store->mutex);
    free(store->path);
    free(store);
}
