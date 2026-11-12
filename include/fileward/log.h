#ifndef FILEWARD_LOG_H
#define FILEWARD_LOG_H

void log_init(const char *program_name);
/* These writers never emit audit records: safe to call from audit failure paths. */
void log_stderr(int priority, const char *format, ...) __attribute__((format(printf, 2, 3)));
typedef struct {
    const char *kind, *code, *message, *root, *path, *event, *rule, *action, *destination;
    const char *event_id, *parent_id, *executable;
    int error, attempt, exit_code, signal;
    unsigned generation;
    long long duration_ms;
} log_failure_t;
void log_failure(int priority, const log_failure_t *failure);

void log_info(const char *format, ...);
void log_warn(const char *format, ...);
void log_error(const char *format, ...);
void log_debug(const char *format, ...);

#endif
