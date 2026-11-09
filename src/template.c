#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <fileward/action.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

const char *action_type_name(action_type_t type) {
    switch (type) {
    case ACTION_LOG:
        return "log";
    case ACTION_MOVE:
        return "move";
    case ACTION_COPY:
        return "copy";
    case ACTION_ARCHIVE:
        return "archive";
    case ACTION_EXEC:
        return "exec";
    default:
        return "unknown";
    }
}

const char *conflict_policy_name(conflict_policy_t policy) {
    switch (policy) {
    case CONFLICT_FAIL:
        return "fail";
    case CONFLICT_SKIP:
        return "skip";
    case CONFLICT_RENAME:
        return "rename";
    case CONFLICT_OVERWRITE:
        return "overwrite";
    }
    return "unknown";
}

int action_split_command(const char *text, char *storage, size_t size, char *argv[FILEWARD_MAX_ARGS + 1]) {
    size_t used = 0;
    int count = 0;
    while (*text) {
        while (isspace((unsigned char)*text)) {
            text++;
        }
        if (!*text) {
            break;
        }
        if (count >= FILEWARD_MAX_ARGS || used >= size) {
            return -1;
        }
        argv[count++] = storage + used;
        char quote = 0;
        while (*text && (quote || !isspace((unsigned char)*text))) {
            char c = *text++;
            if ((c == '\'' || c == '"') && (!quote || quote == c)) {
                quote = quote ? 0 : c;
                continue;
            }
            if (c == '\\' && quote != '\'') {
                if (!*text) {
                    return -1;
                }
                c = *text++;
            }
            if (used + 1 >= size) {
                return -1;
            }
            storage[used++] = c;
        }
        if (quote || used >= size) {
            return -1;
        }
        storage[used++] = '\0';
    }
    argv[count] = NULL;
    return count > 0 && argv[0][0] ? count : -1;
}

static const char *event_name(event_type_t event) {
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

static int append(char *out, size_t size, size_t *used, const char *value, size_t length) {
    if (length >= size || *used >= size - length) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(out + *used, value, length);
    *used += length;
    out[*used] = '\0';
    return 0;
}

int action_expand_template(const char *text, const char *root, const char *filename, event_type_t event,
                           char *out, size_t size) {
    if (!size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    out[0] = '\0';
    size_t used = 0;
    const char *base = strrchr(filename, '/');
    base = base ? base + 1 : filename;
    const char *dot = strrchr(base, '.');
    if (dot == base) {
        dot = NULL;
    }
    const char *dir_end = base == filename ? filename : base - 1;
    char date[11];
    time_t now = time(NULL);
    struct tm utc;
    if (gmtime_r(&now, &utc) == NULL || !strftime(date, sizeof(date), "%Y-%m-%d", &utc)) {
        errno = EINVAL;
        return -1;
    }
    for (const char *p = text; *p;) {
        if (p[0] == '{' && p[1] == '{') {
            if (append(out, size, &used, "{", 1)) {
                return -1;
            }
            p += 2;
        } else if (p[0] == '}' && p[1] == '}') {
            if (append(out, size, &used, "}", 1)) {
                return -1;
            }
            p += 2;
        } else if (*p == '{') {
            const char *end = strchr(p, '}');
            if (!end) {
                errno = EINVAL;
                return -1;
            }
            size_t length = (size_t)(end - p - 1);
            const char *value;
            size_t value_length;
#define TOKEN(name) (length == sizeof(name) - 1 && strncmp(p + 1, name, length) == 0)
            if (TOKEN("name")) {
                value = base;
                value_length = strlen(base);
            } else if (TOKEN("stem")) {
                value = base;
                value_length = dot ? (size_t)(dot - base) : strlen(base);
            } else if (TOKEN("ext")) {
                value = dot ? dot + 1 : "";
                value_length = strlen(value);
            } else if (TOKEN("relative")) {
                value = filename;
                value_length = strlen(value);
            } else if (TOKEN("dir")) {
                value = filename;
                value_length = (size_t)(dir_end - filename);
            } else if (TOKEN("root")) {
                value = root;
                value_length = strlen(root);
            } else if (TOKEN("path")) {
                if (append(out, size, &used, root, strlen(root)) || append(out, size, &used, "/", 1) ||
                    append(out, size, &used, filename, strlen(filename))) {
                    return -1;
                }
                p = end + 1;
                continue;
            } else if (TOKEN("event")) {
                value = event_name(event);
                value_length = strlen(value);
            } else if (TOKEN("date")) {
                value = date;
                value_length = strlen(date);
            } else {
                errno = EINVAL;
                return -1;
            }
#undef TOKEN
            if (append(out, size, &used, value, value_length)) {
                return -1;
            }
            p = end + 1;
        } else {
            if (*p == '}') {
                errno = EINVAL;
                return -1;
            }
            if (append(out, size, &used, p++, 1)) {
                return -1;
            }
        }
    }
    return 0;
}

int action_validate_template(const char *text) {
    char buffer[PATH_MAX * 2];
    return action_expand_template(text, "/watch", "dir/example.txt", EVENT_CREATED, buffer, sizeof(buffer));
}
