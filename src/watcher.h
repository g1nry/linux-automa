#ifndef FILEWARD_WATCHER_H
#define FILEWARD_WATCHER_H

int start_watcher(const char *path);
int watcher_process_events(int timeout_ms);
void stop_watcher(void);

#endif
