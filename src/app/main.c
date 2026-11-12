#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include "daemon.h"
#include "service.h"
#include <fileward/control.h>
#include <fileward/action.h>
#include <fileward/config.h>
#include <fileward/rules.h>
#include <fileward/log.h>
#include <fileward/watcher.h>
#include <fileward/runtime.h>
#include <fileward/audit.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef FILEWARD_VERSION
#define FILEWARD_VERSION "development"
#endif
#define EXIT_OK 0
#define EXIT_USAGE 1
#define EXIT_CONFIG 2
#define EXIT_RUNTIME 3

static volatile sig_atomic_t running = 1;
static volatile sig_atomic_t reload_requested = 0;

static void handle_signal(int signal_number) {
    if (signal_number == SIGHUP) {
        reload_requested = 1;
        return;
    }

    running = 0;
}

static const char *event_type_to_string(event_type_t type) {
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
    default:
        return "unknown";
    }
}

static event_type_t parse_event_type(const char *token) {
    if (strcmp(token, "saved") == 0) {
        return EVENT_SAVED;
    }
    if (strcmp(token, "created") == 0) {
        return EVENT_CREATED;
    }
    if (strcmp(token, "modified") == 0) {
        return EVENT_MODIFIED;
    }
    if (strcmp(token, "deleted") == 0) {
        return EVENT_DELETED;
    }
    if (strcmp(token, "moved_from") == 0) {
        return EVENT_MOVED_FROM;
    }
    if (strcmp(token, "moved_to") == 0) {
        return EVENT_MOVED_TO;
    }
    return EVENT_UNKNOWN;
}

static int is_help_arg(const char *arg) {
    return arg != NULL && (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0 || strcmp(arg, "help") == 0);
}

static void print_usage(const char *program_name) {
    fprintf(stderr, "Usage: %s <subcommand> [options]\n", program_name);
    fprintf(stderr,
            "       %s run [--config <file>] [--dry-run] [--daemon] [--pidfile <file>] [--log-file <file>] "
            "[--state-file <file>] [--control-socket <path>] [--systemd] [<directory_to_watch>]\n",
            program_name);
    fprintf(stderr,
            "       %s start [--config <file>] [--dry-run] [--pidfile <file>] [--log-file <file>] "
            "[--state-file <file>] [--control-socket <path>] [--systemd] [<directory_to_watch>]\n",
            program_name);
    fprintf(stderr, "       %s stop [--pidfile <file>]\n", program_name);
    fprintf(stderr, "       %s status [--pidfile <file>]\n", program_name);
    fprintf(stderr, "       %s reload [--socket <path> | --pidfile <file>]\n", program_name);
    fprintf(stderr, "       %s health [--socket <path>]\n", program_name);
    fprintf(stderr, "       %s history [--journal <file>] [--all] [--limit <count>] [--event-id <id>]\n",
            program_name);
    fprintf(stderr, "       %s stats [--journal <file>]\n", program_name);
    fprintf(stderr, "       %s actions\n", program_name);
    fprintf(stderr, "       %s test [--config <file>] <path>\n", program_name);
    fprintf(stderr, "       %s validate --config <file> [--check-watch]\n", program_name);
    fprintf(stderr, "       %s explain --config <file> [--event <event>] <path>\n", program_name);
}

static void print_run_usage(const char *program_name) {
    fprintf(stderr,
            "Usage: %s run [--config <file>] [--dry-run] [--daemon] [--pidfile <file>] [--log-file <file>] "
            "[--state-file <file>] [--control-socket <path>] [--systemd] [<directory_to_watch>]\n",
            program_name);
}

static void print_test_usage(const char *program_name) {
    fprintf(stderr, "Usage: %s test [--config <file>] <path>\n", program_name);
}

static void print_explain_usage(const char *program_name) {
    fprintf(stderr, "Usage: %s explain --config <file> [--event <event>] <path>\n", program_name);
}

static void print_help(const char *program_name) {
    print_usage(program_name);
    fprintf(stderr, "\nSubcommands:\n");
    fprintf(stderr, "  run      start the watcher in the foreground\n");
    fprintf(stderr, "  start    start the watcher as a background daemon\n");
    fprintf(stderr, "  stop     stop a running daemon\n");
    fprintf(stderr, "  status   show whether the daemon is running\n");
    fprintf(stderr, "  reload   reload config for an already-running daemon\n");
    fprintf(stderr, "  test     validate a path and show rule matches for a config\n");
    fprintf(stderr, "  validate validate config syntax without starting a watcher\n");
    fprintf(stderr, "  explain  explain which rules would match a path for an event\n");
    fprintf(stderr, "  history  show recent action results as JSONL\n  stats    aggregate counters from the "
                    "retained journal\n");
    fprintf(stderr, "  health   live status via the control socket (exit 4 when degraded)\n");
    fprintf(stderr, "  actions  list registered action handlers\n");
    fprintf(stderr, "\nRuntime options: --control-socket <path> --systemd\n");
    fprintf(stderr, "\nRuntime options: --journal <file> --journal-max-bytes <bytes> --workers 1..32\n");
    fprintf(stderr, "  --journal-sync: fdatasync every structured record\n");
    fprintf(stderr, "  --queue-size 1..4096 --ready-mode close-write|stable|immediate\n");
    fprintf(stderr, "  --settle-ms 0..60000 --ready-timeout-ms 100..3600000\n");
    fprintf(stderr, "\nOptions:\n");
    fprintf(stderr, "  --config <file>    load rules from a config file\n");
    fprintf(stderr, "  --check-watch      also check watch directory existence and access in validate\n");
    fprintf(stderr, "  --dry-run          do not execute actions when running\n");
    fprintf(stderr, "  --daemon           detach and run in the background\n");
    fprintf(stderr, "  --pidfile <file>   write/read the daemon pidfile\n");
    fprintf(stderr, "  --log-file <file>  redirect logs to a file\n");
    fprintf(stderr, "  --state-file <file> append handled events to a journal file\n");
    fprintf(stderr, "  --event <event>    event type for explain: created, modified, saved, deleted, "
                    "moved_from, moved_to\n");
    fprintf(stderr, "  The watch directory is required unless --config supplies it.\n");
    fprintf(stderr, "\nExamples:\n");
    fprintf(stderr, "  %s run --config fileward.conf\n", program_name);
    fprintf(stderr, "  %s start --daemon --config fileward.conf ~/Downloads\n", program_name);
    fprintf(stderr, "  %s test --config fileward.conf ~/Downloads/report.pdf\n", program_name);
    fprintf(stderr, "  %s explain --config fileward.conf --event created docs/report.pdf\n", program_name);
}

