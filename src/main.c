#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include <fileward/action.h>
#include <fileward/config.h>
#include <fileward/glob.h>
#include <fileward/log.h>
#include <fileward/watcher.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

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
    return arg != NULL &&
           (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0 || strcmp(arg, "help") == 0);
}

static void print_usage(const char *program_name) {
    fprintf(stderr, "Usage: %s <subcommand> [options]\n", program_name);
    fprintf(stderr, "       %s run [--config <file>] [--dry-run] [--daemon] [--pidfile <file>] [--log-file <file>] [--state-file <file>] <directory_to_watch>\n", program_name);
    fprintf(stderr, "       %s start [--config <file>] [--dry-run] [--pidfile <file>] [--log-file <file>] [--state-file <file>] <directory_to_watch>\n", program_name);
    fprintf(stderr, "       %s stop [--pidfile <file>]\n", program_name);
    fprintf(stderr, "       %s status [--pidfile <file>]\n", program_name);
    fprintf(stderr, "       %s reload [--pidfile <file>]\n", program_name);
    fprintf(stderr, "       %s test [--config <file>] <path>\n", program_name);
    fprintf(stderr, "       %s explain [--config <file>] [--event <event>] <path>\n", program_name);
}

static void print_run_usage(const char *program_name) {
    fprintf(stderr, "Usage: %s run [--config <file>] [--dry-run] [--daemon] [--pidfile <file>] [--log-file <file>] [--state-file <file>] <directory_to_watch>\n", program_name);
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
    fprintf(stderr, "  explain  explain which rules would match a path for an event\n");
    fprintf(stderr, "\nOptions:\n");
    fprintf(stderr, "  --config <file>    load rules from a config file\n");
    fprintf(stderr, "  --dry-run          do not execute actions when running\n");
    fprintf(stderr, "  --daemon           detach and run in the background\n");
    fprintf(stderr, "  --pidfile <file>   write/read the daemon pidfile\n");
    fprintf(stderr, "  --log-file <file>  redirect logs to a file when daemonized\n");
    fprintf(stderr, "  --state-file <file> append handled events to a journal file\n");
    fprintf(stderr, "  --event <event>    event type for explain: created, modified, deleted, moved_from, moved_to\n");
    fprintf(stderr, "\nExamples:\n");
    fprintf(stderr, "  %s run --config fileward.conf\n", program_name);
    fprintf(stderr, "  %s start --daemon --config fileward.conf ~/Downloads\n", program_name);
    fprintf(stderr, "  %s test --config fileward.conf ~/Downloads/report.pdf\n", program_name);
    fprintf(stderr, "  %s explain --config fileward.conf --event created docs/report.pdf\n", program_name);
}

typedef struct {
    const config_t *config;
    int dry_run;
    const char *state_file_path;
    file_event_t pending_events[256];
    size_t pending_count;
} runtime_context_t;

static int write_pidfile(const char *pidfile_path, pid_t pid) {
    if (pidfile_path == NULL || pidfile_path[0] == '\0') {
        return 0;
    }

    FILE *fp = fopen(pidfile_path, "w");
    if (fp == NULL) {
        fprintf(stderr, "failed to open pidfile '%s': %s\n", pidfile_path, strerror(errno));
        return -1;
    }

    fprintf(fp, "%ld\n", (long)pid);
    fclose(fp);
    return 0;
}

static int remove_pidfile(const char *pidfile_path) {
    if (pidfile_path == NULL || pidfile_path[0] == '\0') {
        return 0;
    }

    if (unlink(pidfile_path) != 0 && errno != ENOENT) {
        fprintf(stderr, "failed to remove pidfile '%s': %s\n", pidfile_path, strerror(errno));
        return -1;
    }

    return 0;
}

static int read_pidfile(const char *pidfile_path, pid_t *pid_out) {
    if (pidfile_path == NULL || pidfile_path[0] == '\0') {
        return -1;
    }

    FILE *fp = fopen(pidfile_path, "r");
    if (fp == NULL) {
        return -1;
    }

    long pid_value = 0;
    if (fscanf(fp, "%ld", &pid_value) != 1) {
        fclose(fp);
        return -1;
    }

    fclose(fp);
    *pid_out = (pid_t)pid_value;
    return 0;
}

