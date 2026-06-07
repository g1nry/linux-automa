#include <fileward/config.h>
#include <fileward/log.h>

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static char *trim_whitespace(char *s) {
    char *end;

    while (*s != '\0' && isspace((unsigned char)*s)) {
        s++;
    }

    if (*s == '\0') {
        return s;
    }

    end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) {
        *end-- = '\0';
    }

    return s;
}

static int parse_word(char **text, char *out, size_t out_size) {
    *text = trim_whitespace(*text);
    if (**text == '\0') {
        return -1;
    }

    char *start = *text;
    char *end = start;
    while (*end != '\0' && !isspace((unsigned char)*end)) {
        end++;
    }

    size_t len = (size_t)(end - start);
    if (len >= out_size) {
        return -1;
    }

    memcpy(out, start, len);
    out[len] = '\0';
    *text = end;
    return 0;
}

static event_type_t parse_event_type(const char *token) {
    if (strcmp(token, "created") == 0) {
        return EVENT_CREATED;
    }
    if (strcmp(token, "modified") == 0) {
        return EVENT_MODIFIED;
    }
    if (strcmp(token, "deleted") == 0) {
        return EVENT_DELETED;
    }
    if (strcmp(token, "moved_from") == 0) {
        return EVENT_MOVED_FROM;
    }
    if (strcmp(token, "moved_to") == 0) {
        return EVENT_MOVED_TO;
    }
    return EVENT_UNKNOWN;
}

static action_type_t parse_action_type(const char *token) {
    if (strcmp(token, "log") == 0) {
        return ACTION_LOG;
    }
    if (strcmp(token, "move") == 0) {
        return ACTION_MOVE;
    }
    return ACTION_UNKNOWN;
}

static int read_optional_message(char **text, char *buffer, size_t buffer_size) {
    *text = trim_whitespace(*text);
    if (**text == '\0') {
        buffer[0] = '\0';
        return 0;
    }

    if (**text == '"') {
        (*text)++;
        char *end = strchr(*text, '"');
        if (end == NULL) {
            return -1;
        }

        size_t len = (size_t)(end - *text);
        if (len >= buffer_size) {
            return -1;
        }

        memcpy(buffer, *text, len);
        buffer[len] = '\0';
        *text = end + 1;
        return 0;
    }

    char *trimmed = trim_whitespace(*text);
    size_t len = strlen(trimmed);
    if (len >= buffer_size) {
        return -1;
    }

    strcpy(buffer, trimmed);
    *text += len;
    return 0;
}

int load_config(const char *path, config_t *config) {
    if (path == NULL || config == NULL) {
        log_error("invalid config arguments");
        return -1;
    }

    FILE *file = fopen(path, "r");
    if (file == NULL) {
        log_error("cannot open config '%s': %s", path, strerror(errno));
        return -1;
    }

    config->watch_path[0] = '\0';
    config->rule_count = 0;

    char line[1024];
    int line_no = 0;

    while (fgets(line, sizeof(line), file) != NULL) {
        line_no++;
        char *text = trim_whitespace(line);

        if (*text == '\0' || *text == '#') {
            continue;
        }

        if (strncmp(text, "watch ", 6) == 0) {
            text += 6;
            text = trim_whitespace(text);

            if (*text == '\0') {
                log_error("config %s:%d: watch path is empty", path, line_no);
                fclose(file);
                return -1;
            }

            if (config->watch_path[0] != '\0') {
                log_warn("config %s:%d: multiple watch directives, ignoring", path, line_no);
                continue;
            }

            if (strlen(text) >= sizeof(config->watch_path)) {
                log_error("config %s:%d: watch path is too long", path, line_no);
                fclose(file);
                return -1;
            }

            strcpy(config->watch_path, text);
            continue;
        }

        if (strncmp(text, "when ", 5) == 0) {
            if (config->rule_count >= FILEWARD_MAX_RULES) {
                log_error("config %s:%d: too many rules", path, line_no);
                fclose(file);
                return -1;
            }

            text += 5;
            char event_token[32];
            char path_glob[PATH_MAX];
            char action_token[32];
            rule_t *rule = &config->rules[config->rule_count];

            if (parse_word(&text, event_token, sizeof(event_token)) != 0 ||
                parse_word(&text, path_glob, sizeof(path_glob)) != 0 ||
                parse_word(&text, action_token, sizeof(action_token)) != 0) {
                log_error("config %s:%d: invalid when directive", path, line_no);
                fclose(file);
                return -1;
            }

            rule->event = parse_event_type(event_token);
            if (rule->event == EVENT_UNKNOWN) {
                log_error("config %s:%d: unknown event type '%s'", path, line_no, event_token);
                fclose(file);
                return -1;
            }

            strcpy(rule->path_glob, path_glob);
            rule->action.type = parse_action_type(action_token);
            rule->action.target[0] = '\0';
            rule->action.message[0] = '\0';

            if (rule->action.type == ACTION_LOG) {
                if (read_optional_message(&text, rule->action.message, sizeof(rule->action.message)) != 0) {
                    log_error("config %s:%d: invalid log message", path, line_no);
                    fclose(file);
                    return -1;
                }
            } else if (rule->action.type == ACTION_MOVE) {
                if (parse_word(&text, rule->action.target, sizeof(rule->action.target)) != 0) {
                    log_error("config %s:%d: move action requires a target path", path, line_no);
                    fclose(file);
                    return -1;
                }
            } else {
                log_error("config %s:%d: unknown action '%s'", path, line_no, action_token);
                fclose(file);
                return -1;
            }

            config->rule_count++;
            continue;
        }

        log_warn("config %s:%d: unknown directive: %s", path, line_no, text);
    }

    fclose(file);

    if (config->watch_path[0] == '\0') {
        log_error("config %s does not contain a watch directive", path);
        return -1;
    }

    return 0;
}
