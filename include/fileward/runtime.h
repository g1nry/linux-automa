#ifndef FILEWARD_RUNTIME_H
#define FILEWARD_RUNTIME_H
#include <fileward/config.h>
#include <fileward/watcher.h>

typedef struct runtime runtime_t;
typedef struct {
    unsigned workers, capacity, settle_ms, ready_timeout_ms;
    int ready_mode; /* 0 close-write, 1 stable, 2 immediate */
    int dry_run;
    const char *state_file;
} runtime_options_t;
runtime_t *runtime_create(const config_t *config, const runtime_options_t *options);
int runtime_reload(runtime_t *runtime, const config_t *config);
int runtime_observe(const file_event_t *event, void *runtime);
void runtime_tick(runtime_t *runtime);
typedef struct {
    unsigned workers, queued, active, capacity, generation;
    unsigned long long observed, dropped, completed, action_failures;
} runtime_status_t;
void runtime_status(runtime_t *, runtime_status_t *);
void runtime_destroy(runtime_t *runtime);
#endif
