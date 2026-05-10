#include "watcher.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#define MAX_EVENTS 1024
#define EVENT_SIZE (sizeof(struct inotify_event))
#define BUF_LEN (MAX_EVENTS * (EVENT_SIZE + 16))

static int fd = -1;
static int wd = -1;

int start_watcher(const char *path) {
    fd = inotify_init();
    if (fd == -1) {
        log_error("inotify_init failed: %s", strerror(errno));
        return -1;
    }

    wd = inotify_add_watch(fd, path, IN_CREATE | IN_MODIFY | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO);
    if (wd == -1) {
        log_error("Cannot watch '%s': %s", path, strerror(errno));
        close(fd);
        return -1;
    }

    log_info("Watching directory: %s", path);
    return 0;
}

void stop_watcher(void) {
    if (wd != -1) close(wd);
    if (fd != -1) close(fd);
    log_info("Watcher stopped");
}