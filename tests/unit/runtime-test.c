#define _GNU_SOURCE
#include <fileward/runtime.h>
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static atomic_int hold_probe, entered_probe, query_done, probe_reads;
static runtime_t *runtime;
static runtime_status_t observed;
static void wait_probe(const char *path) {
    if (strstr(path, "/blocked")) {
        atomic_fetch_add(&probe_reads, 1);
    }
    if (atomic_load(&hold_probe) && strstr(path, "/blocked")) {
        atomic_store(&entered_probe, 1);
        while (atomic_load(&hold_probe)) {
            usleep(1000);
        }
    }
}
int __real_lstat(const char *, struct stat *);
int __wrap_lstat(const char *path, struct stat *out) {
    wait_probe(path);
    return __real_lstat(path, out);
}
int __real_lstat64(const char *, struct stat64 *);
int __wrap_lstat64(const char *path, struct stat64 *out) {
    wait_probe(path);
    return __real_lstat64(path, out);
}
static void *tick(void *unused) {
    (void)unused;
    runtime_tick(runtime);
    return NULL;
}
static void *query(void *unused) {
    (void)unused;
    for (int i = 0; i < 400; i++) {
        runtime_status(runtime, &observed);
        if (observed.completed) {
            atomic_store(&query_done, 1);
            break;
        }
        usleep(1000);
    }
    return NULL;
}
int main(void) {
    char folder[] = "/tmp/fw-runtime-XXXXXX";
    assert(mkdtemp(folder));
    config_t config = {0};
    strcpy(config.watch_path, folder);
    config.rule_count = 1;
    rule_t *rule = config.rules;
    strcpy(rule->name, "fast");
    strcpy(rule->path_glob, "fast");
    rule->enabled = 1;
    rule->event = EVENT_CREATED;
    rule->action.type = ACTION_EXEC;
    rule->action.timeout_seconds = 2;
    strcpy(rule->action.command, "/bin/sleep 0.1");
    runtime_options_t options = {.workers = 2, .capacity = 16, .settle_ms = 0, .ready_timeout_ms = 2000};
    char fast[512], blocked[512];
    snprintf(fast, sizeof(fast), "%s/fast", folder);
    snprintf(blocked, sizeof(blocked), "%s/blocked", folder);
    FILE *file = fopen(fast, "w");
    assert(file);
    fclose(file);
    file = fopen(blocked, "w");
    assert(file);
    fclose(file);
    runtime = runtime_create(&config, &options);
    assert(runtime);
    file_event_t event = {.type = EVENT_CREATED};
    strcpy(event.filename, "fast");
    assert(!runtime_observe(&event, runtime));
    event.type = EVENT_CLOSE_WRITE;
    assert(!runtime_observe(&event, runtime));
    runtime_tick(runtime);
    runtime_status_t status;
    for (int i = 0; i < 1000; i++) {
        runtime_status(runtime, &status);
        if (status.active) {
            break;
        }
        usleep(1000);
    }
    assert(status.active == 1);
    event.type = EVENT_CREATED;
    strcpy(event.filename, "blocked");
    assert(!runtime_observe(&event, runtime));
    atomic_store(&hold_probe, 1);
    pthread_t probe_thread, query_thread;
    assert(!pthread_create(&probe_thread, NULL, tick, NULL));
    for (int i = 0; i < 1000 && !atomic_load(&entered_probe); i++) {
        usleep(1000);
    }
    assert(atomic_load(&entered_probe));
    assert(!pthread_create(&query_thread, NULL, query, NULL));
    for (int i = 0; i < 600 && !atomic_load(&query_done); i++) {
        usleep(1000);
    }
    int responsive = atomic_load(&query_done);
    atomic_store(&hold_probe, 0);
    assert(!pthread_join(probe_thread, NULL));
    assert(!pthread_join(query_thread, NULL));
    assert(responsive && observed.completed == 1 && observed.active == 0);
    runtime_destroy(runtime);
    /* Expiring an unready head must wake a ready follower on the same path. */
    options.ready_timeout_ms = 100;
    runtime = runtime_create(&config, &options);
    assert(runtime);
    event.type = EVENT_CREATED;
    strcpy(event.filename, "blocked");
    assert(!runtime_observe(&event, runtime));
    event.type = EVENT_DELETED;
    assert(!runtime_observe(&event, runtime));
    runtime_tick(runtime);
    usleep(150000);
    runtime_tick(runtime);
    for (int i = 0; i < 500; i++) {
        runtime_status(runtime, &status);
        if (status.completed) {
            break;
        }
        usleep(1000);
    }
    assert(status.completed == 1 && status.queued == 0);
    runtime_destroy(runtime);
    /* A pending writable file must not be re-statted on every idle tick. */
    options.ready_timeout_ms = 5000;
    runtime = runtime_create(&config, &options);
    assert(runtime);
    event.type = EVENT_CREATED;
    assert(!runtime_observe(&event, runtime));
    atomic_store(&probe_reads, 0);
    for (int i = 0; i < 100; i++) {
        runtime_tick(runtime);
        usleep(1000);
    }
    int reads = atomic_load(&probe_reads);
    assert(reads > 0 && reads < 50);
    printf("readiness probes for 100 idle ticks: %d\n", reads);
    runtime_destroy(runtime);
    /* Coalescing a symlink modification must retain its immediate readiness. */
    char link[512];
    snprintf(link, sizeof(link), "%s/link", folder);
    assert(!symlink("fast", link));
    options.workers = 1;
    runtime = runtime_create(&config, &options);
    assert(runtime);
    event.type = EVENT_CREATED;
    strcpy(event.filename, "fast");
    assert(!runtime_observe(&event, runtime));
    event.type = EVENT_CLOSE_WRITE;
    assert(!runtime_observe(&event, runtime));
    runtime_tick(runtime);
    for (int i = 0; i < 1000; i++) {
        runtime_status(runtime, &status);
        if (status.active) {
            break;
        }
        usleep(1000);
    }
    assert(status.active == 1);
    event.type = EVENT_MODIFIED;
    strcpy(event.filename, "link");
    assert(!runtime_observe(&event, runtime));
    assert(!runtime_observe(&event, runtime));
    for (int i = 0; i < 1000; i++) {
        runtime_status(runtime, &status);
        if (status.completed == 2) {
            break;
        }
        usleep(1000);
    }
    assert(status.completed == 2 && !status.queued);
    runtime_destroy(runtime);
    assert(!unlink(link));
    assert(!unlink(fast) && !unlink(blocked) && !rmdir(folder));
    puts("slow metadata probe does not block status or worker completion");
}
