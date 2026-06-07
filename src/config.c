#include "config.h"
#include "log.h"

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

        log_warn("config %s:%d: unknown directive: %s", path, line_no, text);
    }

    fclose(file);

    if (config->watch_path[0] == '\0') {
        log_error("config %s does not contain a watch directive", path);
        return -1;
    }

    return 0;
}