static int is_pid_running(pid_t pid) {
    if (pid <= 0) {
        return 0;
    }

    return kill(pid, 0) == 0 || errno == EPERM;
}

static int redirect_output(const char *log_file_path) {
    if (log_file_path == NULL || log_file_path[0] == '\0') {
        int devnull = open("/dev/null", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (devnull < 0) {
            return -1;
        }
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        close(devnull);
        return 0;
    }

    int log_fd = open(log_file_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (log_fd < 0) {
        return -1;
    }

    dup2(log_fd, STDOUT_FILENO);
    dup2(log_fd, STDERR_FILENO);
    close(log_fd);
    return 0;
}

static int daemonize_process(const char *pidfile_path, const char *log_file_path) {
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "fork failed: %s\n", strerror(errno));
        return -1;
    }

    if (pid > 0) {
        exit(EXIT_OK);
    }

    if (setsid() < 0) {
        fprintf(stderr, "setsid failed: %s\n", strerror(errno));
        return -1;
    }

    signal(SIGHUP, SIG_IGN);

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "second fork failed: %s\n", strerror(errno));
        return -1;
    }

    if (pid > 0) {
        exit(EXIT_OK);
    }

    if (chdir("/") != 0) {
        fprintf(stderr, "chdir failed: %s\n", strerror(errno));
        return -1;
    }

    close(STDIN_FILENO);
    if (redirect_output(log_file_path) != 0) {
        fprintf(stderr, "failed to redirect output: %s\n", strerror(errno));
        return -1;
    }

    umask(0);
    if (write_pidfile(pidfile_path, getpid()) != 0) {
        return -1;
    }

    return 0;
}

static int append_state_event(const char *state_file_path, const char *watch_root, const char *filename, event_type_t event, const char *action_name) {
    if (state_file_path == NULL || state_file_path[0] == '\0') {
        return 0;
    }

    FILE *fp = fopen(state_file_path, "a");
    if (fp == NULL) {
        fprintf(stderr, "failed to open state file '%s': %s\n", state_file_path, strerror(errno));
        return -1;
    }

    time_t now = time(NULL);
    char timestamp[32];
    struct tm *time_info = localtime(&now);
    if (time_info != NULL) {
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", time_info);
    } else {
        strcpy(timestamp, "unknown");
    }

    fprintf(fp, "%s\t%s\t%s\t%s\t%s\n", timestamp, event_type_to_string(event), watch_root != NULL ? watch_root : "-", filename, action_name != NULL ? action_name : "unknown");
    fclose(fp);
    return 0;
}

static int print_event_callback(const file_event_t *event, void *user_data) {
    (void)user_data;
    if (event == NULL) {
        return -1;
    }

    printf("event: %s", event_type_to_string(event->type));
    if (event->filename[0] != '\0') {
        printf(" path: %s", event->filename);
    }
    printf("\n");
    return 0;
}

static int dispatch_event(runtime_context_t *context, const file_event_t *event) {
    if (context == NULL || context->config == NULL || event == NULL) {
        return -1;
    }

    if (event->filename[0] == '\0') {
        log_warn("received event without filename");
        return 0;
    }

    int matched = 0;
    for (int i = 0; i < context->config->rule_count; i++) {
        const rule_t *rule = &context->config->rules[i];
        if (rule->event != event->type) {
            continue;
        }
        if (!glob_match(rule->path_glob, event->filename)) {
            continue;
        }

        matched++;
        execute_action(&rule->action, context->config->watch_path, event->filename, event->type, context->dry_run);
        if (context->state_file_path != NULL && context->state_file_path[0] != '\0') {
            const char *action_name = rule->action.type == ACTION_LOG ? "log" : "move";
            append_state_event(context->state_file_path, context->config->watch_path, event->filename, event->type, action_name);
        }
    }

    if (matched == 0) {
        log_debug("no matching rules for %s", event->filename);
    }

    return 0;
}

static int queue_event(runtime_context_t *context, const file_event_t *event) {
    if (context == NULL || event == NULL) {
        return -1;
    }

    if (context->pending_count >= sizeof(context->pending_events) / sizeof(context->pending_events[0])) {
        log_warn("event queue is full; dropping event for %s", event->filename);
        return -1;
    }

    context->pending_events[context->pending_count++] = *event;
    return 0;
}