static int expand_path(const char *path, char *out, size_t out_size) {
    if (path == NULL || out == NULL || out_size == 0) {
        return -1;
    }

    if (path[0] == '~' && (path[1] == '/' || path[1] == '\0')) {
        const char *home = getenv("HOME");
        if (home == NULL) {
            return -1;
        }

        if (snprintf(out, out_size, "%s%s", home, path + 1) >= (int)out_size) {
            return -1;
        }
        return 0;
    }

    if (strlen(path) >= out_size) {
        return -1;
    }

    strcpy(out, path);
    return 0;
}

static int load_watch_config(const char *config_path, const char *watch_path, config_t *config,
                             const char **out_watch_path) {
    char expanded_watch_path[PATH_MAX];

    if (config_path != NULL) {
        if (load_config(config_path, config) != 0) {
            return -1;
        }

        if (watch_path != NULL) {
            if (expand_path(watch_path, expanded_watch_path, sizeof(expanded_watch_path)) != 0) {
                return -1;
            }
            strncpy(config->watch_path, expanded_watch_path, sizeof(config->watch_path));
            config->watch_path[sizeof(config->watch_path) - 1] = '\0';
        }

        *out_watch_path = config->watch_path;
        return 0;
    }

    if (watch_path == NULL) {
        return -1;
    }

    config->watch_path[0] = '\0';
    config->rule_count = 0;
    config->filter_count = 0;
    config->first_match = 0;
    if (expand_path(watch_path, expanded_watch_path, sizeof(expanded_watch_path)) != 0) {
        return -1;
    }
    strncpy(config->watch_path, expanded_watch_path, sizeof(config->watch_path));
    config->watch_path[sizeof(config->watch_path) - 1] = '\0';
    *out_watch_path = config->watch_path;
    return 0;
}

static int path_within_root(const char *root, const char *target, char *relative, size_t relative_size) {
    char resolved_root[PATH_MAX];
    char resolved_target[PATH_MAX];

    if (realpath(root, resolved_root) == NULL) {
        return -1;
    }
    /* Resolve parents, preserving the final directory entry (including symlinks).
     * This also lets explain inspect a removed file when its parent still exists. */
    char target_copy[PATH_MAX];
    if (strlen(target) >= sizeof(target_copy) || target[0] == '\0') {
        return -1;
    }
    strcpy(target_copy, target);
    size_t length = strlen(target_copy);
    while (length > 1 && target_copy[length - 1] == '/') {
        target_copy[--length] = '\0';
    }
    char *slash = strrchr(target_copy, '/');
    const char *leaf = slash ? slash + 1 : target_copy;
    if (*leaf == '\0' || strcmp(leaf, ".") == 0 || strcmp(leaf, "..") == 0) {
        if (realpath(target_copy, resolved_target) == NULL) {
            return -1;
        }
    } else {
        char leaf_copy[PATH_MAX];
        strcpy(leaf_copy, leaf);
        const char *parent = ".";
        if (slash) {
            if (slash == target_copy) {
                parent = "/";
            } else {
                *slash = '\0';
                parent = target_copy;
            }
        }
        char resolved_parent[PATH_MAX];
        if (realpath(parent, resolved_parent) == NULL) {
            return -1;
        }
        struct stat parent_stat;
        if (stat(resolved_parent, &parent_stat) != 0 || !S_ISDIR(parent_stat.st_mode)) {
            return -1;
        }
        int written = snprintf(resolved_target, sizeof(resolved_target), "%s%s%s", resolved_parent,
                               strcmp(resolved_parent, "/") == 0 ? "" : "/", leaf_copy);
        if (written < 0 || (size_t)written >= sizeof(resolved_target)) {
            return -1;
        }
    }

    size_t root_len = strlen(resolved_root);
    if (strncmp(resolved_root, resolved_target, root_len) != 0 ||
        (root_len > 1 && resolved_target[root_len] != '/' && resolved_target[root_len] != '\0')) {
        /* The configured root itself may be reached through a symlink alias. */
        char canonical_target[PATH_MAX];
        if (realpath(target, canonical_target) == NULL || strcmp(canonical_target, resolved_root) != 0) {
            return 0;
        }
        strcpy(resolved_target, resolved_root);
    }

    const char *relative_path = resolved_target + root_len;
    if (*relative_path == '/') {
        relative_path++;
    }

    if (strlen(relative_path) >= relative_size) {
        return -1;
    }

    strcpy(relative, relative_path);
    return 1;
}

