#ifndef FILEWARD_ACTION_H
#define FILEWARD_ACTION_H

#include "config.h"

int execute_action(const action_t *action, const char *watch_path, const char *filename, event_type_t event);

#endif
