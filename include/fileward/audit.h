#ifndef FILEWARD_AUDIT_H
#define FILEWARD_AUDIT_H
#include <stdint.h>
#include <stddef.h>
#include <sys/stat.h>

#define AUDIT_ID_SIZE 80
#define AUDIT_RECORD_MAX 131072U
/* One process owns the journal through a separate lock file, including rotation. */
int audit_open(const char *path, uint64_t max_bytes);
void audit_set_sync(int enabled);
void audit_close(void);
int audit_failed(void);
const char *audit_error_code(int error);
void audit_context(const char *root, const char *path, const char *event, unsigned generation, int dry_run,
                   const char *parent);
void audit_rule(const char *rule, const char *action);
void audit_emit(const char *kind, const char *outcome, const char *code, int error, const char *message,
                int attempt, long long duration_ms, char id[AUDIT_ID_SIZE]);
void audit_metadata(const struct stat *metadata);
void audit_destination(const char *path);
void audit_exec_status(int exit_code, int signal);
void audit_diagnostic(const char *level, const char *message);
int audit_query(const char *path, int stats, int all, unsigned limit, const char *event_id);
#endif
