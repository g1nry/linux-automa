#include <fileward/action.h>
#include <fileward/log.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

        if (expand_tilde(action->target, target, sizeof(target)) != 0) {
            return -1;
        }

        int written = snprintf(source, sizeof(source), "%s/%s", watch_path, filename);
        if (written < 0 || (size_t)written >= sizeof(source)) {
            log_error("source path is too long");
            return -1;
        }

        const char *base = basename_of(filename);
        written = snprintf(target, sizeof(target), "%s/%s", target, base);
        if (written < 0 || (size_t)written >= sizeof(target)) {
            log_error("destination path is too long");
            return -1;
        }

        if (dry_run) {
            log_info("dry-run: would move '%s' -> '%s'", source, target);
            return 0;
        }

        if (rename(source, target) != 0) {
            log_error("cannot move '%s' to '%s': %s", source, target, strerror(errno));
            return -1;
        }

        log_info("moved '%s' -> '%s'", source, target);
        return 0;
    }
    default:
        log_warn("unknown action type");
        return -1;
    }
}
