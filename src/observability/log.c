#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <fileward/log.h>
#include <fileward/audit.h>
#include <errno.h>

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static const char *app_name = "fileward";

void log_init(const char *program_name) {
    if (program_name != NULL) {
        app_name = program_name;
    }
}

static void log_message(const char *level, const char *format, va_list args) {
    int saved = errno;
    char message[4096];
    vsnprintf(message, sizeof(message), format, args);
    time_t now = time(NULL);
    struct tm time_info;
    char timestamp[32] = "unknown-time";
    if (localtime_r(&now, &time_info)) {
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &time_info);
    }
    flockfile(stderr);
    fprintf(stderr, "[%s] %s: %s: %s\n", timestamp, app_name, level, message);
    funlockfile(stderr);
    audit_diagnostic(level, message);
    errno = saved;
}

void log_info(const char *format, ...) {
    va_list args;

    va_start(args, format);
    log_message("info", format, args);
    va_end(args);
}

void log_warn(const char *format, ...) {
    va_list args;

    va_start(args, format);
    log_message("warn", format, args);
    va_end(args);
}

void log_error(const char *format, ...) {
    va_list args;

    va_start(args, format);
    log_message("error", format, args);
    va_end(args);
}

void log_debug(const char *format, ...) {
#ifdef DEBUG
    va_list args;

    va_start(args, format);
    log_message("debug", format, args);
    va_end(args);
#else
    (void)format;
#endif
}
