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
    CHECK(evaluate_rule(&config.rules[0], EVENT_CREATED, "a.txt") == RULE_MATCH);
    CHECK(evaluate_rule(&config.rules[0], EVENT_CREATED, "sub/a.txt") == RULE_PATTERN_MISMATCH);
    CHECK(evaluate_rule(&config.rules[0], EVENT_MODIFIED, "a.txt") == RULE_EVENT_MISMATCH);
    CHECK(evaluate_rule(&config.rules[1], EVENT_CREATED, "sub/a.pdf") == RULE_MATCH);
    CHECK(evaluate_rule(&config.rules[2], EVENT_CREATED, "a.txt") == RULE_DISABLED);
    CHECK(evaluate_rule(&config.rules[3], EVENT_MODIFIED, "a.txt") == RULE_MATCH);
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
    puts("rules tests passed");
    return 0;
}