static int describe_matching_rules(const char *relative_path, event_type_t event, const config_t *config,
                                   int explain) {
    rule_result_t filter_result = evaluate_filters(config, relative_path);
    if (filter_result != RULE_MATCH) {
        printf("path skipped: %s\n", rule_result_description(filter_result));
        return 0;
    }
    rule_input_t input;
    prepare_rule_input(config->watch_path, relative_path, event, &input);
    int order[FILEWARD_MAX_RULES];
    order_rules(config, order);
    int matched = 0, stopped = 0;
    for (int i = 0; i < config->rule_count; i++) {
        const rule_t *rule = &config->rules[order[i]];
        if (stopped) {
            if (explain) {
                printf("Rule %s (line %d): skipped: rule chain stopped\n", rule->name, rule->line_no);
            }
            continue;
        }
        rule_result_t result = evaluate_rule(rule, event, &input);
        if (result != RULE_MATCH) {
            if (explain) {
                printf("Rule %s (line %d): skipped: %s\n", rule->name, rule->line_no,
                       rule_result_description(result));
                printf("  event: %s\n  pattern: %s\n", event_type_to_string(rule->event), rule->path_glob);
            }
            continue;
        }

        matched++;
        printf("Match %d:\n", matched);
        printf("  rule: %s (line %d)\n", rule->name, rule->line_no);
        printf("  priority: %d\n", rule->priority);
        printf("  event: %s\n", event_type_to_string(rule->event));
        printf("  pattern: %s\n", rule->path_glob);
        printf("  action: %s\n", action_type_name(rule->action.type));
        if (rule->action.type == ACTION_MOVE || rule->action.type == ACTION_COPY ||
            rule->action.type == ACTION_ARCHIVE) {
            printf("  target: %s\n", rule->action.target);
            char preview[PATH_MAX];
            if (action_expand_target(rule->action.target, config->watch_path, relative_path, event,
                                     preview) == 0) {
                printf("  expanded target: %s\n", preview);
            }
            printf("  conflict: %s\n  retries: %d (backoff %dms)\n",
                   conflict_policy_name(rule->action.conflict), rule->action.retries,
                   rule->action.backoff_ms);
        } else if (rule->action.type == ACTION_EXEC) {
            printf("  command: %s\n  timeout: %ds\n", rule->action.command, rule->action.timeout_seconds);
        } else if (rule->action.type == ACTION_LOG && rule->action.message[0] != '\0') {
            printf("  message: %s\n", rule->action.message);
        }
        if (config->first_match || rule->stop) {
            stopped = 1;
            if (explain) {
                printf("  stops rule chain\n");
            }
        }
    }

    return matched;
}

static int canonical_config_root(config_t *config) {
    char root[PATH_MAX];
    if (!realpath(config->watch_path, root)) {
        return -1;
    }
    snprintf(config->watch_path, sizeof(config->watch_path), "%s", root);
    return 0;
}

/* Syntax validation remains usable before deployment. The optional preflight and
 * runtime startup share the same checks, without creating any watches or files. */
static int check_watch_root(config_t *config) {
    char root[PATH_MAX];
    if (!realpath(config->watch_path, root)) {
        return -1;
    }
    struct stat st;
    if (stat(root, &st)) {
        return -1;
    }
    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    if (access(root, R_OK | X_OK)) {
        return -1;
    }
    snprintf(config->watch_path, sizeof(config->watch_path), "%s", root);
    return 0;
}

static void startup_error(const char *kind, const char *code, int error, const char *path,
                          const char *reason) {
    char message[PATH_MAX + 512];
    snprintf(message, sizeof(message), "%s: %s%s%s", path ? path : "", reason,
             error ? ": " : "", error ? strerror(error) : "");
    /* Do not rely on the audit journal being visible to the service operator. */
    fprintf(stderr, "%s: %s\n", code, message);
    audit_emit(kind, "failure", code, error, message, 0, 0, NULL);
    errno = error;
}

typedef struct {
    runtime_t *runtime;
    config_t *config;
    const char *config_path, *watch_path;
    unsigned generation;
    int dry_run;
    struct timespec started;
} application_t;

static int application_reload(application_t *application) {
    if (!application->config_path) {
        errno = EINVAL;
        return -1;
    }
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    char notification[160];
    snprintf(notification, sizeof(notification),
             "RELOADING=1\nMONOTONIC_USEC=%llu\nSTATUS=Reloading configuration",
             (unsigned long long)now.tv_sec * 1000000 + now.tv_nsec / 1000);
    if (service_notify(notification)) {
        return -1;
    }
    config_t next;
    const char *root = NULL;
    audit_context(application->config->watch_path, "", "", application->generation, application->dry_run,
                  NULL);
    int error = load_watch_config(application->config_path, application->watch_path, &next, &root) ||
                canonical_config_root(&next) || strcmp(next.watch_path, application->config->watch_path);
    if (!error && runtime_reload(application->runtime, &next)) {
        error = 1;
    }
    if (error) {
        log_error("failed to reload config: %s; keeping previous rules", application->config_path);
        audit_emit("config_reload", "failure", "FW_RELOAD_REJECTED", 0,
                   "configuration rejected; previous generation retained", 0, 0, NULL);
        (void)service_notify("READY=1\nSTATUS=Reload rejected; previous configuration remains active");
        errno = EINVAL;
        return -1;
    }
    *application->config = next;
    application->generation++;
    audit_context(application->config->watch_path, "", "", application->generation, application->dry_run,
                  NULL);
    audit_emit("config_reload", "success", "FW_OK", 0, "new immutable configuration published", 0, 0, NULL);
    log_info("reloaded config: %s", application->config_path);
    return service_notify("READY=1\nSTATUS=Configuration reloaded");
}
static int application_control(void *data, const char *command, char *out, size_t size) {
    application_t *application = data;
    if (strcmp(command, "reload") == 0) {
        int error = application_reload(application);
        snprintf(
            out, size, "{\"schema_version\":1,\"status\":\"%s\",\"code\":\"%s\",\"config_generation\":%u}\n",
            error ? "error" : "reloaded", error ? "FW_RELOAD_REJECTED" : "FW_OK", application->generation);
        return error;
    }
    if (strcmp(command, "health") != 0) {
        snprintf(out, size, "{\"schema_version\":1,\"status\":\"error\",\"code\":\"FW_CONTROL_REQUEST\"}\n");
        return -1;
    }
    runtime_status_t status;
    runtime_status(application->runtime, &status);
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int audit_ok = !audit_failed();
    int healthy = audit_ok && !watcher_degraded() && !status.dropped;
    snprintf(out, size,
             "{\"schema_version\":1,\"provider\":\"fileward\",\"program_version\":\"%s\",\"status\":\"%s\","
             "\"code\":\"%s\",\"pid\":%ld,\"uptime_seconds\":%lld,\"audit_healthy\":%s,\"watch_coverage_ok\":"
             "%s,\"watched_directories\":%d,\"workers\":%u,\"queued\":%u,\"active\":%u,\"capacity\":%u,"
             "\"config_generation\":%u,\"events_observed\":%llu,\"events_dropped\":%llu,\"chains_completed\":"
             "%llu,\"action_failures\":%llu}\n",
             FILEWARD_VERSION, healthy ? "healthy" : "degraded", healthy ? "FW_OK" : "FW_HEALTH_DEGRADED",
             (long)getpid(), (long long)(now.tv_sec - application->started.tv_sec),
             audit_ok ? "true" : "false", watcher_degraded() ? "false" : "true", watcher_watch_count(),
             status.workers, status.queued, status.active, status.capacity, status.generation,
             status.observed, status.dropped, status.completed, status.action_failures);
    return 0;
}

