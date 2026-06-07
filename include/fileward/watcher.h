#ifndef FILEWARD_WATCHER_H
#define FILEWARD_WATCHER_H

#include "config.h"

typedef struct {
    event_type_t type;
    char filename[PATH_MAX];
} file_event_t;

int start_watcher(const char *path);
int watcher_process_events(int timeout_ms, int (*callback)(const file_event_t *, void *), void *user_data);
void stop_watcher(void);

#endif
