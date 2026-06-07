#include "action.h"
#include "config.h"
#include "glob.h"
#include "log.h"
#include "watcher.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>

static volatile sig_atomic_t running = 1;

static void handle_signal(int signal_number) {
    (void)signal_number;
    running = 0;
}

static const char *event_type_to_string(event_type_t type) {
    switch (type) {
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
    default:
        return "unknown";
    }
}

static int print_event_callback(const file_event_t *event, void *user_data) {
    (void)user_data;

    if (event->filename[0] != '\0') {
        printf("[%s] %s\n", event_type_to_string(event->type), event->filename);
    } else {
        printf("[%s]\n", event_type_to_string(event->type));
    }
    fflush(stdout);
    return 0;
}

typedef struct {
    config_t *config;
    int dry_run;
} runtime_context_t;

static int handle_file_event(const file_event_t *event, void *user_data) {
    runtime_context_t *context = (runtime_context_t *)user_data;
    config_t *config = context->config;
    if (event->filename[0] == '\0') {
        return 0;
    }

    for (int i = 0; i < config->rule_count; i++) {
        rule_t *rule = &config->rules[i];

        if (rule->event != event->type) {
            continue;
        }

        if (!glob_match(rule->path_glob, event->filename)) {
            continue;
        }

        execute_action(&rule->action, config->watch_path, event->filename, event->type, context->dry_run);
    }

    return 0;
}

static void print_usage(const char *program_name) {
    fprintf(stderr, "Usage: %s [--config <file>] [--dry-run] <directory_to_watch>\n", program_name);
}

int main(int argc, char *argv[]) {
    const char *config_path = NULL;
    const char *watch_path = NULL;
    int dry_run = 0;
    config_t config;
    runtime_context_t context;

    config.watch_path[0] = '\0';
    config.rule_count = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0) {
            if (i + 1 >= argc) {
                print_usage(argv[0]);
                return 1;
            }
            config_path = argv[++i];
        } else if (strcmp(argv[i], "--dry-run") == 0) {
            dry_run = 1;
        } else if (watch_path == NULL) {
            watch_path = argv[i];
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    if (config_path != NULL && watch_path != NULL) {
        fprintf(stderr, "Cannot use --config and explicit watch path together.\n");
        print_usage(argv[0]);
        return 1;
    }

    if (config_path == NULL && watch_path == NULL) {
        print_usage(argv[0]);
        return 1;
    }

    log_init("fileward");

    if (config_path != NULL) {
        if (load_config(config_path, &config) != 0) {
            return 1;
        }
        watch_path = config.watch_path;
    } else {
        strncpy(config.watch_path, watch_path, sizeof(config.watch_path));
        config.watch_path[sizeof(config.watch_path) - 1] = '\0';
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    if (start_watcher(watch_path) != 0) {
        return 1;
    }

    log_info("fileward started. Press Ctrl+C to stop.%s", dry_run ? " (dry-run mode)" : "");

    context.config = &config;
    context.dry_run = dry_run;

    int (*event_callback)(const file_event_t *, void *) = config.rule_count > 0 ? handle_file_event : print_event_callback;

    while (running) {
        if (watcher_process_events(500, event_callback, &context) != 0) {
            stop_watcher();
            return 1;
        }
    }

    stop_watcher();
    log_info("fileward stopped.");

    return 0;
}