static int run_command(int argc, char *argv[], int daemon_mode) {
    const char *config_path = NULL;
    const char *watch_path = NULL;
    const char *pidfile_path = NULL;
    const char *log_file_path = NULL;
    const char *state_file_path = NULL;
    int dry_run = 0;
    config_t config;
    runtime_t *runtime = NULL;
    runtime_options_t options = {.workers = 4, .capacity = 256, .settle_ms = 100, .ready_timeout_ms = 300000};
    char journal_default[PATH_MAX];
    const char *journal_path = NULL, *control_path = NULL;
    char control_default[PATH_MAX];
    int systemd_mode = 0;
    control_server_t *control = NULL;
    int journal_sync = 0;
    uint64_t journal_limit = 64ULL * 1024 * 1024;
    const char *watch_root = NULL;

    memset(&config, 0, sizeof(config));

    for (int i = 0; i < argc; i++) {
        if (is_help_arg(argv[i])) {
            print_run_usage("fileward");
            return EXIT_OK;
        }

        if (strcmp(argv[i], "--config") == 0) {
            if (i + 1 >= argc) {
                print_run_usage("fileward");
                return EXIT_USAGE;
            }
            config_path = argv[++i];
        } else if (strcmp(argv[i], "--dry-run") == 0) {
            dry_run = 1;
        } else if (strcmp(argv[i], "--daemon") == 0) {
            daemon_mode = 1;
        } else if (strcmp(argv[i], "--pidfile") == 0) {
            if (i + 1 >= argc) {
                print_run_usage("fileward");
                return EXIT_USAGE;
            }
            pidfile_path = argv[++i];
        } else if (strcmp(argv[i], "--log-file") == 0) {
            if (i + 1 >= argc) {
                print_run_usage("fileward");
                return EXIT_USAGE;
            }
            log_file_path = argv[++i];
        } else if (strcmp(argv[i], "--state-file") == 0) {
            if (i + 1 >= argc) {
                print_run_usage("fileward");
                return EXIT_USAGE;
            }
            state_file_path = argv[++i];
        } else if (strcmp(argv[i], "--control-socket") == 0) {
            if (++i >= argc) {
                return EXIT_USAGE;
            }
            control_path = argv[i];
        } else if (strcmp(argv[i], "--systemd") == 0) {
            systemd_mode = 1;
        } else if (strcmp(argv[i], "--journal-sync") == 0) {
            journal_sync = 1;
        } else if (strcmp(argv[i], "--journal") == 0) {
            if (++i >= argc) {
                return EXIT_USAGE;
            }
            journal_path = argv[i];
        } else if (strcmp(argv[i], "--ready-mode") == 0) {
            if (++i >= argc) {
                return EXIT_USAGE;
            }
            if (strcmp(argv[i], "close-write") == 0) {
                options.ready_mode = 0;
            } else if (strcmp(argv[i], "stable") == 0) {
                options.ready_mode = 1;
            } else if (strcmp(argv[i], "immediate") == 0) {
                options.ready_mode = 2;
            } else {
                return EXIT_USAGE;
            }
        } else if (strcmp(argv[i], "--workers") == 0 || strcmp(argv[i], "--queue-size") == 0 ||
                   strcmp(argv[i], "--settle-ms") == 0 || strcmp(argv[i], "--ready-timeout-ms") == 0 ||
                   strcmp(argv[i], "--journal-max-bytes") == 0) {
            const char *option = argv[i];
            if (++i >= argc || argv[i][0] < '0' || argv[i][0] > '9') {
                return EXIT_USAGE;
            }
            char *end;
            errno = 0;
            unsigned long long number = strtoull(argv[i], &end, 10);
            if (errno || *end) {
                return EXIT_USAGE;
            }
            if (strcmp(option, "--workers") == 0) {
                if (number < 1 || number > 32) {
                    return EXIT_USAGE;
                }
                options.workers = number;
            } else if (strcmp(option, "--queue-size") == 0) {
                if (number < 1 || number > 4096) {
                    return EXIT_USAGE;
                }
                options.capacity = number;
            } else if (strcmp(option, "--settle-ms") == 0) {
                if (number > 60000) {
                    return EXIT_USAGE;
                }
                options.settle_ms = number;
            } else if (strcmp(option, "--ready-timeout-ms") == 0) {
                if (number < 100 || number > 3600000) {
                    return EXIT_USAGE;
                }
                options.ready_timeout_ms = number;
            } else {
                if (number < 4096 || number > 4294967296ULL) {
                    return EXIT_USAGE;
                }
                journal_limit = number;
            }
        } else if (watch_path == NULL) {
            watch_path = argv[i];
        } else {
            print_run_usage("fileward");
            return EXIT_USAGE;
        }
    }

    if (options.ready_timeout_ms <= options.settle_ms) {
        return EXIT_USAGE;
    }
    if (systemd_mode && daemon_mode) {
        return EXIT_USAGE;
    }
    if (service_initialize(systemd_mode)) {
        fprintf(stderr, "FW_SYSTEMD_NOTIFY: missing or invalid systemd environment\n");
        return EXIT_RUNTIME;
    }
    if (!control_path) {
        if (journal_path) {
            int size = snprintf(control_default, sizeof(control_default), "%s.sock", journal_path);
            if (size < 0 || size >= (int)sizeof(control_default)) {
                return EXIT_USAGE;
            }
        } else if (control_default_path(control_default, sizeof(control_default))) {
            return EXIT_USAGE;
        }
        control_path = control_default;
    }
    if (!journal_path) {
        const char *state_home = getenv("XDG_STATE_HOME");
        const char *home = getenv("HOME");
        int size =
            state_home && *state_home
                ? snprintf(journal_default, sizeof(journal_default), "%s/fileward/events.jsonl", state_home)
                : snprintf(journal_default, sizeof(journal_default), "%s/.local/state/fileward/events.jsonl",
                           home ? home : ".");
        if (size < 0 || size >= (int)sizeof(journal_default)) {
            return EXIT_USAGE;
        }
        journal_path = journal_default;
    }
    audit_set_sync(journal_sync);
    if (audit_open(journal_path, journal_limit)) {
        fprintf(stderr, "FW_JOURNAL_OPEN: %s: %s\n", journal_path, strerror(errno));
        return EXIT_RUNTIME;
    }
    if (load_watch_config(config_path, watch_path, &config, &watch_root) != 0) {
        audit_context(config.watch_path, "", "", 1, dry_run, NULL);
        startup_error("config_load", "FW_CONFIG_INVALID", 0, config_path, "configuration rejected");
        audit_close();
        return EXIT_CONFIG;
    }

    audit_context(config.watch_path, "", "", 1, dry_run, NULL);
    if (check_watch_root(&config)) {
        startup_error("watcher_error", "FW_WATCHER_START", errno, watch_root,
                      "watch root is not an accessible directory");
        audit_close();
        return EXIT_RUNTIME;
    }
    audit_context(config.watch_path, "", "", 1, dry_run, NULL);
    const char *artifacts[] = {journal_path, state_file_path, log_file_path, control_path};
    for (unsigned i = 0; i < sizeof(artifacts) / sizeof(artifacts[0]); i++) {
        char relative[PATH_MAX];
        if (artifacts[i] && path_within_root(watch_root, artifacts[i], relative, sizeof(relative)) == 1) {
            startup_error("config_load", "FW_OUTPUT_INSIDE_WATCH", 0, artifacts[i],
                          "journal/log/state/socket must be outside the watched tree");
            audit_close();
            return EXIT_CONFIG;
        }
    }
    if (!daemon_mode && log_file_path != NULL && redirect_output(log_file_path) != 0) {
        startup_error("runtime_error", "FW_LOG_OPEN", errno, log_file_path, "log file could not be opened");
        audit_close();
        return EXIT_RUNTIME;
    }

    if (daemon_mode && pidfile_path == NULL) {
        pidfile_path = "/tmp/fileward.pid";
    }
    if (pidfile_path != NULL && lock_pidfile(pidfile_path) != 0) {
        return EXIT_RUNTIME;
    }

    if (daemon_mode) {
        if (daemonize_process(log_file_path) != 0) {
            release_pidfile();
            return EXIT_RUNTIME;
        }
    } else if (pidfile_path != NULL) {
        if (write_pidfile(getpid()) != 0) {
            release_pidfile();
            return EXIT_RUNTIME;
        }
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGHUP, handle_signal);

    control = control_open(control_path);
    if (!control) {
        startup_error("runtime_error", "FW_CONTROL_LISTEN", errno, control_path,
                      "control socket could not be opened");
        notify_startup(0);
        release_pidfile();
        audit_close();
        return EXIT_RUNTIME;
    }
    char relative_control[PATH_MAX];
    if (path_within_root(watch_root, control_path, relative_control, sizeof(relative_control)) == 1) {
        startup_error("config_load", "FW_OUTPUT_INSIDE_WATCH", 0, control_path,
                      "control socket must be outside watch");
        control_close(control);
        notify_startup(0);
        release_pidfile();
        audit_close();
        return EXIT_CONFIG;
    }
    if (start_watcher(watch_root) != 0) {
        startup_error("watcher_error", "FW_WATCHER_START", errno, watch_root,
                      "watch root could not be initialized");
        audit_close();
        control_close(control);
        notify_startup(0);
        release_pidfile();
        return EXIT_RUNTIME;
    }

    options.dry_run = dry_run;
    options.state_file = state_file_path;
    runtime = runtime_create(&config, &options);
    if (!runtime) {
        startup_error("runtime_error", "FW_WORKER_START", errno, watch_root, "worker pool could not start");
        stop_watcher();
        control_close(control);
        notify_startup(0);
        release_pidfile();
        audit_close();
        return EXIT_RUNTIME;
    }
    audit_context(config.watch_path, "", "", 1, dry_run, NULL);
    audit_emit("daemon_started", "success", "FW_OK", 0, "watcher and workers ready", 0, 0, NULL);
    if (audit_failed()) {
        runtime_destroy(runtime);
        stop_watcher();
        control_close(control);
        notify_startup(0);
        audit_close();
        release_pidfile();
        return EXIT_RUNTIME;
    }
    application_t application = {.runtime = runtime,
                                 .config = &config,
                                 .config_path = config_path,
                                 .watch_path = watch_path,
                                 .generation = 1,
                                 .dry_run = dry_run};
    clock_gettime(CLOCK_MONOTONIC, &application.started);
    if (service_notify("READY=1\nSTATUS=Watching files; worker pool and control socket ready")) {
        startup_error("runtime_error", "FW_SYSTEMD_NOTIFY", errno, watch_root, "readiness notification failed");
        runtime_destroy(runtime);
        control_close(control);
        stop_watcher();
        notify_startup(0);
        release_pidfile();
        audit_close();
        return EXIT_RUNTIME;
    }
    notify_startup(1);
    log_info("fileward started. Press Ctrl+C to stop.%s", dry_run ? " (dry-run mode)" : "");
    int result = EXIT_OK;
    while (running) {
        if (audit_failed()) {
            result = EXIT_RUNTIME;
            break;
        }
        if (reload_requested) {
            reload_requested = 0;
            (void)application_reload(&application);
        }
        control_poll(control, application_control, &application);
        if (service_watchdog()) {
            audit_emit("runtime_error", "failure", "FW_SYSTEMD_NOTIFY", errno, "watchdog notification failed",
                       0, 0, NULL);
            result = EXIT_RUNTIME;
            break;
        }
        if (watcher_process_events(50, runtime_observe, runtime) != 0) {
            audit_emit("watcher_error", "failure", "FW_WATCHER_IO", errno, "watcher stopped after an error",
                       0, 0, NULL);
            result = EXIT_RUNTIME;
            break;
        }
        runtime_tick(runtime);
    }
    (void)service_notify("STOPPING=1\nSTATUS=Stopping workers");
    control_close(control);
    runtime_destroy(runtime);
    audit_context(config.watch_path, "", "", application.generation, dry_run, NULL);
    stop_watcher();
    audit_emit("daemon_stopped", result ? "failure" : "success", result ? "FW_RUNTIME_FAILED" : "FW_OK", 0,
               "workers joined and pending jobs cancelled", 0, 0, NULL);
    log_info("fileward stopped.");
    audit_close();
    if (audit_failed()) {
        result = EXIT_RUNTIME;
    }
    release_pidfile();
    return result;
}

