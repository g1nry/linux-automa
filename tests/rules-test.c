#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <fileward/config.h>
#include <fileward/rules.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

static config_t config;

static int parse(const char *text) {
    char path[] = "/tmp/fileward-config-test-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    FILE *file = fdopen(fd, "w");
    CHECK(file != NULL);
    CHECK(fputs(text, file) >= 0);
    CHECK(fclose(file) == 0);
    int result = load_config(path, &config);
    CHECK(unlink(path) == 0);
    return result;
}

static rule_result_t evaluate_path(const rule_t *rule, event_type_t event, const char *path) {
    rule_input_t input = {0};
    input.path = path;
    return evaluate_rule(rule, event, &input);
}

int main(void) {
    CHECK(parse("# example\nwatch /does-not-need-to-exist\n"
                "when created *.txt log legacy message\n"
                "rule pdfs when created **/*.pdf move /tmp/pdf\n"
                "rule paused disabled when created *.txt log \"paused\"\n"
                "rule edits enabled when modified *.txt log\n") == 0);
    CHECK(config.rule_count == 4);
    CHECK(strcmp(config.rules[0].name, "@line-3") == 0);
    CHECK(config.rules[0].line_no == 3);
    CHECK(strcmp(config.rules[0].action.message, "legacy message") == 0);
    CHECK(evaluate_path(&config.rules[0], EVENT_CREATED, "a.txt") == RULE_MATCH);
    CHECK(evaluate_path(&config.rules[0], EVENT_CREATED, "sub/a.txt") == RULE_PATTERN_MISMATCH);
    CHECK(evaluate_path(&config.rules[0], EVENT_MODIFIED, "a.txt") == RULE_EVENT_MISMATCH);
    CHECK(evaluate_path(&config.rules[1], EVENT_CREATED, "sub/a.pdf") == RULE_MATCH);
    CHECK(evaluate_path(&config.rules[2], EVENT_CREATED, "a.txt") == RULE_DISABLED);
    CHECK(evaluate_path(&config.rules[3], EVENT_MODIFIED, "a.txt") == RULE_MATCH);
    CHECK(parse("watch\t/tmp\nwhen\tdeleted\t*.txt\tlog\n") == 0);
    CHECK(parse("watch /tmp\nwhen moved_from * log") == 0);
    CHECK(parse("watch /tmp\nwhen moved_to * log\n") == 0);
    CHECK(parse("watch /tmp\n") == 0);
    CHECK(parse("when created * log\n") != 0);
    CHECK(parse("watch /tmp\nwatch /other\n") != 0);
    CHECK(parse("watch /tmp\nwatc /other\n") != 0);
    CHECK(parse("watch /tmp\nrule same when created * log\nrule same disabled when created * log\n") != 0);
    CHECK(parse("watch /tmp\nrule bad/name when created * log\n") != 0);
    CHECK(parse("watch /tmp\nrule @line-2 when created * log\n") != 0);
    CHECK(parse("watch /tmp\nrule name unknown when created * log\n") != 0);
    CHECK(parse("watch /tmp\nwhen bogus * log\n") != 0);
    CHECK(parse("watch /tmp\nwhen created * exec echo\n") != 0);
    CHECK(parse("watch /tmp\nwhen created * move\n") != 0);
    CHECK(parse("watch /tmp\nwhen created * move /tmp extra\n") != 0);
    CHECK(parse("watch /tmp\nwhen created * log \"message\" extra\n") != 0);
    CHECK(parse("watch /tmp\nwhen created * log \"unfinished\n") != 0);
    CHECK(parse("watch /tmp\nrule disabled-rule disabled when bogus * log\n") != 0);
    char name[FILEWARD_MAX_RULE_NAME + 1];
    memset(name, 'a', sizeof(name));
    name[FILEWARD_MAX_RULE_NAME - 1] = '\0';
    char text[8192];
    snprintf(text, sizeof(text), "watch /tmp\nrule %s when created * log\n", name);
    CHECK(parse(text) == 0);
    name[FILEWARD_MAX_RULE_NAME - 1] = 'a';
    name[FILEWARD_MAX_RULE_NAME] = '\0';
    snprintf(text, sizeof(text), "watch /tmp\nrule %s when created * log\n", name);
    CHECK(parse(text) != 0);
    strcpy(text, "watch /tmp\n");
    memset(text + strlen(text), 'x', 1100);
    text[1111] = '\0';
    CHECK(parse(text) != 0);
    strcpy(text, "watch /tmp\n");
    for (int i = 0; i < FILEWARD_MAX_RULES; i++) strcat(text, "when created * log\n");
    CHECK(parse(text) == 0);
    strcat(text, "when created * log\n");
    CHECK(parse(text) != 0);
    CHECK(parse("watch /tmp\nmode first\nignore **/*.part\nallow **/*.pdf\ndeny secret/**\n"
                "rule report priority=-2 stop name=report* extension=.pdf type=file min-size=1KiB max-size=2MiB min-age=1m max-age=2d when created ** log\n"
                "rule high priority=5 when created ** log\n"
                "rule tied priority=5 when created ** log\n") == 0);
    CHECK(config.first_match && config.filter_count == 3);
    CHECK(config.rules[0].priority == -2 && config.rules[0].stop);
    CHECK(config.rules[0].min_size == 1024 && config.rules[0].max_size == 2097152);
    CHECK(config.rules[0].min_age == 60 && config.rules[0].max_age == 172800);
    int order[FILEWARD_MAX_RULES];
    order_rules(&config, order);
    CHECK(order[0] == 1 && order[1] == 2 && order[2] == 0);
    CHECK(evaluate_filters(&config, "report.pdf") == RULE_MATCH);
    CHECK(evaluate_filters(&config, "sub/report.part") == RULE_IGNORED);
    CHECK(evaluate_filters(&config, "secret/report.pdf") == RULE_DENIED);
    CHECK(evaluate_filters(&config, "report.txt") == RULE_NOT_ALLOWED);
    rule_t *rule = &config.rules[0];
    rule_input_t input = {0};
    input.path = "sub/report.pdf";
    input.now = 1000;
    input.metadata.st_mtime = 900;
    input.metadata.st_size = 1024;
    input.metadata.st_mode = S_IFREG | 0600;
    CHECK(evaluate_rule(rule, EVENT_CREATED, &input) == RULE_METADATA_UNAVAILABLE);
    input.metadata_valid = 1;
    CHECK(evaluate_rule(rule, EVENT_CREATED, &input) == RULE_MATCH);
    input.metadata.st_size = 1023;
    CHECK(evaluate_rule(rule, EVENT_CREATED, &input) == RULE_TOO_SMALL);
    input.metadata.st_size = 2097152;
    CHECK(evaluate_rule(rule, EVENT_CREATED, &input) == RULE_MATCH);
    input.metadata.st_size++;
    CHECK(evaluate_rule(rule, EVENT_CREATED, &input) == RULE_TOO_LARGE);
    input.metadata.st_size = 1024;
    input.metadata.st_mtime = 941;
    CHECK(evaluate_rule(rule, EVENT_CREATED, &input) == RULE_TOO_YOUNG);
    input.metadata.st_mtime = 940;
    CHECK(evaluate_rule(rule, EVENT_CREATED, &input) == RULE_MATCH);
    input.now = 172800 + 940;
    CHECK(evaluate_rule(rule, EVENT_CREATED, &input) == RULE_MATCH);
    input.now++;
    CHECK(evaluate_rule(rule, EVENT_CREATED, &input) == RULE_TOO_OLD);
    input.now = 1000;
    input.metadata.st_mtime = 1100;
    CHECK(evaluate_rule(rule, EVENT_CREATED, &input) == RULE_TOO_YOUNG);
    input.metadata.st_mtime = 900;
    input.metadata.st_mode = S_IFDIR | 0700;
    CHECK(evaluate_rule(rule, EVENT_CREATED, &input) == RULE_TYPE_MISMATCH);
    rule->object_type = OBJECT_DIRECTORY;
    CHECK(evaluate_rule(rule, EVENT_CREATED, &input) == RULE_MATCH);
    rule->object_type = OBJECT_SYMLINK;
    input.metadata.st_mode = S_IFLNK | 0777;
    CHECK(evaluate_rule(rule, EVENT_CREATED, &input) == RULE_MATCH);
    input.path = "other.pdf";
    CHECK(evaluate_rule(rule, EVENT_CREATED, &input) == RULE_NAME_MISMATCH);
    input.path = "report.txt";
    CHECK(evaluate_rule(rule, EVENT_CREATED, &input) == RULE_EXTENSION_MISMATCH);
    input.path = "report.PDF";
    CHECK(evaluate_rule(rule, EVENT_CREATED, &input) == RULE_EXTENSION_MISMATCH);
    prepare_rule_input("/tmp", "does-not-exist", EVENT_DELETED, &input);
    CHECK(!input.metadata_valid);
    prepare_rule_input("/tmp", "does-not-exist", EVENT_MOVED_FROM, &input);
    CHECK(!input.metadata_valid);
    CHECK(parse("watch /tmp\nallow **\ndeny **\nignore **\n") == 0);
    CHECK(evaluate_filters(&config, "a") == RULE_IGNORED);
    CHECK(parse("watch /tmp\nmode all\n") == 0);
    CHECK(!config.first_match && config.filter_count == 0);
    static const char *bad_options[] = {
        "priority=1 priority=2", "priority=2147483648", "priority=-2147483649",
        "priority=abc", "priority=", "enabled disabled", "stop stop", "unknown=1",
        "min-size=-1", "min-size=1MB", "min-size=1.5MiB", "min-size=18446744073709551616",
        "min-size=9223372036854775807GiB", "min-size=2 max-size=1",
        "min-age=-1", "min-age=2ms", "min-age=2m max-age=1m", "type=socket",
        "name=", "extension=.", "extension=*.pdf", "extension=tar.gz",
        "name=a name=b", "min-size=1 min-size=2", "max-size=1 max-size=2",
        "min-age=1 min-age=2", "max-age=1 max-age=2", "type=file type=any",
        "extension=pdf extension=txt",
    };
    for (unsigned i = 0; i < sizeof(bad_options) / sizeof(bad_options[0]); i++) {
        snprintf(text, sizeof(text), "watch /tmp\nrule invalid %s when created * log\n", bad_options[i]);
        CHECK(parse(text) != 0);
    }
    CHECK(parse("watch /tmp\nrule bounds priority=-2147483648 min-size=0 max-size=1GiB min-age=0s max-age=1h when created * log\n") == 0);
    CHECK(parse("watch /tmp\nmode first\nmode all\n") != 0);
    CHECK(parse("watch /tmp\nmode unknown\n") != 0);
    CHECK(parse("watch /tmp\nignore\n") != 0);
    CHECK(parse("watch /tmp\nallow * extra\n") != 0);
    strcpy(text, "watch /tmp\n");
    for (int i = 0; i < FILEWARD_MAX_FILTERS; i++) strcat(text, "ignore *.part\n");
    CHECK(parse(text) == 0);
    strcat(text, "ignore *.part\n");
    CHECK(parse(text) != 0);
    puts("rules tests passed");
    return 0;
}
