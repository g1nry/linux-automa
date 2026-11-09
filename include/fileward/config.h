#ifndef FILEWARD_CONFIG_H
#define FILEWARD_CONFIG_H

#include <linux/limits.h>
#include <stdint.h>

#define FILEWARD_MAX_RULES 64
#define FILEWARD_MAX_MESSAGE 256
#define FILEWARD_MAX_RULE_NAME 64
#define FILEWARD_MAX_FILTERS 64

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
    ACTION_COPY,
    ACTION_ARCHIVE,
    ACTION_EXEC,
    ACTION_UNKNOWN,
} action_type_t;

typedef enum {
    CONFLICT_FAIL,
    CONFLICT_SKIP,
    CONFLICT_RENAME,
    CONFLICT_OVERWRITE,
} conflict_policy_t;

typedef struct {
    action_type_t type;
    conflict_policy_t conflict;
    unsigned option_flags;
    int retries;
    int backoff_ms;
    int timeout_seconds;
    char command[1024];
    char target[PATH_MAX];
    char message[FILEWARD_MAX_MESSAGE];
} action_t;

typedef enum {
    OBJECT_ANY,
    OBJECT_FILE,
    OBJECT_DIRECTORY,
    OBJECT_SYMLINK,
} object_type_t;

typedef enum {
    FILTER_IGNORE,
    FILTER_ALLOW,
    FILTER_DENY,
} filter_type_t;

typedef struct {
    filter_type_t type;
    char pattern[PATH_MAX];
} path_filter_t;

typedef struct {
    char name[FILEWARD_MAX_RULE_NAME];
    int enabled;
    int line_no;
    int priority;
    int stop;
    char name_glob[256];
    char extension[64];
    object_type_t object_type;
    int has_min_size, has_max_size, has_min_age, has_max_age;
    uint64_t min_size, max_size, min_age, max_age;
    event_type_t event;
    char path_glob[PATH_MAX];
    action_t action;
} rule_t;

typedef struct {
    char watch_path[PATH_MAX];
    rule_t rules[FILEWARD_MAX_RULES];
    int rule_count;
    int first_match;
    path_filter_t filters[FILEWARD_MAX_FILTERS];
    int filter_count;
} config_t;

int load_config(const char *path, config_t *config);

#endif
