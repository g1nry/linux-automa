#include "watcher.h"
#include "log.h"

#include <errno.h>
#include <linux/limits.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>

#define MAX_EVENTS 1024
#define EVENT_SIZE (sizeof(struct inotify_event))
#define EVENT_BUF_LEN (MAX_EVENTS * (EVENT_SIZE + NAME_MAX + 1))

static int inotify_fd = -1;
static int watch_fd = -1;
static char watched_path[PATH_MAX];

static const char *event_type_from_mask(uint32_t mask) {
    if (mask & IN_CREATE) {
        return "created";
    }

    if (mask & IN_MODIFY) {
        return "modified";
    }

    if (mask & IN_DELETE) {
        return "deleted";
    }

    if (mask & IN_MOVED_FROM) {
        return "moved_from";
    }

    if (mask & IN_MOVED_TO) {
        return "moved_to";
    }

    return "unknown";
}

static void print_event(const struct inotify_event *event) {
    char full_path[PATH_MAX];
    const char *event_type = event_type_from_mask(event->mask);

    if (event->len > 0 && event->name[0] != '\0') {
        snprintf(full_path, sizeof(full_path), "%s/%s", watched_path, event->name);
    } else {
        snprintf(full_path, sizeof(full_path), "%s", watched_path);
    }

    printf("[%s] %s\n", event_type, full_path);
    fflush(stdout);
}

int start_watcher(const char *path) {
    if (path == NULL || path[0] == '\0') {
        log_error("watch path is empty");
        return -1;
    }

    inotify_fd = inotify_init();
    if (inotify_fd == -1) {
        log_error("inotify_init failed: %s", strerror(errno));
        return -1;
    }

    watch_fd = inotify_add_watch(
        inotify_fd,
        path,
        IN_CREATE | IN_MODIFY | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO
    );

    if (watch_fd == -1) {
        log_error("cannot watch '%s': %s", path, strerror(errno));
        close(inotify_fd);
        inotify_fd = -1;
        return -1;
    }

    snprintf(watched_path, sizeof(watched_path), "%s", path);
    log_info("watching directory: %s", watched_path);

    return 0;
}

int watcher_process_events(int timeout_ms) {
    char buffer[EVENT_BUF_LEN];
    struct pollfd poll_fd;
    ssize_t bytes_read;
    size_t offset = 0;

    if (inotify_fd == -1) {
        log_error("watcher is not started");
        return -1;
    }

    poll_fd.fd = inotify_fd;
    poll_fd.events = POLLIN;
    poll_fd.revents = 0;

    int poll_result = poll(&poll_fd, 1, timeout_ms);
    if (poll_result == -1) {
        if (errno == EINTR) {
            return 0;
        }

        log_error("poll failed: %s", strerror(errno));
        return -1;
    }

    if (poll_result == 0) {
        return 0;
    }

    bytes_read = read(inotify_fd, buffer, sizeof(buffer));
    if (bytes_read == -1) {
        if (errno == EINTR) {
            return 0;
        }

        log_error("read failed: %s", strerror(errno));
        return -1;
    }

    while (offset < (size_t)bytes_read) {
        const struct inotify_event *event = (const struct inotify_event *)(buffer + offset);

        print_event(event);
        offset += EVENT_SIZE + event->len;
    }

    return 0;
}

void stop_watcher(void) {
    if (inotify_fd != -1 && watch_fd != -1) {
        inotify_rm_watch(inotify_fd, watch_fd);
        watch_fd = -1;
    }

    if (inotify_fd != -1) {
        close(inotify_fd);
        inotify_fd = -1;
    }

    log_info("watcher stopped");
}
