#include <fileward/config.h>
#include <fileward/action.h>
#include <fileward/log.h>

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
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

static int expand_tilde_path(const char *path, char *out, size_t out_size) {
    if (path == NULL || out == NULL || out_size == 0) {
        return -1;
    }

    if (path[0] == '~' && (path[1] == '/' || path[1] == '\0')) {
        const char *home = getenv("HOME");
        if (home == NULL) {
            return -1;
        }

        if (snprintf(out, out_size, "%s%s", home, path + 1) >= (int)out_size) {
            return -1;
        }
        return 0;
    }

    if (strlen(path) >= out_size) {
        return -1;
    }

    strcpy(out, path);
    return 0;
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
    if (strcmp(token, "saved") == 0) {
        return EVENT_SAVED;
    }
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
    const action_handler_t *handler = action_handler_named(token);
    return handler ? handler->type : ACTION_UNKNOWN;
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

static int valid_rule_name(const char *name) {
    if (*name == '\0') {
        return 0;
    }
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        if (!isalnum(*p) && *p != '_' && *p != '-' && *p != '.') {
            return 0;
        }
    }
    return 1;
}

/* Integer quantities only; cap at INT64_MAX so stat/time comparisons stay meaningful. */
static int parse_quantity(const char *text, int age, uint64_t *value) {
    if (!isdigit((unsigned char)*text)) {
        return -1;
    }
    errno = 0;
    char *end;
    unsigned long long number = strtoull(text, &end, 10);
    if (errno != 0) {
        return -1;
    }
    uint64_t multiplier = 1;
    if (age) {
        if (*end == '\0' || strcmp(end, "s") == 0) {
            multiplier = 1;
        } else if (strcmp(end, "m") == 0) {
            multiplier = 60;
        } else if (strcmp(end, "h") == 0) {
            multiplier = 3600;
        } else if (strcmp(end, "d") == 0) {
            multiplier = 86400;
        } else {
            return -1;
        }
    } else {
        if (*end == '\0' || strcmp(end, "B") == 0) {
            multiplier = 1;
        } else if (strcmp(end, "KiB") == 0) {
            multiplier = 1024;
        } else if (strcmp(end, "MiB") == 0) {
            multiplier = 1024 * 1024;
        } else if (strcmp(end, "GiB") == 0) {
            multiplier = 1024ULL * 1024 * 1024;
        } else {
            return -1;
        }
    }
    if (number > (uint64_t)INT64_MAX / multiplier) {
        return -1;
    }
    *value = (uint64_t)number * multiplier;
    return 0;
}