static int stop_command(int argc, char *argv[]) {
    if (argc == 1 && is_help_arg(argv[0])) {
        printf("Usage: fileward stop [--pidfile <file>]\n");
        return EXIT_OK;
    }
    const char *pidfile_path = "/tmp/fileward.pid";
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--pidfile") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "missing value for --pidfile\n");
                return EXIT_USAGE;
            }
            pidfile_path = argv[++i];
        } else {
            fprintf(stderr, "unexpected argument: %s\n", argv[i]);
            return EXIT_USAGE;
        }
    }

    pid_t pid = 0;
    if (read_running_pid(pidfile_path, &pid) != 0) {
        fprintf(stderr, "fileward is not running\n");
        return EXIT_RUNTIME;
    }

    if (kill(pid, SIGTERM) != 0) {
        fprintf(stderr, "failed to signal process %ld: %s\n", (long)pid, strerror(errno));
        return EXIT_RUNTIME;
    }

    for (int attempt = 0; attempt < 50; attempt++) {
        if (read_running_pid(pidfile_path, &pid) != 0) {
            printf("stopped fileward\n");
            return EXIT_OK;
        }
        struct timespec delay = {0, 100000000L};
        nanosleep(&delay, NULL);
    }

    fprintf(stderr, "timed out waiting for fileward to stop\n");
    return EXIT_RUNTIME;
}