static int flush_pending_events(runtime_context_t *context) {
    if (context == NULL) {
        return -1;
    }

    while (context->pending_count > 0) {
        file_event_t event = context->pending_events[0];
        for (size_t i = 1; i < context->pending_count; i++) {
            context->pending_events[i - 1] = context->pending_events[i];
        }
        context->pending_count--;
        dispatch_event(context, &event);
    }

    return 0;
}

static int handle_file_event(const file_event_t *event, void *user_data) {
    if (event == NULL || user_data == NULL) {
        return -1;
    }

    runtime_context_t *context = user_data;
    if (context->config == NULL) {
        return -1;
    }

    return queue_event(context, event);
}

static int load_watch_config(const char *config_path, const char *watch_path, config_t *config, const char **out_watch_path) {
    if (config_path != NULL) {
        if (load_config(config_path, config) != 0) {
            return -1;
        }
        *out_watch_path = config->watch_path;
        return 0;
    }

    if (watch_path == NULL) {
        return -1;
    }

    config->watch_path[0] = '\0';
    config->rule_count = 0;
    strncpy(config->watch_path, watch_path, sizeof(config->watch_path));
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
    if (realpath(target, resolved_target) == NULL) {
        return -1;
    }

    size_t root_len = strlen(resolved_root);
    if (strncmp(resolved_root, resolved_target, root_len) != 0) {
        return 0;
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

static int describe_matching_rules(const char *relative_path, event_type_t event, const config_t *config) {
    int matched = 0;
    for (int i = 0; i < config->rule_count; i++) {
        const rule_t *rule = &config->rules[i];
        if (rule->event != event) {
            continue;
        }
        if (!glob_match(rule->path_glob, relative_path)) {
            continue;
        }

        matched++;
        printf("Match %d:\n", matched);
        printf("  event: %s\n", event_type_to_string(rule->event));
        printf("  pattern: %s\n", rule->path_glob);
        printf("  action: %s\n", rule->action.type == ACTION_LOG ? "log" : "move");
        if (rule->action.type == ACTION_MOVE) {
            printf("  target: %s\n", rule->action.target);
        } else if (rule->action.type == ACTION_LOG && rule->action.message[0] != '\0') {
            printf("  message: %s\n", rule->action.message);
        }
    }

    return matched;
}

static int run_command(int argc, char *argv[], int daemon_mode) {
    const char *config_path = NULL;
    const char *watch_path = NULL;
    const char *pidfile_path = NULL;
    const char *log_file_path = NULL;
    const char *state_file_path = NULL;
    int dry_run = 0;
    config_t config;
    runtime_context_t context;
    const char *watch_root = NULL;

    config.watch_path[0] = '\0';
    config.rule_count = 0;

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
        } else if (watch_path == NULL) {
            watch_path = argv[i];
        } else {
            print_run_usage("fileward");
            return EXIT_USAGE;
        }
    }

    if (config_path != NULL && watch_path != NULL) {
        fprintf(stderr, "Cannot use --config and explicit watch path together.\n");
        print_run_usage("fileward");
        return EXIT_USAGE;
    }

    if (load_watch_config(config_path, watch_path, &config, &watch_root) != 0) {
        fprintf(stderr, "Failed to load watch configuration.\n");
        return EXIT_CONFIG;
    }

    if (daemon_mode) {
        if (pidfile_path == NULL) {
            pidfile_path = "/tmp/fileward.pid";
        }

        pid_t existing_pid = 0;
        if (read_pidfile(pidfile_path, &existing_pid) == 0 && is_pid_running(existing_pid)) {
            fprintf(stderr, "fileward is already running (pid %ld)\n", (long)existing_pid);
            return EXIT_RUNTIME;
        }

        if (daemonize_process(pidfile_path, log_file_path) != 0) {
            return EXIT_RUNTIME;
        }
    } else if (pidfile_path != NULL) {
        if (write_pidfile(pidfile_path, getpid()) != 0) {
            return EXIT_RUNTIME;
        }
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGHUP, handle_signal);

    if (start_watcher(watch_root) != 0) {
        if (daemon_mode) {
            remove_pidfile(pidfile_path);
        }
        return EXIT_RUNTIME;
    }

    log_info("fileward started. Press Ctrl+C to stop.%s", dry_run ? " (dry-run mode)" : "");

    context.config = &config;
    context.dry_run = dry_run;
    context.state_file_path = state_file_path;

    while (running) {
        if (reload_requested && config_path != NULL) {
            reload_requested = 0;
            config_t new_config;
            if (load_config(config_path, &new_config) != 0) {
                log_error("failed to reload config: %s", config_path);
                stop_watcher();
                if (daemon_mode) {
                    remove_pidfile(pidfile_path);
                }
                return EXIT_CONFIG;
            }
            if (strcmp(new_config.watch_path, config.watch_path) != 0) {
                log_error("config reload changed watch path, restart required");
                stop_watcher();
                if (daemon_mode) {
                    remove_pidfile(pidfile_path);
                }
                return EXIT_RUNTIME;
            }
            config = new_config;
            log_info("reloaded config: %s", config_path);
        }

        int (*event_callback)(const file_event_t *, void *) = config.rule_count > 0 ? handle_file_event : print_event_callback;
        if (watcher_process_events(500, event_callback, &context) != 0) {
            stop_watcher();
            if (daemon_mode) {
                remove_pidfile(pidfile_path);
            }
            return EXIT_RUNTIME;
        }

        if (config.rule_count > 0) {
            flush_pending_events(&context);
        }
    }

    stop_watcher();
    if (daemon_mode) {
        remove_pidfile(pidfile_path);
    }
    log_info("fileward stopped.");
    return EXIT_OK;
}

