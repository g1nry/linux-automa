#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <fileward/runtime.h>
#include <fileward/state.h>
#include <fileward/action.h>
#include <fileward/rules.h>
#include <fileward/log.h>
#include <fileward/audit.h>
#include <pthread.h>
#include <stdatomic.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct snapshot {
    atomic_uint references;
    unsigned generation;
    config_t config;
    int order[FILEWARD_MAX_RULES];
    int saved_enabled;
} snapshot_t;
typedef struct job {
    struct job *next, *previous;
    snapshot_t *snapshot;
    file_event_t event;
    struct stat last;
    int have_stat, closed, ready;
    long long received, stable_since, check_after;
    char id[AUDIT_ID_SIZE];
} job_t;
struct runtime {
    pthread_mutex_t mutex;
    state_store_t *state;
    pthread_cond_t changed;
    pthread_t threads[32];
    unsigned started, pending_count, active_count;
    job_t *pending, *tail, *active[32];
    snapshot_t *current;
    runtime_options_t options;
    atomic_int cancel;
    unsigned long long observed, dropped, completed, action_failures;
};
typedef struct {
    runtime_t *runtime;
    unsigned slot;
} worker_arg_t;

static long long clock_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static const char *type_name(event_type_t type) {
    switch (type) {
    case EVENT_SAVED:
        return "saved";
    case EVENT_CREATED:
        return "created";
    case EVENT_MODIFIED:
        return "modified";
    case EVENT_DELETED:
        return "deleted";
    case EVENT_MOVED_FROM:
        return "moved_from";
    case EVENT_MOVED_TO:
        return "moved_to";
    case EVENT_CLOSE_WRITE:
        return "close_write";
    case EVENT_OVERFLOW:
        return "overflow";
    case EVENT_WATCH_LOST:
        return "watch_lost";
    default:
        return "unknown";
    }
}
static snapshot_t *snapshot_new(const config_t *config, unsigned generation) {
    snapshot_t *snapshot = malloc(sizeof(*snapshot));
    if (snapshot) {
        snapshot->config = *config;
        snapshot->generation = generation;
        order_rules(config, snapshot->order);
        snapshot->saved_enabled = 0;
        for (int i = 0; i < config->rule_count; i++) {
            snapshot->saved_enabled |= config->rules[i].enabled && config->rules[i].event == EVENT_SAVED;
        }
        atomic_init(&snapshot->references, 1);
    }
    return snapshot;
}
static void release(snapshot_t *snapshot) {
    if (atomic_fetch_sub(&snapshot->references, 1) == 1) {
        free(snapshot);
    }
}
static void context_for(job_t *job, int dry) {
    audit_context(job->snapshot->config.watch_path, job->event.filename, type_name(job->event.type),
                  job->snapshot->generation, dry, job->id);
    audit_metadata(job->have_stat ? &job->last : NULL);
}
static int same_path(job_t *first, job_t *second) {
    return strcmp(first->event.filename, second->event.filename) == 0;
}
/* Queue links are only changed under runtime->mutex. */
static void append_job(runtime_t *runtime, job_t *job) {
    job->previous = runtime->tail;
    if (runtime->tail) {
        runtime->tail->next = job;
    } else {
        runtime->pending = job;
    }
    runtime->tail = job;
    runtime->pending_count++;
}
static void remove_pending(runtime_t *runtime, job_t *job) {
    if (job->previous) {
        job->previous->next = job->next;
    } else {
        runtime->pending = job->next;
    }
    if (job->next) {
        job->next->previous = job->previous;
    } else {
        runtime->tail = job->previous;
    }
    job->next = job->previous = NULL;
    runtime->pending_count--;
}
static int stat_job(job_t *job, struct stat *st) {
    char path[PATH_MAX];
    int length = snprintf(path, sizeof(path), "%s/%s", job->snapshot->config.watch_path, job->event.filename);
    if (length < 0 || (size_t)length >= sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return lstat(path, st);
}
static int unchanged(const struct stat *a, const struct stat *b) {
    return a->st_size == b->st_size && a->st_mtim.tv_sec == b->st_mtim.tv_sec &&
           a->st_mtim.tv_nsec == b->st_mtim.tv_nsec && a->st_ctim.tv_sec == b->st_ctim.tv_sec &&
           a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}
static void finish_job(job_t *job) {
    release(job->snapshot);
    free(job);
}

static void state_success(runtime_t *runtime, job_t *job, const char *action) {
    if (!runtime->options.state_file || runtime->options.dry_run) {
        return;
    }
    int error = state_store_append(runtime->state, job->snapshot->config.watch_path, job->event.filename,
                                   type_name(job->event.type), action)
                    ? errno
                    : 0;
    if (error) {
        audit_emit("state_error", "failure", "FW_STATE_WRITE", error, strerror(error), 0, 0, NULL);
    }
}
static void execute_job(runtime_t *runtime, job_t *job) {
    context_for(job, runtime->options.dry_run);
    config_t *config = &job->snapshot->config;
    rule_input_t input;
    prepare_rule_input(config->watch_path, job->event.filename, job->event.type, &input);
    audit_metadata(input.metadata_valid ? &input.metadata : NULL);
    if (job->have_stat && !input.metadata_valid) {
        audit_emit("event_cancelled", "skipped", "FW_FILE_NOT_FOUND", ENOENT, "file vanished before dispatch",
                   0, 0, NULL);
        return;
    }
    if (job->have_stat && input.metadata_valid &&
        (input.metadata.st_ino != job->last.st_ino || input.metadata.st_dev != job->last.st_dev)) {
        audit_emit("event_cancelled", "skipped", "FW_SOURCE_REPLACED", ESTALE,
                   "path now identifies a different object", 0, 0, NULL);
        return;
    }
    if (!config->rule_count) {
        flockfile(stdout);
        printf("event: %s path: %s\n", type_name(job->event.type), job->event.filename);
        fflush(stdout);
        funlockfile(stdout);
    }
    for (int i = 0; i < config->rule_count; i++) {
        if (atomic_load(&runtime->cancel) || audit_failed()) {
            break;
        }
        rule_t *rule = &config->rules[job->snapshot->order[i]];
        audit_rule(rule->name, action_type_name(rule->action.type));
        rule_result_t result = evaluate_rule(rule, job->event.type, &input);
        if (result != RULE_MATCH) {
            audit_emit("rule_evaluated", "skipped", "FW_RULE_MISMATCH", 0, rule_result_description(result), 0,
                       0, NULL);
            continue;
        }
        audit_emit("rule_evaluated", "matched", "FW_OK", 0, "rule matched", 0, 0, NULL);
        log_info("rule '%s' (line %d) matched %s", rule->name, rule->line_no, job->event.filename);
        long long start = clock_ms();
        audit_emit("action_started", "started", "FW_OK", 0, "action started", 0, 0, NULL);
        if (audit_failed()) {
            break;
        }
        int status = execute_action(&rule->action, config->watch_path, job->event.filename, job->event.type,
                                    runtime->options.dry_run);
        int error = status < 0 ? errno : 0;
        if (status < 0 && error != ECANCELED) {
            pthread_mutex_lock(&runtime->mutex);
            runtime->action_failures++;
            pthread_mutex_unlock(&runtime->mutex);
        }
        audit_emit("action_result",
                   status < 0                 ? (error == ECANCELED ? "cancelled" : "failure")
                   : status == ACTION_SKIPPED ? "skipped"
                   : runtime->options.dry_run ? "dry_run"
                                              : "success",
                   status < 0                 ? action_last_error_code(error)
                   : status == ACTION_SKIPPED ? "FW_CONFLICT_SKIP"
                                              : "FW_OK",
                   error, status < 0 ? strerror(error) : "action completed", action_attempts(),
                   clock_ms() - start, NULL);
        if (status < 0 && error == ECANCELED) {
            log_info("rule '%s': action cancelled for %s", rule->name, job->event.filename);
        } else if (status < 0) {
            log_error("rule '%s': action failed for %s: %s", rule->name, job->event.filename,
                      strerror(error));
        } else if (!status) {
            state_success(runtime, job, action_type_name(rule->action.type));
        }
        if (config->first_match || rule->stop) {
            break;
        }
    }
    audit_rule(NULL, NULL);
    audit_emit("event_finished", atomic_load(&runtime->cancel) ? "cancelled" : "complete",
               atomic_load(&runtime->cancel) ? "FW_CANCELLED" : "FW_OK", 0, "rule chain finished", 0, 0,
               NULL);
}

static job_t *take_job(runtime_t *runtime, unsigned slot) {
    for (job_t *job = runtime->pending; job; job = job->next) {
        if (!job->ready) {
            continue;
        }
        int blocked = 0;
        for (job_t *earlier = runtime->pending; earlier != job; earlier = earlier->next) {
            if (same_path(earlier, job)) {
                blocked = 1;
                break;
            }
        }
        for (unsigned i = 0; !blocked && i < runtime->started; i++) {
            if (runtime->active[i] && same_path(runtime->active[i], job)) {
                blocked = 1;
            }
        }
        if (blocked) {
            continue;
        }
        remove_pending(runtime, job);
        runtime->active_count++;
        runtime->active[slot] = job;
        return job;
    }
    return NULL;
}
static void *worker(void *argument) {
    worker_arg_t args = *(worker_arg_t *)argument;
    free(argument);
    runtime_t *runtime = args.runtime;
    pthread_mutex_lock(&runtime->mutex);
    while (!atomic_load(&runtime->cancel)) {
        job_t *job = take_job(runtime, args.slot);
        if (!job) {
            pthread_cond_wait(&runtime->changed, &runtime->mutex);
            continue;
        }
        pthread_mutex_unlock(&runtime->mutex);
        execute_job(runtime, job);
        pthread_mutex_lock(&runtime->mutex);
        runtime->active[args.slot] = NULL;
        runtime->active_count--;
        runtime->completed++;
        finish_job(job);
        pthread_cond_broadcast(&runtime->changed);
    }
    pthread_mutex_unlock(&runtime->mutex);
    return NULL;
}

runtime_t *runtime_create(const config_t *config, const runtime_options_t *options) {
    runtime_t *runtime = calloc(1, sizeof(*runtime));
    if (!runtime) {
        return NULL;
    }
    runtime->options = *options;
    atomic_init(&runtime->cancel, 0);
    runtime->current = snapshot_new(config, 1);
    if (!runtime->current) {
        free(runtime);
        return NULL;
    }
    pthread_mutex_init(&runtime->mutex, NULL);
    runtime->state = state_store_create(options->state_file);
    if (!runtime->state) {
        pthread_mutex_destroy(&runtime->mutex);
        release(runtime->current);
        free(runtime);
        return NULL;
    }
    pthread_cond_init(&runtime->changed, NULL);
    action_set_cancel_flag(&runtime->cancel);
    /* Workers inherit blocked termination signals. Only the observation thread handles them. */
    sigset_t signals, original;
    sigemptyset(&signals);
    sigaddset(&signals, SIGTERM);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGHUP);
    pthread_sigmask(SIG_BLOCK, &signals, &original);
    for (unsigned i = 0; i < options->workers; i++) {
        worker_arg_t *args = malloc(sizeof(*args));
        if (!args) {
            break;
        }
        args->runtime = runtime;
        args->slot = i;
        int error = pthread_create(runtime->threads + i, NULL, worker, args);
        if (error) {
            free(args);
            errno = error;
            break;
        }
        runtime->started++;
    }
    pthread_sigmask(SIG_SETMASK, &original, NULL);
    if (runtime->started != options->workers) {
        runtime_destroy(runtime);
        return NULL;
    }
    return runtime;
}
int runtime_reload(runtime_t *runtime, const config_t *config) {
    snapshot_t *next = snapshot_new(config, runtime->current->generation + 1);
    if (!next) {
        return -1;
    }
    pthread_mutex_lock(&runtime->mutex);
    snapshot_t *old = runtime->current;
    runtime->current = next;
    pthread_mutex_unlock(&runtime->mutex);
    release(old);
    return 0;
}
static int observe_event(const file_event_t *, runtime_t *, const rule_input_t *);
int runtime_observe(const file_event_t *event, void *data) {
    runtime_t *runtime = data;
    snapshot_t *snapshot = runtime->current;
    audit_context(snapshot->config.watch_path, event->filename, type_name(event->type), snapshot->generation,
                  runtime->options.dry_run, NULL);
    if (event->type == EVENT_CLOSE_WRITE) {
        rule_input_t closed;
        prepare_rule_input(snapshot->config.watch_path, event->filename, event->type, &closed);
        audit_metadata(closed.metadata_valid ? &closed.metadata : NULL);
        audit_emit("file_closed", "observed", "FW_OK", 0,
                   "writable file descriptor closed; not a durability guarantee", 0, 0, NULL);
        pthread_mutex_lock(&runtime->mutex);
        for (job_t *job = runtime->pending; job; job = job->next) {
            if (!strcmp(job->event.filename, event->filename)) {
                job->closed = 1;
                job->check_after = 0;
            }
        }
        pthread_mutex_unlock(&runtime->mutex);
        if (snapshot->saved_enabled && *event->filename) {
            file_event_t saved = *event;
            saved.type = EVENT_SAVED;
            return observe_event(&saved, runtime, &closed);
        }
        return 0;
    }
    if (event->type == EVENT_OVERFLOW || event->type == EVENT_WATCH_LOST) {
        pthread_mutex_lock(&runtime->mutex);
        runtime->dropped++;
        pthread_mutex_unlock(&runtime->mutex);
        audit_emit("event_dropped", "failure",
                   event->type == EVENT_OVERFLOW ? "FW_INOTIFY_OVERFLOW" : "FW_WATCH_LOST", 0,
                   "filesystem observation is incomplete", 0, 0, NULL);
        return event->type == EVENT_WATCH_LOST ? -1 : 0;
    }
    return observe_event(event, runtime, NULL);
}
static int observe_event(const file_event_t *event, runtime_t *runtime, const rule_input_t *known) {
    snapshot_t *snapshot = runtime->current;
    audit_context(snapshot->config.watch_path, event->filename, type_name(event->type), snapshot->generation,
                  runtime->options.dry_run, NULL);
    if (!*event->filename || event->type == EVENT_UNKNOWN) {
        return 0;
    }
    pthread_mutex_lock(&runtime->mutex);
    runtime->observed++;
    pthread_mutex_unlock(&runtime->mutex);
    rule_input_t observed;
    if (known) {
        observed = *known;
    } else {
        prepare_rule_input(snapshot->config.watch_path, event->filename, event->type, &observed);
    }
    audit_metadata(observed.metadata_valid ? &observed.metadata : NULL);
    char id[AUDIT_ID_SIZE];
    audit_emit("file_observed", "observed", "FW_OK", 0, "kernel event received", 0, 0, id);
    if (audit_failed()) {
        return -1;
    }
    audit_context(snapshot->config.watch_path, event->filename, type_name(event->type), snapshot->generation,
                  runtime->options.dry_run, id);
    rule_result_t filter_result = evaluate_filters(&snapshot->config, event->filename);
    const char *filter = rule_result_description(filter_result);
    if (filter_result != RULE_MATCH) {
        audit_context(snapshot->config.watch_path, event->filename, type_name(event->type),
                      snapshot->generation, runtime->options.dry_run, id);
        audit_emit("event_filtered", "skipped", "FW_PATH_FILTERED", 0, filter, 0, 0, NULL);
        return 0;
    }
    pthread_mutex_lock(&runtime->mutex);
    if (event->type == EVENT_MODIFIED) {
        for (job_t *job = runtime->pending; job; job = job->next) {
            if (job->event.type == EVENT_MODIFIED && job->snapshot == snapshot &&
                strcmp(job->event.filename, event->filename) == 0) {
                job->stable_since = clock_ms();
                job->closed = 0;
                job->ready =
                    runtime->options.ready_mode == 2 || (job->have_stat && !S_ISREG(job->last.st_mode));
                job->check_after = 0;
                pthread_mutex_unlock(&runtime->mutex);
                audit_emit("event_coalesced", "skipped", "FW_EVENT_COALESCED", 0,
                           "pending modification updated", 0, 0, NULL);
                return 0;
            }
        }
    }
    if (runtime->pending_count + runtime->active_count >= runtime->options.capacity) {
        runtime->dropped++;
        pthread_mutex_unlock(&runtime->mutex);
        audit_emit("event_dropped", "failure", "FW_QUEUE_FULL", 0, "bounded action queue is full", 0, 0,
                   NULL);
        return 0;
    }
    job_t *job = calloc(1, sizeof(*job));
    if (!job) {
        pthread_mutex_unlock(&runtime->mutex);
        return -1;
    }
    job->event = *event;
    job->snapshot = snapshot;
    atomic_fetch_add(&snapshot->references, 1);
    snprintf(job->id, sizeof(job->id), "%s", id);
    job->received = job->stable_since = clock_ms();
    job->closed = event->type == EVENT_SAVED;
    job->have_stat = observed.metadata_valid;
    if (job->have_stat) {
        job->last = observed.metadata;
    }
    int content_event = event->type == EVENT_CREATED || event->type == EVENT_MODIFIED ||
                        event->type == EVENT_MOVED_TO || event->type == EVENT_SAVED;
    job->ready =
        !content_event || runtime->options.ready_mode == 2 || (job->have_stat && !S_ISREG(job->last.st_mode));
    append_job(runtime, job);
    if (job->ready) {
        pthread_cond_broadcast(&runtime->changed);
    }
    pthread_mutex_unlock(&runtime->mutex);
    return 0;
}
void runtime_tick(runtime_t *runtime) {
    long long now = clock_ms();
    int became_ready = 0;
    pthread_mutex_lock(&runtime->mutex);
    job_t *job = runtime->pending;
    while (job) {
        if (job->ready ||
            (now < job->check_after && now - job->received < runtime->options.ready_timeout_ms)) {
            job = job->next;
            continue;
        }
        /* An unready job cannot be taken by workers. Main-loop ownership keeps
           this job alive while I/O runs without the queue lock.
           Other ready nodes may disappear, so read next/previous only after relocking. */
        pthread_mutex_unlock(&runtime->mutex);
        struct stat current;
        int stat_error = stat_job(job, &current) ? errno : 0;
        int discard = 0, ready = 0;
        context_for(job, runtime->options.dry_run);
        if (stat_error) {
            audit_emit("event_cancelled", "skipped", "FW_FILE_NOT_FOUND", stat_error,
                       "file vanished before readiness", 0, 0, NULL);
            discard = 1;
        } else if (job->have_stat &&
                   (current.st_ino != job->last.st_ino || current.st_dev != job->last.st_dev)) {
            audit_emit("event_cancelled", "skipped", "FW_SOURCE_REPLACED", ESTALE,
                       "file replaced before readiness", 0, 0, NULL);
            discard = 1;
        } else if (now - job->received >= runtime->options.ready_timeout_ms) {
            audit_emit("event_cancelled", "failure", "FW_READY_TIMEOUT", ETIMEDOUT,
                       "file did not become ready", 0, 0, NULL);
            discard = 1;
        } else {
            if (!job->have_stat || !unchanged(&current, &job->last)) {
                job->stable_since = now;
            }
            job->last = current;
            job->have_stat = 1;
            int close_ok =
                runtime->options.ready_mode == 1 || job->closed || job->event.type == EVENT_MOVED_TO;
            ready = close_ok && now - job->stable_since >= runtime->options.settle_ms;
            unsigned delay = close_ok ? runtime->options.settle_ms : 250;
            if (delay < 50) {
                delay = 50;
            }
            if (delay > 250) {
                delay = 250;
            }
            job->check_after = now + delay;
            if (ready) {
                audit_metadata(&current);
                audit_emit("file_ready", "ready", "FW_OK", 0,
                           job->closed ? "close_write and stability confirmed" : "stability confirmed", 0,
                           now - job->received, NULL);
            }
        }
        pthread_mutex_lock(&runtime->mutex);
        job_t *next = job->next;
        if (discard) {
            became_ready = 1; /* Removing a head can release an already-ready follower. */
            remove_pending(runtime, job);
            finish_job(job);
        } else if (ready) {
            job->ready = 1;
            became_ready = 1;
        }
        job = next;
    }
    if (became_ready || runtime->pending_count) {
        pthread_cond_broadcast(&runtime->changed);
    }
    pthread_mutex_unlock(&runtime->mutex);
}
void runtime_status(runtime_t *runtime, runtime_status_t *status) {
    pthread_mutex_lock(&runtime->mutex);
    *status = (runtime_status_t){.workers = runtime->started,
                                 .queued = runtime->pending_count,
                                 .active = runtime->active_count,
                                 .capacity = runtime->options.capacity,
                                 .generation = runtime->current->generation,
                                 .observed = runtime->observed,
                                 .dropped = runtime->dropped,
                                 .completed = runtime->completed,
                                 .action_failures = runtime->action_failures};
    pthread_mutex_unlock(&runtime->mutex);
}
void runtime_destroy(runtime_t *runtime) {
    atomic_store(&runtime->cancel, 1);
    pthread_mutex_lock(&runtime->mutex);
    pthread_cond_broadcast(&runtime->changed);
    pthread_mutex_unlock(&runtime->mutex);
    for (unsigned i = 0; i < runtime->started; i++) {
        pthread_join(runtime->threads[i], NULL);
    }
    while (runtime->pending) {
        job_t *job = runtime->pending;
        remove_pending(runtime, job);
        context_for(job, runtime->options.dry_run);
        audit_emit("event_cancelled", "cancelled", "FW_SHUTDOWN", ECANCELED,
                   "pending action cancelled on shutdown", 0, 0, NULL);
        finish_job(job);
    }
    release(runtime->current);
    action_set_cancel_flag(NULL);
    pthread_mutex_destroy(&runtime->mutex);
    state_store_destroy(runtime->state);
    pthread_cond_destroy(&runtime->changed);
    free(runtime);
}
