#include "log.h"
#include <stdio.h>
#include <time.h>
#include <syslog.h>

static int use_syslog = 0;

void log_init(const char *program_name) {
    openlog(program_name, LOG_PID | LOG_CONS, LOG_USER);
    use_syslog = 1;
}

static void log_message(int priority, const char *format, va_list args) {
    if (use_syslog) {
        vsyslog(priority, format, args);
    } else {
        time_t now = time(NULL);
        char *time_str = ctime(&now);
        time_str[strlen(time_str)-1] = '\0'; // remove \n
        fprintf(stderr, "[%s] ", time_str);
        vfprintf(stderr, format, args);
        fprintf(stderr, "\n");
    }
}

void log_info(const char *format, ...) {
    va_list args;
    va_start(args, format);
    log_message(LOG_INFO, format, args);
    va_end(args);
}

void log_warn(const char *format, ...) {
    va_list args;
    va_start(args, format);
    log_message(LOG_WARNING, format, args);
    va_end(args);
}

void log_error(const char *format, ...) {
    va_list args;
    va_start(args, format);
    log_message(LOG_ERR, format, args);
    va_end(args);
}

void log_debug(const char *format, ...) {
#ifdef DEBUG
    va_list args;
    va_start(args, format);
    log_message(LOG_DEBUG, format, args);
    va_end(args);
#endif
}