static int stop_command(int argc, char *argv[]) {
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
    if (read_pidfile(pidfile_path, &pid) != 0 || !is_pid_running(pid)) {
        fprintf(stderr, "fileward is not running\n");
        return EXIT_RUNTIME;
    }

    if (kill(pid, SIGTERM) != 0) {
        fprintf(stderr, "failed to signal process %ld: %s\n", (long)pid, strerror(errno));
        return EXIT_RUNTIME;
    }

    remove_pidfile(pidfile_path);
    printf("stopped fileward (pid %ld)\n", (long)pid);
    return EXIT_OK;
}

static int status_command(int argc, char *argv[]) {
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
    if (read_pidfile(pidfile_path, &pid) != 0 || !is_pid_running(pid)) {
        printf("fileward: stopped\n");
        return EXIT_OK;
    }

    printf("fileward: running (pid %ld)\n", (long)pid);
    return EXIT_OK;
}

static int reload_command(int argc, char *argv[]) {
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
    if (read_pidfile(pidfile_path, &pid) != 0 || !is_pid_running(pid)) {
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

    struct stat st;
    if (stat(target_path, &st) != 0) {
        perror("path validation failed");
        return EXIT_RUNTIME;
    }

    printf("path exists: %s\n", target_path);
    if (config_path == NULL) {
        printf("no config loaded, test completed\n");
        return EXIT_OK;
    }

    if (load_config(config_path, &config) != 0) {
        return EXIT_CONFIG;
    }
    watch_root = config.watch_path;

    char relative[PATH_MAX];
    int within = path_within_root(watch_root, target_path, relative, sizeof(relative));
    if (within != 1) {
        printf("path is outside watch root: %s\n", watch_root);
        return EXIT_USAGE;
    }

    printf("watch root: %s\n", watch_root);
    printf("relative path: %s\n", relative);
    printf("matching rules for created event:\n");
    int matched = describe_matching_rules(relative, EVENT_CREATED, &config);
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
    watch_root = config.watch_path;

    event_type_t event = parse_event_type(event_token);
    if (event == EVENT_UNKNOWN) {
        fprintf(stderr, "unknown event type: %s\n", event_token);
        return EXIT_USAGE;
    }

    char relative[PATH_MAX];
    int within = path_within_root(watch_root, target_path, relative, sizeof(relative));
    if (within != 1) {
        fprintf(stderr, "path is outside watch root: %s\n", watch_root);
        return EXIT_USAGE;
    }

    printf("explain %s for %s\n", event_type_to_string(event), target_path);
    printf("watch root: %s\n", watch_root);
    printf("relative path: %s\n", relative);

    int matched = describe_matching_rules(relative, event, &config);
    if (matched == 0) {
        printf("no matching rules\n");
    }

    return 0;
}

int main(int argc, char *argv[]) {
    if (argc == 1 || is_help_arg(argv[1])) {
        print_help(argv[0]);
        return EXIT_OK;
    }

    const char *command = argv[1];
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