static int parse_rule_options(char **text, rule_t *rule) {
    unsigned seen = 0;
    char token[512];
    while (parse_word(text, token, sizeof(token)) == 0) {
        if (strcmp(token, "when") == 0) {
            if ((rule->has_min_size && rule->has_max_size && rule->min_size > rule->max_size) ||
                (rule->has_min_age && rule->has_max_age && rule->min_age > rule->max_age)) {
                return -1;
            }
            return 0;
        }
        unsigned bit;
        if (strcmp(token, "enabled") == 0 || strcmp(token, "disabled") == 0) {
            bit = 1u;
            rule->enabled = strcmp(token, "enabled") == 0;
        } else if (strcmp(token, "stop") == 0) {
            bit = 2u;
            rule->stop = 1;
        } else {
            char *value = strchr(token, '=');
            if (value == NULL || value[1] == '\0') {
                return -1;
            }
            *value++ = '\0';
            if (strcmp(token, "priority") == 0) {
                bit = 4u;
                char *end;
                errno = 0;
                long priority = strtol(value, &end, 10);
                if (end == value || *end != '\0' || errno || priority < INT_MIN || priority > INT_MAX) {
                    return -1;
                }
                rule->priority = (int)priority;
            } else if (strcmp(token, "name") == 0) {
                bit = 8u;
                if (strlen(value) >= sizeof(rule->name_glob)) {
                    return -1;
                }
                strcpy(rule->name_glob, value);
            } else if (strcmp(token, "extension") == 0) {
                bit = 16u;
                if (*value == '.') {
                    value++;
                }
                if (*value == '\0' || strlen(value) >= sizeof(rule->extension) ||
                    strpbrk(value, "/.*?[]\\")) {
                    return -1;
                }
                strcpy(rule->extension, value);
            } else if (strcmp(token, "type") == 0) {
                bit = 32u;
                if (strcmp(value, "file") == 0) {
                    rule->object_type = OBJECT_FILE;
                } else if (strcmp(value, "directory") == 0) {
                    rule->object_type = OBJECT_DIRECTORY;
                } else if (strcmp(value, "symlink") == 0) {
                    rule->object_type = OBJECT_SYMLINK;
                } else if (strcmp(value, "any") == 0) {
                    rule->object_type = OBJECT_ANY;
                } else {
                    return -1;
                }
            } else if (strcmp(token, "min-size") == 0) {
                bit = 64u;
                rule->has_min_size = 1;
                if (parse_quantity(value, 0, &rule->min_size) != 0) {
                    return -1;
                }
            } else if (strcmp(token, "max-size") == 0) {
                bit = 128u;
                rule->has_max_size = 1;
                if (parse_quantity(value, 0, &rule->max_size) != 0) {
                    return -1;
                }
            } else if (strcmp(token, "min-age") == 0) {
                bit = 256u;
                rule->has_min_age = 1;
                if (parse_quantity(value, 1, &rule->min_age) != 0) {
                    return -1;
                }
            } else if (strcmp(token, "max-age") == 0) {
                bit = 512u;
                rule->has_max_age = 1;
                if (parse_quantity(value, 1, &rule->max_age) != 0) {
                    return -1;
                }
            } else if (strcmp(token, "conflict") == 0) {
                bit = 1024u;
                if (strcmp(value, "fail") == 0) {
                    rule->action.conflict = CONFLICT_FAIL;
                } else if (strcmp(value, "skip") == 0) {
                    rule->action.conflict = CONFLICT_SKIP;
                } else if (strcmp(value, "rename") == 0) {
                    rule->action.conflict = CONFLICT_RENAME;
                } else if (strcmp(value, "overwrite") == 0) {
                    rule->action.conflict = CONFLICT_OVERWRITE;
                } else {
                    return -1;
                }
                rule->action.option_flags |= bit;
            } else if (strcmp(token, "retries") == 0) {
                bit = 2048u;
                uint64_t number;
                if (parse_quantity(value, 0, &number) != 0 || strspn(value, "0123456789") != strlen(value) ||
                    number > 5) {
                    return -1;
                }
                rule->action.retries = (int)number;
                rule->action.option_flags |= bit;
            } else if (strcmp(token, "backoff") == 0) {
                bit = 4096u;
                char *end;
                errno = 0;
                long number = strtol(value, &end, 10);
                if (end == value || errno || strcmp(end, "ms") != 0 || number < 1 || number > 5000) {
                    return -1;
                }
                rule->action.backoff_ms = (int)number;
                rule->action.option_flags |= bit;
            } else if (strcmp(token, "timeout") == 0) {
                bit = 8192u;
                uint64_t number;
                if (parse_quantity(value, 1, &number) != 0 || number < 1 || number > 300) {
                    return -1;
                }
                rule->action.timeout_seconds = (int)number;
                rule->action.option_flags |= bit;
            } else {
                return -1;
            }
        }
        if (seen & bit) {
            return -1;
        }
        seen |= bit;
    }
    return -1;
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
    config->filter_count = 0;
    config->first_match = 0;
    int mode_seen = 0;

    char line[1024];
    int line_no = 0;

    while (fgets(line, sizeof(line), file) != NULL) {
        line_no++;
        if (strchr(line, '\n') == NULL && !feof(file)) {
            int next = fgetc(file);
            if (next != EOF && next != '\n') {
                log_error("config %s:%d: line is too long", path, line_no);
                fclose(file);
                return -1;
            }
        }
        char *text = trim_whitespace(line);

        if (*text == '\0' || *text == '#') {
            continue;
        }

        if (strncmp(text, "watch", 5) == 0 && (text[5] == '\0' || isspace((unsigned char)text[5]))) {
            text += 5;
            text = trim_whitespace(text);

            if (*text == '\0') {
                log_error("config %s:%d: watch path is empty", path, line_no);
                fclose(file);
                return -1;
            }

            if (config->watch_path[0] != '\0') {
                log_error("config %s:%d: multiple watch directives", path, line_no);
                fclose(file);
                return -1;
            }

            char expanded_watch_path[PATH_MAX];
            if (expand_tilde_path(text, expanded_watch_path, sizeof(expanded_watch_path)) != 0) {
                log_error("config %s:%d: watch path is too long or invalid", path, line_no);
                fclose(file);
                return -1;
            }

            if (strlen(expanded_watch_path) >= sizeof(config->watch_path)) {
                log_error("config %s:%d: watch path is too long", path, line_no);
                fclose(file);
                return -1;
            }

            strcpy(config->watch_path, expanded_watch_path);
            continue;
        }

        char directive[32];
        if (parse_word(&text, directive, sizeof(directive)) != 0) {
            log_error("config %s:%d: invalid directive", path, line_no);
            fclose(file);
            return -1;
        }
        if (strcmp(directive, "mode") == 0) {
            char mode[16];
            if (mode_seen || parse_word(&text, mode, sizeof(mode)) != 0 ||
                (strcmp(mode, "all") != 0 && strcmp(mode, "first") != 0) || *trim_whitespace(text)) {
                log_error("config %s:%d: expected a single 'mode all' or 'mode first' directive", path,
                          line_no);
                fclose(file);
                return -1;
            }
            config->first_match = strcmp(mode, "first") == 0;
            mode_seen = 1;
            continue;
        }
        if (strcmp(directive, "ignore") == 0 || strcmp(directive, "allow") == 0 ||
            strcmp(directive, "deny") == 0) {
            if (config->filter_count >= FILEWARD_MAX_FILTERS) {
                log_error("config %s:%d: too many path filters", path, line_no);
                fclose(file);
                return -1;
            }
            path_filter_t *filter = &config->filters[config->filter_count];
            if (parse_word(&text, filter->pattern, sizeof(filter->pattern)) != 0 || *trim_whitespace(text)) {
                log_error("config %s:%d: path filter requires exactly one pattern", path, line_no);
                fclose(file);
                return -1;
            }
            filter->type = strcmp(directive, "ignore") == 0  ? FILTER_IGNORE
                           : strcmp(directive, "allow") == 0 ? FILTER_ALLOW
                                                             : FILTER_DENY;
            config->filter_count++;
            continue;
        }
        if (strcmp(directive, "when") == 0 || strcmp(directive, "rule") == 0) {
            if (config->rule_count >= FILEWARD_MAX_RULES) {
                log_error("config %s:%d: too many rules", path, line_no);
                fclose(file);
                return -1;
            }

            char event_token[32];
            char path_glob[PATH_MAX];
            char action_token[32];
            rule_t *rule = &config->rules[config->rule_count];
            memset(rule, 0, sizeof(*rule));
            rule->enabled = 1;
            rule->action.retries = 2;
            rule->action.backoff_ms = 100;
            rule->action.timeout_seconds = 30;
            rule->line_no = line_no;
            snprintf(rule->name, sizeof(rule->name), "@line-%d", line_no);
            if (strcmp(directive, "rule") == 0) {
                if (parse_word(&text, rule->name, sizeof(rule->name)) != 0 || !valid_rule_name(rule->name)) {
                    log_error("config %s:%d: invalid rule name (use letters, digits, _, - or .)", path,
                              line_no);
                    fclose(file);
                    return -1;
                }
                for (int i = 0; i < config->rule_count; i++) {
                    if (strcmp(config->rules[i].name, rule->name) == 0) {
                        log_error("config %s:%d: duplicate rule name '%s'", path, line_no, rule->name);
                        fclose(file);
                        return -1;
                    }
                }
                if (parse_rule_options(&text, rule) != 0) {
                    log_error("config %s:%d: invalid, duplicate or contradictory rule option; expected "
                              "options followed by 'when'",
                              path, line_no);
                    fclose(file);
                    return -1;
                }
            }

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

            const action_handler_t *handler = action_handler_find(rule->action.type);
            unsigned capabilities = handler ? handler->capabilities : 0;
            if (capabilities & ACTION_MESSAGE) {
                if (read_optional_message(&text, rule->action.message, sizeof(rule->action.message)) != 0) {
                    log_error("config %s:%d: invalid log message", path, line_no);
                    fclose(file);
                    return -1;
                }
            } else if (capabilities & (ACTION_TARGET | ACTION_COMMAND)) {
                char storage[1024];
                char *arguments[FILEWARD_MAX_ARGS + 1];
                int count = action_split_command(text, storage, sizeof(storage), arguments);
                if (capabilities & ACTION_COMMAND) {
                    if (count < 1 || arguments[0][0] != '/' || strchr(arguments[0], '{') ||
                        strchr(arguments[0], '}') ||
                        strlen(trim_whitespace(text)) >= sizeof(rule->action.command)) {
                        log_error("config %s:%d: exec requires a literal absolute executable and valid "
                                  "quoted arguments",
                                  path, line_no);
                        fclose(file);
                        return -1;
                    }
                    for (int i = 0; i < count; i++) {
                        if (action_validate_template(arguments[i]) != 0) {
                            log_error("config %s:%d: invalid exec argument template", path, line_no);
                            fclose(file);
                            return -1;
                        }
                    }
                    strcpy(rule->action.command, trim_whitespace(text));
                } else {
                    if (count != 1 || arguments[0][0] == '\0' ||
                        strlen(arguments[0]) >= sizeof(rule->action.target) ||
                        action_validate_template(arguments[0]) != 0) {
                        log_error("config %s:%d: %s requires one destination directory with valid templates",
                                  path, line_no, action_token);
                        fclose(file);
                        return -1;
                    }
                    strcpy(rule->action.target, arguments[0]);
                }
                text += strlen(text);
            } else {
                log_error("config %s:%d: unknown action '%s'", path, line_no, action_token);
                fclose(file);
                return -1;
            }
            unsigned file_options = 1024u | 2048u | 4096u;
            if (((rule->action.option_flags & file_options) && !(capabilities & ACTION_FILE_OPTIONS)) ||
                ((rule->action.option_flags & 8192u) && !(capabilities & ACTION_TIMEOUT))) {
                log_error("config %s:%d: action options do not apply to %s", path, line_no, action_token);
                fclose(file);
                return -1;
            }

            if (*trim_whitespace(text) != '\0') {
                log_error("config %s:%d: unexpected trailing arguments", path, line_no);
                fclose(file);
                return -1;
            }
            config->rule_count++;
            continue;
        }

        log_error("config %s:%d: unknown directive: %s", path, line_no, directive);
        fclose(file);
        return -1;
    }

    if (ferror(file)) {
        log_error("cannot read config '%s': %s", path, strerror(errno));
        fclose(file);
        return -1;
    }
    fclose(file);

    if (config->watch_path[0] == '\0') {
        log_error("config %s does not contain a watch directive", path);
        return -1;
    }

    return 0;
}