static int status_command(int argc, char *argv[]) {
    if (argc == 1 && is_help_arg(argv[0])) {
        printf("Usage: fileward status [--pidfile <file>]\n");
        return EXIT_OK;
    }
    const char *pidfile_path = "/tmp/fileward.pid";
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--pidfile") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "missing value for --pidfile\n");
                return EXIT_USAGE;
            }
            pidfile_path = argv[++i];
        } else {
            fprintf(stderr, "unexpected argument: %s\n", argv[i]);
            return EXIT_USAGE;
        }
    }

    pid_t pid = 0;
    if (read_running_pid(pidfile_path, &pid) != 0) {
        printf("fileward: stopped\n");
        return EXIT_OK;
    }

    printf("fileward: running (pid %ld)\n", (long)pid);
    return EXIT_OK;
}

static int live_command(int argc, char **argv, int reload) {
    char path[PATH_MAX], response[4096];
    if (control_default_path(path, sizeof(path))) {
        return EXIT_USAGE;
    }
    if (argc == 1 && is_help_arg(argv[0])) {
        printf("Usage: fileward %s [--socket <path>]\n", reload ? "reload" : "health");
        return 0;
    }
    if (argc) {
        if (argc != 2 || strcmp(argv[0], "--socket") || strlen(argv[1]) >= sizeof(path)) {
            return EXIT_USAGE;
        }
        snprintf(path, sizeof(path), "%s", argv[1]);
    }
    if (control_request(path, reload ? "reload" : "health", response, sizeof(response))) {
        fprintf(stderr, "FW_CONTROL_CONNECT: %s: %s\n", path, strerror(errno));
        return EXIT_RUNTIME;
    }
    fputs(response, stdout);
    if (reload) {
        return strstr(response, "\"status\":\"reloaded\"") ? 0 : EXIT_CONFIG;
    }
    return strstr(response, "\"status\":\"healthy\"")    ? 0
           : strstr(response, "\"status\":\"degraded\"") ? 4
                                                         : EXIT_RUNTIME;
}

