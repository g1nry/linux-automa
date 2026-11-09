#include <fileward/rules.h>
#include <fileward/glob.h>

rule_result_t evaluate_rule(const rule_t *rule, event_type_t event, const char *path) {
    if (!rule->enabled) {
        return RULE_DISABLED;
    }
    if (rule->event != event) {
        return RULE_EVENT_MISMATCH;
    }
    if (!glob_match(rule->path_glob, path)) {
        return RULE_PATTERN_MISMATCH;
    }
    return RULE_MATCH;
}

const char *rule_result_description(rule_result_t result) {
    switch (result) {
    case RULE_MATCH: return "matched";
    case RULE_DISABLED: return "disabled";
    case RULE_EVENT_MISMATCH: return "event does not match";
    case RULE_PATTERN_MISMATCH: return "pattern does not match";
    }
    return "unknown result";
}
