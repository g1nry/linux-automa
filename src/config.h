#ifndef FILEWARD_CONFIG_H
#define FILEWARD_CONFIG_H

#include <linux/limits.h>

typedef struct {
    char watch_path[PATH_MAX];
} config_t;

int load_config(const char *path, config_t *config);

#endif