static int reload_command(int argc, char *argv[]) {
    if (argc == 1 && is_help_arg(argv[0])) {
        printf("Usage: fileward reload [--socket <path> | --pidfile <file>]\n");
        return EXIT_OK;
    }
    if (!argc || strcmp(argv[0], "--socket") == 0) {
        return live_command(argc, argv, 1);
    }
    const char *pidfile_path = "/tmp/fileward.pid";
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--pidfile") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "missing value for --pidfile\n");
                return EXIT_USAGE;
            }
            pidfile_path = argv[++i];
        } else {
            fprintf(stderr, "unexpected argument: %s\n", argv[i]);
            return EXIT_USAGE;
        }
    }

    pid_t pid = 0;
    if (read_running_pid(pidfile_path, &pid) != 0) {
        fprintf(stderr, "fileward is not running\n");
        return EXIT_RUNTIME;
    }

    if (kill(pid, SIGHUP) != 0) {
        fprintf(stderr, "failed to reload process %ld: %s\n", (long)pid, strerror(errno));
        return EXIT_RUNTIME;
    }

    printf("reload requested for pid %ld\n", (long)pid);
    return EXIT_OK;
}

static int test_command(int argc, char *argv[]) {
    const char *config_path = NULL;
    const char *target_path = NULL;
    config_t config;
    const char *watch_root = NULL;

    config.watch_path[0] = '\0';
    config.rule_count = 0;

    for (int i = 0; i < argc; i++) {
        if (is_help_arg(argv[i])) {
            print_test_usage("fileward");
            return EXIT_OK;
        }

        if (strcmp(argv[i], "--config") == 0) {
            if (i + 1 >= argc) {
                print_test_usage("fileward");
                return EXIT_USAGE;
            }
            config_path = argv[++i];
        } else if (target_path == NULL) {
            target_path = argv[i];
        } else {
            print_test_usage("fileward");
            return EXIT_USAGE;
        }
    }

    if (target_path == NULL) {
        print_test_usage("fileward");
        return EXIT_USAGE;
    }

    char expanded_target[PATH_MAX];
    const char *resolved_target = target_path;
    if (expand_path(target_path, expanded_target, sizeof(expanded_target)) == 0) {
        resolved_target = expanded_target;
    }

    struct stat st;
    if (lstat(resolved_target, &st) != 0) {
        perror("path validation failed");
        return EXIT_RUNTIME;
    }

    printf("path exists: %s\n", resolved_target);
    if (config_path == NULL) {
        printf("no config loaded, test completed\n");
        return EXIT_OK;
    }

    if (load_config(config_path, &config) != 0) {
        return EXIT_CONFIG;
    }
    if (canonical_config_root(&config)) {
        return EXIT_RUNTIME;
    }
    watch_root = config.watch_path;

    char relative[PATH_MAX];
    int within = path_within_root(watch_root, resolved_target, relative, sizeof(relative));
    if (within != 1) {
        printf("path is outside watch root: %s\n", watch_root);
        return EXIT_USAGE;
    }

    printf("watch root: %s\n", watch_root);
    printf("relative path: %s\n", relative);
    printf("matching rules for created event:\n");
    int matched = describe_matching_rules(relative, EVENT_CREATED, &config, 0);
    if (matched == 0) {
        printf("  no rules match this path for created event\n");
    }

    return 0;
}

static int explain_command(int argc, char *argv[]) {
    const char *config_path = NULL;
    const char *target_path = NULL;
    const char *event_token = "created";
    config_t config;
    const char *watch_root = NULL;

    config.watch_path[0] = '\0';
    config.rule_count = 0;

    for (int i = 0; i < argc; i++) {
        if (is_help_arg(argv[i])) {
            print_explain_usage("fileward");
            return EXIT_OK;
        }

        if (strcmp(argv[i], "--config") == 0) {
            if (i + 1 >= argc) {
                print_explain_usage("fileward");
                return EXIT_USAGE;
            }
            config_path = argv[++i];
        } else if (strcmp(argv[i], "--event") == 0) {
            if (i + 1 >= argc) {
                print_explain_usage("fileward");
                return EXIT_USAGE;
            }
            event_token = argv[++i];
        } else if (target_path == NULL) {
            target_path = argv[i];
        } else {
            print_explain_usage("fileward");
            return EXIT_USAGE;
        }
    }

    if (target_path == NULL || config_path == NULL) {
        fprintf(stderr, "explain requires --config <file> and <path>\n");
        print_explain_usage("fileward");
        return EXIT_USAGE;
    }

    if (load_config(config_path, &config) != 0) {
        return EXIT_CONFIG;
    }
    if (canonical_config_root(&config)) {
        return EXIT_RUNTIME;
    }
    watch_root = config.watch_path;

    event_type_t event = parse_event_type(event_token);
    if (event == EVENT_UNKNOWN) {
        fprintf(stderr, "unknown event type: %s\n", event_token);
        return EXIT_USAGE;
    }

    char expanded_target[PATH_MAX];
    const char *resolved_target = target_path;
    if (expand_path(target_path, expanded_target, sizeof(expanded_target)) == 0) {
        resolved_target = expanded_target;
    }

    char relative[PATH_MAX];
    int within = path_within_root(watch_root, resolved_target, relative, sizeof(relative));
    if (within != 1) {
        fprintf(stderr, "path is outside watch root: %s\n", watch_root);
        return EXIT_USAGE;
    }

    printf("explain %s for %s\n", event_type_to_string(event), resolved_target);
    printf("watch root: %s\n", watch_root);
    printf("relative path: %s\n", relative);

    int matched = describe_matching_rules(relative, event, &config, 1);
    if (matched == 0) {
        printf("no matching rules\n");
    }

    return 0;
}

