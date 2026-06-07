#ifndef FILEWARD_CONFIG_H
#define FILEWARD_CONFIG_H

#include <linux/limits.h>

#define FILEWARD_MAX_RULES 64
#define FILEWARD_MAX_MESSAGE 256

typedef enum {
    EVENT_CREATED,
    EVENT_MODIFIED,
    EVENT_DELETED,
    EVENT_MOVED_FROM,
    EVENT_MOVED_TO,
    EVENT_UNKNOWN,
} event_type_t;

typedef enum {
    ACTION_LOG,
    ACTION_MOVE,
    ACTION_UNKNOWN,
} action_type_t;

typedef struct {
    action_type_t type;
    char target[PATH_MAX];
    char message[FILEWARD_MAX_MESSAGE];
} action_t;

typedef struct {
    event_type_t event;
    char path_glob[PATH_MAX];
    action_t action;
} rule_t;

typedef struct {
    char watch_path[PATH_MAX];
    rule_t rules[FILEWARD_MAX_RULES];
    int rule_count;
} config_t;

int load_config(const char *path, config_t *config);

#endif
