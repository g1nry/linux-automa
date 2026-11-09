#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "service.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

static struct sockaddr_un address;
static socklen_t address_size;
static unsigned long long interval, last;
static int active;
static unsigned long long microseconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (unsigned long long)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}
int service_initialize(int enabled) {
    active = enabled;
    interval = last = 0;
    if (!enabled) {
        return 0;
    }
    const char *path = getenv("NOTIFY_SOCKET");
    if (!path || (path[0] != '/' && path[0] != '@') || strlen(path) >= sizeof(address.sun_path)) {
        errno = EINVAL;
        return -1;
    }
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, path);
    address_size = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strlen(path) + 1);
    if (*path == '@') {
        address.sun_path[0] = 0;
        address_size--;
    }
    const char *value = getenv("WATCHDOG_USEC"), *pid = getenv("WATCHDOG_PID");
    if (pid && *pid) {
        char *end;
        unsigned long number = strtoul(pid, &end, 10);
        if (*end || number != (unsigned long)getpid()) {
            errno = EINVAL;
            return -1;
        }
    }
    if (value && *value) {
        char *end;
        errno = 0;
        unsigned long long number = strtoull(value, &end, 10);
        if (errno || *end || number < 100000 || number > 3600000000ULL) {
            errno = EINVAL;
            return -1;
        }
        interval = number / 2;
    }
    return 0;
}
int service_notify(const char *status) {
    if (!active) {
        return 0;
    }
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        return -1;
    }
    ssize_t count =
        sendto(fd, status, strlen(status), MSG_NOSIGNAL, (struct sockaddr *)&address, address_size);
    int saved = errno;
    close(fd);
    errno = saved;
    return count == (ssize_t)strlen(status) ? 0 : -1;
}
int service_watchdog(void) {
    if (!active || !interval) {
        return 0;
    }
    unsigned long long now = microseconds();
    if (now - last < interval) {
        return 0;
    }
    if (service_notify("WATCHDOG=1")) {
        return -1;
    }
    last = now;
    return 0;
}