static int validate_command(int argc, char *argv[]) {
    const char *config_path = NULL;
    int check_watch = 0;
    if (argc == 1 && is_help_arg(argv[0])) {
        fprintf(stderr, "Usage: fileward validate --config <file> [--check-watch]\n");
        return EXIT_OK;
    }
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--config") && !config_path && i + 1 < argc) {
            config_path = argv[++i];
        } else if (!strcmp(argv[i], "--check-watch") && !check_watch) {
            check_watch = 1;
        } else {
            fprintf(stderr, "Usage: fileward validate --config <file> [--check-watch]\n");
            return EXIT_USAGE;
        }
    }
    if (!config_path) {
        fprintf(stderr, "Usage: fileward validate --config <file> [--check-watch]\n");
        return EXIT_USAGE;
    }
    config_t config;
    if (load_config(config_path, &config) != 0) {
        return EXIT_CONFIG;
    }
    if (check_watch && check_watch_root(&config)) {
        startup_error("watcher_error", "FW_WATCHER_START", errno, config.watch_path,
                      "watch root is not an accessible directory");
        return EXIT_RUNTIME;
    }
    int enabled = 0;
    for (int i = 0; i < config.rule_count; i++) {
        enabled += config.rules[i].enabled;
    }
    printf(
        "config valid: %s\nwatch root: %s\nrules: %d (%d enabled, %d disabled)\nmode: %s\npath filters: %d\n",
        config_path, config.watch_path, config.rule_count, enabled, config.rule_count - enabled,
        config.first_match ? "first" : "all", config.filter_count);
    printf("watch check: %s\n", check_watch ? "accessible directory" : "not requested (syntax only)");
    return EXIT_OK;
}

static int journal_command(int argc, char **argv, int stats) {
    char default_path[PATH_MAX];
    const char *base = getenv("XDG_STATE_HOME");
    int written = base && *base
                      ? snprintf(default_path, sizeof(default_path), "%s/fileward/events.jsonl", base)
                      : snprintf(default_path, sizeof(default_path), "%s/.local/state/fileward/events.jsonl",
                                 getenv("HOME") ? getenv("HOME") : ".");
    if (written < 0 || written >= (int)sizeof(default_path)) {
        return EXIT_USAGE;
    }
    const char *path = default_path, *id = NULL;
    unsigned limit = 50;
    int all = 0;
    for (int i = 0; i < argc; i++) {
        if (is_help_arg(argv[i])) {
            printf("Usage: fileward %s [--journal <file>] [--limit 1..1000] [--all] [--event-id <id>]\n",
                   stats ? "stats" : "history");
            return 0;
        }
        if (strcmp(argv[i], "--journal") == 0 && i + 1 < argc) {
            path = argv[++i];
        } else if (!stats && strcmp(argv[i], "--all") == 0) {
            all = 1;
        } else if (!stats && strcmp(argv[i], "--event-id") == 0 && i + 1 < argc) {
            id = argv[++i];
        } else if (!stats && strcmp(argv[i], "--limit") == 0 && i + 1 < argc) {
            char *end;
            unsigned long n = strtoul(argv[++i], &end, 10);
            if (*end || !n || n > 1000) {
                return EXIT_USAGE;
            }
            limit = n;
        } else {
            return EXIT_USAGE;
        }
    }
    return audit_query(path, stats, all, limit, id);
}

int main(int argc, char *argv[]) {
    if (argc == 1 || is_help_arg(argv[1])) {
        print_help(argv[0]);
        return EXIT_OK;
    }

    const char *command = argv[1];
    if (strcmp(command, "--version") == 0) {
        puts(FILEWARD_VERSION);
        return 0;
    }
    if (strcmp(command, "health") == 0) {
        return live_command(argc - 2, argv + 2, 0);
    }
    if (strcmp(command, "actions") == 0) {
        for (size_t i = 0; i < action_handler_count(); i++) {
            const action_handler_t *handler = action_handler_at(i);
            printf("%s\tcapabilities=%u\n", handler->name, handler->capabilities);
        }
        return 0;
    }
    if (strcmp(command, "history") == 0 || strcmp(command, "stats") == 0) {
        return journal_command(argc - 2, argv + 2, strcmp(command, "stats") == 0);
    }
    if (strcmp(command, "run") == 0) {
        return run_command(argc - 2, argv + 2, 0);
    }
    if (strcmp(command, "start") == 0) {
        return run_command(argc - 2, argv + 2, 1);
    }
    if (strcmp(command, "stop") == 0) {
        return stop_command(argc - 2, argv + 2);
    }
    if (strcmp(command, "status") == 0) {
        return status_command(argc - 2, argv + 2);
    }
    if (strcmp(command, "reload") == 0) {
        return reload_command(argc - 2, argv + 2);
    }
    if (strcmp(command, "validate") == 0) {
        return validate_command(argc - 2, argv + 2);
    }
    if (strcmp(command, "test") == 0) {
        return test_command(argc - 2, argv + 2);
    }
    if (strcmp(command, "explain") == 0) {
        return explain_command(argc - 2, argv + 2);
    }
    if (strcmp(command, "help") == 0) {
        print_help(argv[0]);
        return EXIT_OK;
    }

    /* Legacy default behavior: treat first argument as watch path or options for run. */
    return run_command(argc - 1, argv + 1, 0);
}
