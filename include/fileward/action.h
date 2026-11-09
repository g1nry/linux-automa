#ifndef FILEWARD_ACTION_H
#define FILEWARD_ACTION_H

#include "config.h"
#include <signal.h>
#include <stddef.h>

#define FILEWARD_MAX_ARGS 32
#define ACTION_SKIPPED 1

const char *action_type_name(action_type_t type);
const char *conflict_policy_name(conflict_policy_t policy);
/* Split quotes/escapes without shell interpretation. argv points into storage. */
int action_split_command(const char *text, char *storage, size_t size, char *argv[FILEWARD_MAX_ARGS + 1]);
/* A single pass; substituted filenames never become template or shell syntax. */
int action_expand_template(const char *text, const char *root, const char *filename, event_type_t event,
                           char *out, size_t size);
int action_expand_target(const char *text, const char *root, const char *filename, event_type_t event,
                         char out[PATH_MAX]);
int action_validate_template(const char *text);
void action_set_running_flag(const volatile sig_atomic_t *flag);
/* 0 executed, ACTION_SKIPPED on a configured conflict skip, -1 on error. */
int execute_action(const action_t *action, const char *watch_path, const char *filename, event_type_t event,
                   int dry_run);

#endif
