#ifndef FILEWARD_SERVICE_H
#define FILEWARD_SERVICE_H
int service_initialize(int enabled);
int service_notify(const char *status);
int service_watchdog(void);
#endif
