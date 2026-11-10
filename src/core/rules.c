#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <fileward/rules.h>
#include <fileward/glob.h>

#include <stdio.h>
#include <string.h>

void prepare_rule_input(const char *root, const char *path, event_type_t event, rule_input_t *input) {
    memset(input, 0, sizeof(*input));
    input->path = path;
    input->basename = strrchr(path, '/');
    input->basename = input->basename ? input->basename + 1 : path;
    const char *dot = strrchr(input->basename, '.');
    input->extension = dot && dot != input->basename ? dot + 1 : "";
    input->now = time(NULL);
    if (event == EVENT_DELETED || event == EVENT_MOVED_FROM) {
        return;
    }
    char full_path[PATH_MAX];
    int length = snprintf(full_path, sizeof(full_path), "%s/%s", root, path);
    if (length >= 0 && (size_t)length < sizeof(full_path)) {
        input->metadata_valid = lstat(full_path, &input->metadata) == 0;
    }
}

rule_result_t evaluate_filters(const config_t *config, const char *path) {
    int has_allow = 0, allowed = 0, ignored = 0, denied = 0;
    for (int i = 0; i < config->filter_count; i++) {
        const path_filter_t *filter = &config->filters[i];
        int match = glob_match(filter->pattern, path);
        if (filter->type == FILTER_ALLOW) {
            has_allow = 1;
            allowed |= match;
        } else if (filter->type == FILTER_IGNORE) {
            ignored |= match;
        } else {
            denied |= match;
        }
    }
    if (ignored) {
        return RULE_IGNORED;
    }
    if (denied) {
        return RULE_DENIED;
    }
    if (has_allow && !allowed) {
        return RULE_NOT_ALLOWED;
    }
    return RULE_MATCH;
}

rule_result_t evaluate_rule(const rule_t *rule, event_type_t event, const rule_input_t *input) {
    if (!rule->enabled) {
        return RULE_DISABLED;
    }
    if (rule->event != event) {
        return RULE_EVENT_MISMATCH;
    }
    if (!glob_match(rule->path_glob, input->path)) {
        return RULE_PATTERN_MISMATCH;
    }
    const char *base = input->basename;
    if (!base) {
        base = strrchr(input->path, '/');
        base = base ? base + 1 : input->path;
    }
    if (rule->name_glob[0] && !glob_match(rule->name_glob, base)) {
        return RULE_NAME_MISMATCH;
    }
    if (rule->extension[0]) {
        const char *extension = input->extension;
        if (!extension) {
            const char *dot = strrchr(base, '.');
            extension = dot && dot != base ? dot + 1 : "";
        }
        if (strcmp(extension, rule->extension) != 0) {
            return RULE_EXTENSION_MISMATCH;
        }
    }
    int needs_metadata = rule->object_type != OBJECT_ANY || rule->has_min_size || rule->has_max_size ||
                         rule->has_min_age || rule->has_max_age;
    if (needs_metadata && !input->metadata_valid) {
        return RULE_METADATA_UNAVAILABLE;
    }
    if ((rule->object_type == OBJECT_FILE && !S_ISREG(input->metadata.st_mode)) ||
        (rule->object_type == OBJECT_DIRECTORY && !S_ISDIR(input->metadata.st_mode)) ||
        (rule->object_type == OBJECT_SYMLINK && !S_ISLNK(input->metadata.st_mode))) {
        return RULE_TYPE_MISMATCH;
    }
    if (rule->has_min_size &&
        (input->metadata.st_size < 0 || (uint64_t)input->metadata.st_size < rule->min_size)) {
        return RULE_TOO_SMALL;
    }
    if (rule->has_max_size && input->metadata.st_size >= 0 &&
        (uint64_t)input->metadata.st_size > rule->max_size) {
        return RULE_TOO_LARGE;
    }
    double age = difftime(input->now, input->metadata.st_mtime);
    if (age < 0) {
        age = 0;
    }
    if (rule->has_min_age && age < (double)rule->min_age) {
        return RULE_TOO_YOUNG;
    }
    if (rule->has_max_age && age > (double)rule->max_age) {
        return RULE_TOO_OLD;
    }
    return RULE_MATCH;
}

void order_rules(const config_t *config, int order[FILEWARD_MAX_RULES]) {
    for (int i = 0; i < config->rule_count; i++) {
        int j = i;
        while (j > 0 && config->rules[order[j - 1]].priority < config->rules[i].priority) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = i;
    }
}

const char *rule_result_description(rule_result_t result) {
    switch (result) {
    case RULE_MATCH:
        return "matched";
    case RULE_DISABLED:
        return "disabled";
    case RULE_EVENT_MISMATCH:
        return "event does not match";
    case RULE_PATTERN_MISMATCH:
        return "pattern does not match";
    case RULE_NAME_MISMATCH:
        return "name does not match";
    case RULE_EXTENSION_MISMATCH:
        return "extension does not match";
    case RULE_METADATA_UNAVAILABLE:
        return "metadata unavailable";
    case RULE_TYPE_MISMATCH:
        return "object type does not match";
    case RULE_TOO_SMALL:
        return "size below minimum";
    case RULE_TOO_LARGE:
        return "size above maximum";
    case RULE_TOO_YOUNG:
        return "modification age below minimum";
    case RULE_TOO_OLD:
        return "modification age above maximum";
    case RULE_IGNORED:
        return "path ignored";
    case RULE_DENIED:
        return "path denied";
    case RULE_NOT_ALLOWED:
        return "path not in allow list";
    }
    return "unknown result";
}
