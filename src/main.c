#include "watcher.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>

static volatile int running = 1;

void handle_signal(int sig) {
    running = 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <directory_to_watch>\n", argv[0]);
        return 1;
    }

    log_init("fileward");

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    if (start_watcher(argv[1]) != 0) {
        return 1;
    }

    log_info("Fileward started. Press Ctrl+C to stop.");

    while (running) {
        // Пока просто спим, в следующей версии будем читать события
        sleep(1);
    }

    stop_watcher();
    log_info("Fileward stopped.");
    return 0;
}