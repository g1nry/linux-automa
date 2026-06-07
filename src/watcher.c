#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <fileward/watcher.h>
#include <fileward/log.h>

#include <dirent.h>
#include <errno.h>
#include <linux/limits.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_EVENTS 1024
#define EVENT_SIZE (sizeof(struct inotify_event))
#define EVENT_BUF_LEN (MAX_EVENTS * (EVENT_SIZE + NAME_MAX + 1))
#define MAX_WATCHES 512

typedef struct {
    int wd;
    char path[PATH_MAX];
} watch_entry_t;

static int inotify_fd = -1;
static char watched_path[PATH_MAX];
static watch_entry_t watch_entries[MAX_WATCHES];
static int watch_count = 0;

static event_type_t event_type_from_mask(uint32_t mask) {
    if (mask & IN_CREATE) {
        return EVENT_CREATED;
    }

    if (mask & IN_MODIFY) {
        return EVENT_MODIFIED;
    }

    if (mask & IN_DELETE) {
        return EVENT_DELETED;
    }

    if (mask & IN_MOVED_FROM) {
        return EVENT_MOVED_FROM;
    }

    if (mask & IN_MOVED_TO) {
        return EVENT_MOVED_TO;
    }

    return EVENT_UNKNOWN;
}

static int add_watch_entry(int wd, const char *path) {
    if (watch_count >= MAX_WATCHES) {
        log_error("too many watched directories");
        return -1;
    }

    watch_entries[watch_count].wd = wd;
    strncpy(watch_entries[watch_count].path, path, sizeof(watch_entries[watch_count].path));
    watch_entries[watch_count].path[sizeof(watch_entries[watch_count].path) - 1] = '\0';
    watch_count++;
    return 0;
}

static void remove_watch_entry(int wd) {
    for (int i = 0; i < watch_count; i++) {
        if (watch_entries[i].wd == wd) {
            watch_entries[i] = watch_entries[watch_count - 1];
            watch_count--;
            return;
        }
    }
}

static const char *watch_path_for_wd(int wd) {
    for (int i = 0; i < watch_count; i++) {
        if (watch_entries[i].wd == wd) {
            return watch_entries[i].path;
        }
    }
    return NULL;
}

static int add_directory_watch(const char *path) {
    int wd = inotify_add_watch(
        inotify_fd,
        path,
        IN_CREATE | IN_MODIFY | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_ONLYDIR
    );

    if (wd == -1) {
        log_warn("cannot watch directory '%s': %s", path, strerror(errno));
        return -1;
    }

    if (add_watch_entry(wd, path) != 0) {
        inotify_rm_watch(inotify_fd, wd);
        return -1;
    }

    log_info("watching directory: %s", path);
    return 0;
}

static int add_recursive_watches(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        log_warn("cannot stat '%s': %s", path, strerror(errno));
        return -1;
    }

    if (!S_ISDIR(st.st_mode)) {
        log_warn("path is not a directory: %s", path);
        return -1;
    }

    if (add_directory_watch(path) != 0) {
        return -1;
    }

    DIR *dir = opendir(path);
    if (dir == NULL) {
        log_warn("cannot open directory '%s': %s", path, strerror(errno));
        return 0;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char child_path[PATH_MAX];
        int written = snprintf(child_path, sizeof(child_path), "%s/%s", path, entry->d_name);
        if (written < 0 || (size_t)written >= sizeof(child_path)) {
            continue;
        }

        if (entry->d_type == DT_DIR) {
            add_recursive_watches(child_path);
        } else if (entry->d_type == DT_UNKNOWN) {
            struct stat child_st;
            if (stat(child_path, &child_st) == 0 && S_ISDIR(child_st.st_mode)) {
                add_recursive_watches(child_path);
            }
        }
    }

    closedir(dir);
    return 0;
}

static int build_relative_path(int wd, const char *name, char *out, size_t out_size) {
    const char *base = watch_path_for_wd(wd);
    if (base == NULL) {
        return -1;
    }

    size_t root_len = strlen(watched_path);
    const char *relative_base = base;
    if (strncmp(base, watched_path, root_len) == 0 && (base[root_len] == '/' || base[root_len] == '\0')) {
        relative_base = base + root_len;
        if (*relative_base == '/') {
            relative_base++;
        }
    }

    if (*relative_base == '\0') {
        if (snprintf(out, out_size, "%s", name) >= (int)out_size) {
            return -1;
        }
    } else {
        if (snprintf(out, out_size, "%s/%s", relative_base, name) >= (int)out_size) {
            return -1;
        }
    }

    return 0;
}

int start_watcher(const char *path) {
    int written;

    if (path == NULL || path[0] == '\0') {
        log_error("watch path is empty");
        return -1;
    }

    written = snprintf(watched_path, sizeof(watched_path), "%s", path);
    if (written < 0 || (size_t)written >= sizeof(watched_path)) {
        log_error("watch path is too long");
        return -1;
    }

    size_t len = strlen(watched_path);
    if (len > 1 && watched_path[len - 1] == '/') {
        watched_path[len - 1] = '\0';
    }

    inotify_fd = inotify_init();
    if (inotify_fd == -1) {
        log_error("inotify_init failed: %s", strerror(errno));
        return -1;
    }

    watch_count = 0;

    if (add_recursive_watches(watched_path) != 0) {
        close(inotify_fd);
        inotify_fd = -1;
        return -1;
    }

    return 0;
}

int watcher_process_events(int timeout_ms, int (*callback)(const file_event_t *, void *), void *user_data) {
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
        file_event_t file_event;

        file_event.type = event_type_from_mask(event->mask);
        file_event.filename[0] = '\0';

        if (event->len > 0 && event->name[0] != '\0') {
            if (build_relative_path(event->wd, event->name, file_event.filename, sizeof(file_event.filename)) != 0) {
                file_event.filename[0] = '\0';
            }
        }

        if (event->mask & IN_ISDIR) {
            if ((event->mask & IN_CREATE) || (event->mask & IN_MOVED_TO)) {
                const char *base = watch_path_for_wd(event->wd);
                if (base != NULL && event->len > 0 && event->name[0] != '\0') {
                    char child_path[PATH_MAX];
                    int written = snprintf(child_path, sizeof(child_path), "%s/%s", base, event->name);
                    if (written >= 0 && (size_t)written < sizeof(child_path)) {
                        add_recursive_watches(child_path);
                    }
                }
            }

            if (event->mask & IN_IGNORED) {
                remove_watch_entry(event->wd);
            }
        }

        if (event->mask & IN_IGNORED) {
            remove_watch_entry(event->wd);
        }

        if (callback != NULL) {
            callback(&file_event, user_data);
        }

        offset += EVENT_SIZE + event->len;
    }

    return 0;
}

void stop_watcher(void) {
    if (inotify_fd != -1) {
        for (int i = 0; i < watch_count; i++) {
            inotify_rm_watch(inotify_fd, watch_entries[i].wd);
        }
        close(inotify_fd);
        inotify_fd = -1;
        watch_count = 0;
    }

    log_info("watcher stopped");
}
