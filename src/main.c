#include "log.h"
#include "watcher.h"

#include <signal.h>
#include <stdio.h>

static volatile sig_atomic_t running = 1;

static void handle_signal(int signal_number) {
    (void)signal_number;
    running = 0;
}

static void print_usage(const char *program_name) {
    fprintf(stderr, "Usage: %s <directory_to_watch>\n", program_name);
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        print_usage(argv[0]);
        return 1;
    }

    log_init("fileward");

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    if (start_watcher(argv[1]) != 0) {
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
