#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <fileward/log.h>
#include <fileward/audit.h>
#include <errno.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char *app_name = "fileward";
static int journal_stream;

void log_init(const char *program_name) {
    if (program_name != NULL) {
        app_name = program_name;
    }
    int saved = errno;
    const char *stream = getenv("JOURNAL_STREAM");
    unsigned long long device, inode;
    char extra;
    struct stat st;
    journal_stream = stream && sscanf(stream, "%llu:%llu%c", &device, &inode, &extra) == 2 &&
                     !fstat(STDERR_FILENO, &st) && device == (unsigned long long)st.st_dev &&
                     inode == (unsigned long long)st.st_ino;
    errno = saved;
}

static const char *level_name(int priority) {
    return priority <= 3 ? "error" : priority == 4 ? "warn" : priority == 7 ? "debug" : "info";
}

/* Keep readable UTF-8, but never let filenames forge physical log lines or
 * terminal escapes. Invalid bytes remain visible as hex, rather than binary. */
static int utf8_length(const unsigned char *p) {
    unsigned a = *p;
    int n = a >= 0xc2 && a <= 0xdf ? 2 : a >= 0xe0 && a <= 0xef ? 3 : a >= 0xf0 && a <= 0xf4 ? 4 : 0;
    for (int i = 1; i < n; i++) {
        if (!p[i] || (p[i] & 0xc0) != 0x80)
            return 0;
    }
    if (n == 3 && ((a == 0xe0 && p[1] < 0xa0) || (a == 0xed && p[1] >= 0xa0)))
        return 0;
    if (n == 4 && ((a == 0xf0 && p[1] < 0x90) || (a == 0xf4 && p[1] >= 0x90)))
        return 0;
    return n;
}

static void text(const char *value, int quoted) {
    size_t used = 0, maximum = quoted ? 2048 : 16384;
    const unsigned char *p = (const unsigned char *)(value ? value : "");
    if (quoted)
        fputc('"', stderr);
    while (*p && used < maximum) {
        const unsigned char *start = p;
        while ((size_t)(p - start) < maximum - used && *p >= 32 && *p < 127 &&
               (!quoted || (*p != '"' && *p != '\\')))
            p++;
        fwrite(start, 1, (size_t)(p - start), stderr);
        used += (size_t)(p - start);
        if (!*p || used == maximum)
            break;
        int n = *p >= 128 ? utf8_length(p) : 0;
        size_t size = (*p == '\n' || *p == '\r' || *p == '\t' || (quoted && (*p == '"' || *p == '\\'))) ? 2
                      : n ? (size_t)n
                          : 4;
        if (size > maximum - used)
            break;
        used += size;
        if (*p == '\n')
            fputs("\\n", stderr);
        else if (*p == '\r')
            fputs("\\r", stderr);
        else if (*p == '\t')
            fputs("\\t", stderr);
        else if (quoted && (*p == '"' || *p == '\\'))
            fprintf(stderr, "\\%c", *p);
        else if (*p >= 128) {
            if (n) {
                fwrite(p, 1, (size_t)n, stderr);
                p += n;
                continue;
            }
            fprintf(stderr, "\\x%02x", *p);
        } else
            fprintf(stderr, "\\x%02x", *p);
        p++;
    }
    if (*p)
        fputs("... [truncated]", stderr);
    if (quoted)
        fputc('"', stderr);
}

static void prefix(int priority) {
    time_t now = time(NULL);
    struct tm time_info;
    char timestamp[32] = "unknown-time";
    if (localtime_r(&now, &time_info)) {
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &time_info);
    }
    if (journal_stream)
        fprintf(stderr, "<%d>", priority);
    fprintf(stderr, "[%s] %s: %s: ", timestamp, app_name, level_name(priority));
}

static void formatted(int priority, const char *format, va_list args, int audit) {
    int saved = errno;
    char message[4096];
    int length = vsnprintf(message, sizeof(message), format, args);
    if (length < 0)
        strcpy(message, "log formatting failed");
    else if ((size_t)length >= sizeof(message)) {
        static const char mark[] = "... [truncated]";
        memcpy(message + sizeof(message) - sizeof(mark), mark, sizeof(mark));
    }
    flockfile(stderr);
    prefix(priority);
    text(message, 0);
    fputc('\n', stderr);
    funlockfile(stderr);
    if (audit)
        audit_diagnostic(level_name(priority), message);
    errno = saved;
}

void log_stderr(int priority, const char *format, ...) {
    va_list args;
    va_start(args, format);
    formatted(priority, format, args, 0);
    va_end(args);
}

static void field(const char *name, const char *value) {
    if (value && *value) {
        fprintf(stderr, " %s=", name);
        text(value, 1);
    }
}

void log_failure(int priority, const log_failure_t *f) {
    int saved = errno;
    flockfile(stderr);
    prefix(priority);
    text(f->code, 0);
    fputc(':', stderr);
    field("kind", f->kind);
    field("message", f->message);
    if (f->error) {
        fprintf(stderr, " errno=%d", f->error);
        field("reason", strerror(f->error));
    }
    field("watch_root", f->root);
    field("relative_path", f->path);
    field("event", f->event);
    field("rule", f->rule);
    field("action", f->action);
    field("destination", f->destination);
    field("executable", f->executable);
    if (f->attempt)
        fprintf(stderr, " attempt=%d", f->attempt);
    if (f->exit_code >= 0)
        fprintf(stderr, " exit_code=%d", f->exit_code);
    if (f->signal) {
        fprintf(stderr, " signal=%d", f->signal);
        field("signal_name", strsignal(f->signal));
    }
    fprintf(stderr, " duration_ms=%lld generation=%u", f->duration_ms, f->generation);
    field("event_id", f->event_id);
    field("parent_id", f->parent_id);
    fputc('\n', stderr);
    funlockfile(stderr);
    errno = saved;
}

void log_info(const char *format, ...) {
    va_list args;

    va_start(args, format);
    formatted(6, format, args, 1);
    va_end(args);
}

void log_warn(const char *format, ...) {
    va_list args;

    va_start(args, format);
    formatted(4, format, args, 1);
    va_end(args);
}

void log_error(const char *format, ...) {
    va_list args;

    va_start(args, format);
    formatted(3, format, args, 1);
    va_end(args);
}

void log_debug(const char *format, ...) {
#ifdef DEBUG
    va_list args;

    va_start(args, format);
    formatted(7, format, args, 1);
    va_end(args);
#else
    (void)format;
#endif
}
