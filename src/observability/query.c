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
#include <time.h>
#include <unistd.h>

/* Pin descriptors before parsing; names can change during rotation. A capture
 * is a bounded collection of inode prefixes, not an atomic global timestamp. */
typedef struct {
    int fd;
    struct stat st;
} segment_t;
static void release_segments(segment_t files[4]) {
    for (int i = 0; i < 4; i++)
        if (files[i].fd >= 0) {
            close(files[i].fd);
            files[i].fd = -1;
        }
}
static int capture(const char *path, segment_t files[4]) {
    for (int attempt = 0; attempt < 8; attempt++) {
        int changed = 0;
        char names[4][4096];
        for (int i = 0; i < 4; i++)
            files[i].fd = -1;
        for (int i = 0; i < 4; i++) {
            int n = i ? snprintf(names[i], sizeof(names[i]), "%s.%d", path, i)
                      : snprintf(names[i], sizeof(names[i]), "%s", path);
            if (n < 0 || n >= (int)sizeof(names[i])) {
                errno = ENAMETOOLONG;
                goto fail;
            }
            files[i].fd = open(names[i], O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
            if (files[i].fd < 0) {
                if (errno != ENOENT)
                    goto fail;
                if (!i)
                    changed = 1;
                continue;
            }
            if (fstat(files[i].fd, &files[i].st))
                goto fail;
            if (!S_ISREG(files[i].st.st_mode)) {
                errno = EINVAL;
                goto fail;
            }
            for (int j = 0; j < i; j++) {
                if (files[j].fd >= 0 && files[i].st.st_dev == files[j].st.st_dev &&
                    files[i].st.st_ino == files[j].st.st_ino)
                    changed = 1;
            }
        }
        for (int i = 0; i < 4; i++) {
            struct stat now;
            if (lstat(names[i], &now)) {
                if (errno != ENOENT)
                    goto fail;
                if (files[i].fd >= 0)
                    changed = 1;
            } else if (files[i].fd < 0 || now.st_dev != files[i].st.st_dev ||
                       now.st_ino != files[i].st.st_ino)
                changed = 1;
        }
        if (!changed)
            return 0;
        release_segments(files);
        struct timespec delay = {0, 1000000};
        nanosleep(&delay, NULL);
    }
    errno = EAGAIN;
fail: {
    int error = errno;
    release_segments(files);
    errno = error;
    return -1;
}
}

typedef struct {
    unsigned char block[65536];
    char line[AUDIT_RECORD_MAX + 1];
    off_t position;
    size_t available, used, partial;
    int started, overflow, finished;
} reverse_t;
/* 1: complete line, 0: end/incomplete tail, -1: I/O, -2: oversized line.
 * pread never follows appends beyond the captured size. */
static int previous(reverse_t *r, int fd, char **line, size_t *length) {
    while (!r->finished) {
        if (!r->available && r->position) {
            size_t n = r->position > (off_t)sizeof(r->block) ? sizeof(r->block) : (size_t)r->position;
            off_t start = r->position - (off_t)n;
            size_t done = 0;
            while (done < n) {
                ssize_t got = pread(fd, r->block + done, n - done, start + (off_t)done);
                if (got < 0 && errno == EINTR)
                    continue;
                if (got <= 0) {
                    if (!got)
                        errno = ESTALE;
                    return -1;
                }
                done += (size_t)got;
            }
            r->available = n;
            r->position = start;
        }
        if (!r->available) {
            r->finished = 1;
            if (!r->started)
                return r->partial > AUDIT_RECORD_MAX ? -2 : 0;
            if (r->overflow)
                return -2;
            *length = r->used;
            *line = r->line + AUDIT_RECORD_MAX - r->used;
            return 1;
        }
        unsigned char c = r->block[--r->available];
        if (c == '\n') {
            if (r->started) {
                int result = r->overflow ? -2 : 1;
                *length = r->used;
                *line = r->line + AUDIT_RECORD_MAX - r->used;
                r->used = 1;
                r->overflow = 0;
                return result;
            }
            r->started = 1;
            r->used = 1;
            r->line[AUDIT_RECORD_MAX - 1] = '\n';
            r->line[AUDIT_RECORD_MAX] = 0;
            if (r->partial > AUDIT_RECORD_MAX)
                return -2;
        } else {
            const unsigned char *newline = memrchr(r->block, '\n', r->available);
            size_t start = newline ? (size_t)(newline - r->block) + 1 : 0;
            size_t n = r->available + 1 - start;
            r->available = start;
            if (!r->started) {
                r->partial = r->partial + n > AUDIT_RECORD_MAX ? AUDIT_RECORD_MAX + 1 : r->partial + n;
            } else if (n > AUDIT_RECORD_MAX - r->used) {
                r->overflow = 1;
            } else if (!r->overflow) {
                r->used += n;
                memcpy(r->line + AUDIT_RECORD_MAX - r->used, r->block + start, n);
            }
        }
    }
    return 0;
}
static int equal(const char *line, const jsmntok_t *token, const char *value) {
    return token && token->end - token->start == (int)strlen(value) &&
           !memcmp(line + token->start, value, strlen(value));
}
/* Extract all needed top-level fields once; skip nested values by token extent. */
typedef struct {
    jsmntok_t *schema, *kind, *outcome, *code, *id, *parent;
} fields_t;
static fields_t fields(const char *line, jsmntok_t *tokens, int count) {
    fields_t f = {0};
    for (int i = 1; i + 1 < count && tokens[i].start < tokens[0].end;) {
        if (tokens[i].type != JSMN_STRING)
            break;
        jsmntok_t **slot = NULL;
        switch (tokens[i].end - tokens[i].start) {
        case 4:
            if (equal(line, tokens + i, "kind"))
                slot = &f.kind;
            else if (equal(line, tokens + i, "code"))
                slot = &f.code;
            break;
        case 7:
            if (equal(line, tokens + i, "outcome"))
                slot = &f.outcome;
            break;
        case 8:
            if (equal(line, tokens + i, "event_id"))
                slot = &f.id;
            break;
        case 9:
            if (equal(line, tokens + i, "parent_id"))
                slot = &f.parent;
            break;
        case 14:
            if (equal(line, tokens + i, "schema_version"))
                slot = &f.schema;
            break;
        }
        if (slot && !*slot)
            *slot = tokens + i + 1;
        if (f.schema && f.kind && f.outcome && f.code && f.id && f.parent)
            break;
        int end = tokens[i + 1].end;
        i += 2;
        while (i < count && tokens[i].start < end)
            i++;
    }
    return f;
}
static int query(const char *path, int stats, int all, unsigned limit, const char *event_id, int tail) {
    if (!limit || limit > 1000) {
        errno = EINVAL;
        return 3;
    }
    char **history = stats ? NULL : calloc(limit, sizeof(*history));
    reverse_t *reader = calloc(1, sizeof(*reader));
    if ((!stats && !history) || !reader) {
        free(history);
        free(reader);
        return 3;
    }
    segment_t files[4];
    int captured = !capture(path, files);
    int capture_error = captured ? 0 : errno;
    uint64_t records = 0, observed = 0, ready = 0, succeeded = 0, failures = 0, skipped = 0, retries = 0,
             dropped = 0, cancelled = 0, matched = 0, processed = 0, actions_cancelled = 0, evaluated = 0,
             coalesced = 0, diagnostics = 0;
    unsigned selected = 0, malformed = captured ? 0 : 1;
    int found = captured;
    for (int index = 0; captured && index < 4 && !(tail && selected == limit); index++) {
        if (files[index].fd < 0)
            continue;
        memset(reader, 0, sizeof(*reader));
        reader->position = files[index].st.st_size;
        int result;
        char *line;
        size_t length;
        while ((result = previous(reader, files[index].fd, &line, &length)) != 0) {
            if (result < 0) {
                malformed++;
                if (result == -1)
                    break;
                continue;
            }
            if (memchr(line, 0, length)) {
                malformed++;
                continue;
            }
            jsmn_parser parser;
            jsmntok_t tokens[512];
            jsmn_init(&parser);
            int count = jsmn_parse(&parser, line, length, tokens, 512);
            if (count < 1 || tokens[0].type != JSMN_OBJECT) {
                malformed++;
                continue;
            }
            size_t end = (size_t)tokens[0].end;
            while (end < length &&
                   (line[end] == ' ' || line[end] == '\t' || line[end] == '\r' || line[end] == '\n'))
                end++;
            if (end != length) {
                malformed++;
                continue;
            }
            fields_t f = fields(line, tokens, count);
            if (!equal(line, f.schema, "1") || !f.kind || !f.outcome || !f.code || !f.id ||
                f.schema->type != JSMN_PRIMITIVE || f.kind->type != JSMN_STRING ||
                f.outcome->type != JSMN_STRING || f.code->type != JSMN_STRING || f.id->type != JSMN_STRING) {
                malformed++;
                continue;
            }
            records++;
            if (stats) {
                evaluated += equal(line, f.kind, "rule_evaluated");
                coalesced += equal(line, f.kind, "event_coalesced");
                diagnostics += equal(line, f.kind, "diagnostic") && equal(line, f.outcome, "error");
                matched += equal(line, f.kind, "rule_evaluated") && equal(line, f.outcome, "matched");
                processed += equal(line, f.kind, "event_finished");
                observed += equal(line, f.kind, "file_observed");
                ready += equal(line, f.kind, "file_ready");
                retries += equal(line, f.kind, "action_retry");
                dropped += equal(line, f.kind, "event_dropped");
                cancelled += equal(line, f.kind, "event_cancelled");
                if (equal(line, f.kind, "action_result")) {
                    succeeded += equal(line, f.outcome, "success");
                    actions_cancelled += equal(line, f.outcome, "cancelled");
                    skipped += equal(line, f.outcome, "skipped");
                    failures += equal(line, f.outcome, "failure");
                } else {
                    failures += equal(line, f.outcome, "failure") && !equal(line, f.kind, "diagnostic") &&
                                !equal(line, f.kind, "exec_status");
                }
                continue;
            }
            if ((!all && !equal(line, f.kind, "action_result")) || selected == limit)
                continue;
            if (event_id && !equal(line, f.id, event_id) && !equal(line, f.parent, event_id))
                continue;
            history[selected] = strndup(line, length);
            if (!history[selected]) {
                malformed++;
                break;
            }
            selected++;
            if (tail && selected == limit)
                break;
        }
    }
    release_segments(files);
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
        for (unsigned i = selected; i > 0; i--)
            fputs(history[i - 1], stdout);
    }
    if (history) {
        for (unsigned i = 0; i < selected; i++)
            free(history[i]);
    }
    free(history);
    free(reader);
    if (malformed || !found) {
        if (capture_error)
            fprintf(stderr, "FW_JOURNAL_READ: cannot capture stable journal segments: %s\n",
                    strerror(capture_error));
        else
            fprintf(stderr, "FW_JOURNAL_READ: %u malformed or unreadable records\n", malformed);
    }
    return malformed || !found ? 3 : 0;
}
int audit_query(const char *path, int stats, int all, unsigned limit, const char *event_id) {
    return query(path, stats, all, limit, event_id, 0);
}
int audit_query_recent(const char *path, int all, unsigned limit, const char *event_id) {
    return query(path, 0, all, limit, event_id, 1);
}
