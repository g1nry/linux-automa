#ifndef FILEWARD_DAEMON_H
#define FILEWARD_DAEMON_H
#include <sys/types.h>
int lock_pidfile(const char *path);
int write_pidfile(pid_t pid);
void release_pidfile(void);
int read_running_pid(const char *path, pid_t *pid);
int redirect_output(const char *path);
int daemonize_process(const char *path);
void notify_startup(int success);
#endif
