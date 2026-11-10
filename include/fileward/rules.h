#ifndef FILEWARD_RULES_H
#define FILEWARD_RULES_H

#include <fileward/config.h>
#include <sys/stat.h>
#include <time.h>

typedef enum {
    RULE_MATCH,
    RULE_DISABLED,
    RULE_EVENT_MISMATCH,
    RULE_PATTERN_MISMATCH,
    RULE_NAME_MISMATCH,
    RULE_EXTENSION_MISMATCH,
    RULE_METADATA_UNAVAILABLE,
    RULE_TYPE_MISMATCH,
    RULE_TOO_SMALL,
    RULE_TOO_LARGE,
    RULE_TOO_YOUNG,
    RULE_TOO_OLD,
    RULE_IGNORED,
    RULE_DENIED,
    RULE_NOT_ALLOWED,
} rule_result_t;

typedef struct {
    const char *path;
    const char *basename, *extension;
    struct stat metadata;
    int metadata_valid;
    time_t now;
} rule_input_t;

/* Snapshot once before executing any actions; removal events have no metadata. */
void prepare_rule_input(const char *root, const char *path, event_type_t event, rule_input_t *input);
rule_result_t evaluate_filters(const config_t *config, const char *path);
rule_result_t evaluate_rule(const rule_t *rule, event_type_t event, const rule_input_t *input);
/* Stable descending priority; ties preserve config order. */
void order_rules(const config_t *config, int order[FILEWARD_MAX_RULES]);
const char *rule_result_description(rule_result_t result);

#endif
