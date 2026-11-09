#ifndef FILEWARD_HANDLERS_H
#define FILEWARD_HANDLERS_H
#include <fileward/action.h>
int action_handle_log(const action_t *, const char *, const char *, event_type_t, int);
int action_handle_exec(const action_t *, const char *, const char *, event_type_t, int);
int action_handle_file(const action_t *, const char *, const char *, event_type_t, int);
#endif
