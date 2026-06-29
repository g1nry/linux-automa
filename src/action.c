#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <fileward/action.h>
#include <fileward/log.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static int expand_tilde(const char *path, char *out, size_t out_size) {
    if (path == NULL || out == NULL) {
        return -1;
    }

    if (path[0] == '~' && (path[1] == '/' || path[1] == '\0')) {
        const char *home = getenv("HOME");
        if (home == NULL) {
            log_error("HOME is not set for tilde expansion");
            return -1;
        }

        if (snprintf(out, out_size, "%s%s", home, path + 1) >= (int)out_size) {
            log_error("expanded path is too long");
            return -1;
        }

        return 0;
    }

    if (strlen(path) >= out_size) {
        log_error("path is too long");
        return -1;
    }

    strcpy(out, path);
    return 0;
}

static const char *event_type_name(event_type_t event) {
    switch (event) {
    case EVENT_CREATED:
        return "created";
    case EVENT_MODIFIED:
        return "modified";
    case EVENT_DELETED:
        return "deleted";
    case EVENT_MOVED_FROM:
        return "moved_from";
    case EVENT_MOVED_TO:
        return "moved_to";
    default:
        return "unknown";
    }
}

static const char *basename_of(const char *path) {
    const char *base = strrchr(path, '/');
    return base ? base + 1 : path;
}

static int ensure_directory(const char *path) {
    if (path == NULL || path[0] == '\0') {
        return -1;
    }

    char buffer[PATH_MAX];
    size_t len = strlen(path);

    if (len == 0 || len >= sizeof(buffer)) {
        return -1;
    }

    strncpy(buffer, path, sizeof(buffer));
    buffer[sizeof(buffer) - 1] = '\0';

    if (buffer[len - 1] == '/') {
        buffer[len - 1] = '\0';
    }

    for (char *p = buffer + 1; *p != '\0'; p++) {
        if (*p != '/') {
            continue;
        }

        *p = '\0';
        if (mkdir(buffer, 0755) != 0 && errno != EEXIST) {
            return -1;
        }
        *p = '/';
    }

    if (mkdir(buffer, 0755) == 0) {
        return 0;
    }

    if (errno == EEXIST) {
        struct stat st;
        if (stat(buffer, &st) == 0 && S_ISDIR(st.st_mode)) {
            return 0;
        }
    }

    return -1;
}

static int backup_existing_file(const char *target_path) {
    char backup_path[PATH_MAX];
    int written = snprintf(backup_path, sizeof(backup_path), "%s.bak", target_path);
    if (written < 0 || (size_t)written >= sizeof(backup_path)) {
        return -1;
    }

    if (rename(target_path, backup_path) == 0) {
        return 0;
    }

    if (errno == ENOENT) {
        return 0;
    }

    return -1;
}

int execute_action(const action_t *action, const char *watch_path, const char *filename, event_type_t event, int dry_run) {
    if (action == NULL || filename == NULL || watch_path == NULL) {
        return -1;
    }

    char source[PATH_MAX];
    char target[PATH_MAX];

    switch (action->type) {
    case ACTION_LOG: {
        if (dry_run) {
            if (action->message[0] != '\0') {
                log_info("dry-run: would log '%s'", action->message);
            } else {
                log_info("dry-run: would log '%s %s/%s'", event_type_name(event), watch_path, filename);
            }
            return 0;
        }

        if (action->message[0] != '\0') {
            log_info("action log: %s", action->message);
        } else {
            log_info("action log: %s %s/%s", event_type_name(event), watch_path, filename);
        }
        return 0;
    }
    case ACTION_MOVE: {
        if (action->target[0] == '\0') {
            log_error("move action target is empty");
            return -1;
        }

        char expanded_target[PATH_MAX];
        if (expand_tilde(action->target, expanded_target, sizeof(expanded_target)) != 0) {
            return -1;
        }

        int written = snprintf(source, sizeof(source), "%s/%s", watch_path, filename);
        if (written < 0 || (size_t)written >= sizeof(source)) {
            log_error("source path is too long");
            return -1;
        }

        const char *base = basename_of(filename);
        char destination[PATH_MAX];
        written = snprintf(destination, sizeof(destination), "%s/%s", expanded_target, base);
        if (written < 0 || (size_t)written >= sizeof(destination)) {
            log_error("destination path is too long");
            return -1;
        }

        if (dry_run) {
            log_info("dry-run: would move '%s' -> '%s'", source, destination);
            return 0;
        }

        if (ensure_directory(expanded_target) != 0) {
            log_error("destination directory could not be created: %s", expanded_target);
            return -1;
        }

        for (int attempt = 0; attempt < 3; attempt++) {
            if (rename(source, destination) == 0) {
                log_info("moved '%s' -> '%s'", source, destination);
                return 0;
            }

            if (errno != EXDEV && errno != EBUSY && errno != EAGAIN) {
                break;
            }

            struct timespec delay = {0, 100000000L};
            nanosleep(&delay, NULL);
        }

        if (access(destination, F_OK) == 0) {
            if (backup_existing_file(destination) != 0) {
                log_error("cannot back up existing target '%s': %s", destination, strerror(errno));
                return -1;
            }
            if (rename(source, destination) == 0) {
                log_info("moved '%s' -> '%s'", source, destination);
                return 0;
            }
        }

        log_error("cannot move '%s' to '%s': %s", source, destination, strerror(errno));
        return -1;
    }
    default:
        log_warn("unknown action type");
        return -1;
    }
}
