#include "config.h"
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

static void print_usage(const char *program_name) {
    fprintf(stderr, "Usage: %s [--config <file>] <directory_to_watch>\n", program_name);
}

int main(int argc, char *argv[]) {
    const char *config_path = NULL;
    const char *watch_path = NULL;
    config_t config;

    if (argc == 3 && strcmp(argv[1], "--config") == 0) {
        config_path = argv[2];
    } else if (argc == 2) {
        watch_path = argv[1];
    } else {
        print_usage(argv[0]);
        return 1;
    }

    log_init("fileward");

    if (config_path != NULL) {
        if (load_config(config_path, &config) != 0) {
            return 1;
        }
        watch_path = config.watch_path;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    if (start_watcher(watch_path) != 0) {
        return 1;
    }

    log_info("fileward started. Press Ctrl+C to stop.");

    while (running) {
        if (watcher_process_events(500) != 0) {
            stop_watcher();
            return 1;
        }
    }

    stop_watcher();
    log_info("fileward stopped.");

    return 0;
}
