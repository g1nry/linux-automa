#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <fileward/audit.h>
#define JSMN_STRICT
#include "../../third_party/jsmn/jsmn.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int equal(const char *line, jsmntok_t *token, const char *value) {
    return token && token->end - token->start == (int)strlen(value) &&
           memcmp(line + token->start, value, strlen(value)) == 0;
}
static jsmntok_t *value(const char *line, jsmntok_t *tokens, int count, const char *key) {
    for (int i = 1; i + 1 < count && tokens[i].start < tokens[0].end;) {
        if (tokens[i].type != JSMN_STRING) {
            return NULL;
        }
        if (equal(line, tokens + i, key)) {
            return tokens + i + 1;
        }
        int end = tokens[i + 1].end;
        i += 2;
        while (i < count && tokens[i].start < end) {
            i++;
        }
    }
    return NULL;
}
int audit_query(const char *path, int stats, int all, unsigned limit, const char *event_id) {
    char **history = calloc(limit, sizeof(char *));
    char *line = malloc(131072);
    if (!history || !line) {
        free(history);
        free(line);
        return 3;
    }
    uint64_t records = 0, observed = 0, ready = 0, succeeded = 0, failures = 0, skipped = 0, retries = 0,
             dropped = 0, cancelled = 0, matched = 0, processed = 0, actions_cancelled = 0, evaluated = 0,
             coalesced = 0, diagnostics = 0;
    unsigned selected = 0, malformed = 0;
    int found = 0;
    for (int suffix = 3; suffix >= 0; suffix--) {
        char name[4096];
        int written = suffix ? snprintf(name, sizeof(name), "%s.%d", path, suffix)
                             : snprintf(name, sizeof(name), "%s", path);
        if (written < 0 || (size_t)written >= sizeof(name)) {
            malformed++;
            continue;
        }
        int fd = open(name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (fd < 0) {
            if (suffix == 0 || errno != ENOENT) {
                malformed++;
            }
            continue;
        }
        struct stat st;
        if (fstat(fd, &st) || !S_ISREG(st.st_mode)) {
            close(fd);
            malformed++;
            continue;
        }
        FILE *file = fdopen(fd, "r");
        if (!file) {
            close(fd);
            malformed++;
            continue;
        }
        found = 1;
        /* Read a finite segment snapshot even when the active writer outpaces queries. */
        while (ftello(file) < st.st_size && fgets(line, 131072, file)) {
            if (ftello(file) > st.st_size) break;
            if (!strchr(line, '\n')) {
                /* A live writer can have an incomplete final line. */
                if (!feof(file)) {
                    int c;
                    while ((c = fgetc(file)) != EOF && c != '\n') {
                    }
                    malformed++;
                }
                continue;
            }
            jsmn_parser parser;
            jsmntok_t tokens[512];
            jsmn_init(&parser);
            int count = jsmn_parse(&parser, line, strlen(line), tokens, 512);
            if (count < 1 || tokens[0].type != JSMN_OBJECT ||
                !equal(line, value(line, tokens, count, "schema_version"), "1")) {
                malformed++;
                continue;
            }
            jsmntok_t *kind = value(line, tokens, count, "kind");
            jsmntok_t *outcome = value(line, tokens, count, "outcome");
            jsmntok_t *code = value(line, tokens, count, "code");
            if (!kind || !outcome || !code || !value(line, tokens, count, "event_id")) {
                malformed++;
                continue;
            }
            records++;
            evaluated += equal(line, kind, "rule_evaluated");
            coalesced += equal(line, kind, "event_coalesced");
            diagnostics += equal(line, kind, "diagnostic") && equal(line, outcome, "error");
            matched += equal(line, kind, "rule_evaluated") && equal(line, outcome, "matched");
            processed += equal(line, kind, "event_finished");
            observed += equal(line, kind, "file_observed");
            ready += equal(line, kind, "file_ready");
            retries += equal(line, kind, "action_retry");
            dropped += equal(line, kind, "event_dropped");
            cancelled += equal(line, kind, "event_cancelled");
            if (equal(line, kind, "action_result")) {
                succeeded += equal(line, outcome, "success");
                actions_cancelled += equal(line, outcome, "cancelled");
                skipped += equal(line, outcome, "skipped");
                failures += equal(line, outcome, "failure");
            } else {
                failures += equal(line, outcome, "failure") && !equal(line, kind, "diagnostic") &&
                            !equal(line, kind, "exec_status");
            }
            if (!all && !equal(line, kind, "action_result")) {
                continue;
            }
            if (event_id && !equal(line, value(line, tokens, count, "event_id"), event_id) &&
                !equal(line, value(line, tokens, count, "parent_id"), event_id)) {
                continue;
            }
            free(history[selected % limit]);
            history[selected % limit] = strdup(line);
            if (!history[selected % limit]) {
                malformed++;
                break;
            }
            selected++;
        }
        if (ferror(file)) {
            malformed++;
        }
        fclose(file);
    }
    if (stats) {
        printf("{\"schema_version\":1,\"scope\":\"retained_journal\",\"records\":%llu,\"events_observed\":%"
               "llu,\"events_ready\":%llu,\"events_processed\":%llu,\"rules_matched\":%llu,\"rules_"
               "evaluated\":%llu,\"events_coalesced\":%llu,\"diagnostic_errors\":%llu,\"actions_succeeded\":%"
               "llu,\"actions_skipped\":%llu,\"actions_cancelled\":%llu,\"failures\":%llu,\"retries\":%llu,"
               "\"events_dropped\":%llu,"
               "\"events_cancelled\":%llu,\"malformed_records\":%u}\n",
               (unsigned long long)records, (unsigned long long)observed, (unsigned long long)ready,
               (unsigned long long)processed, (unsigned long long)matched, (unsigned long long)evaluated,
               (unsigned long long)coalesced, (unsigned long long)diagnostics, (unsigned long long)succeeded,
               (unsigned long long)skipped, (unsigned long long)actions_cancelled,
               (unsigned long long)failures, (unsigned long long)retries, (unsigned long long)dropped,
               (unsigned long long)cancelled, malformed);
    } else {
        unsigned start = selected > limit ? selected - limit : 0;
        for (unsigned i = start; i < selected; i++) {
            if (history[i % limit]) {
                fputs(history[i % limit], stdout);
            }
        }
    }
    for (unsigned i = 0; i < limit; i++) {
        free(history[i]);
    }
    free(history);
    free(line);
    if (malformed || !found) {
        fprintf(stderr, "FW_JOURNAL_READ: missing/unreadable journal or %u malformed records\n", malformed);
    }
    return malformed || !found ? 3 : 0;
}
