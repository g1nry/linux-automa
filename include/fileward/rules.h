#ifndef FILEWARD_RULES_H
#define FILEWARD_RULES_H

#include <fileward/config.h>

typedef enum {
    RULE_MATCH,
    RULE_DISABLED,
    RULE_EVENT_MISMATCH,
    RULE_PATTERN_MISMATCH,
} rule_result_t;

rule_result_t evaluate_rule(const rule_t *rule, event_type_t event, const char *path);
const char *rule_result_description(rule_result_t result);

#endif
