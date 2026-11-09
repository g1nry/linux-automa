#include "handlers.h"
#include <string.h>

static const action_handler_t handlers[] = {
    {ACTION_LOG, "log", ACTION_MESSAGE, action_handle_log},
    {ACTION_MOVE, "move", ACTION_TARGET | ACTION_FILE_OPTIONS, action_handle_file},
    {ACTION_COPY, "copy", ACTION_TARGET | ACTION_FILE_OPTIONS, action_handle_file},
    {ACTION_ARCHIVE, "archive", ACTION_TARGET | ACTION_FILE_OPTIONS, action_handle_file},
    {ACTION_EXEC, "exec", ACTION_COMMAND | ACTION_TIMEOUT, action_handle_exec},
};
size_t action_handler_count(void) {
    return sizeof(handlers) / sizeof(handlers[0]);
}
const action_handler_t *action_handler_at(size_t index) {
    return index < action_handler_count() ? handlers + index : NULL;
}
const action_handler_t *action_handler_find(action_type_t type) {
    for (size_t i = 0; i < action_handler_count(); i++) {
        if (handlers[i].type == type) {
            return handlers + i;
        }
    }
    return NULL;
}
const action_handler_t *action_handler_named(const char *name) {
    for (size_t i = 0; i < action_handler_count(); i++) {
        if (strcmp(handlers[i].name, name) == 0) {
            return handlers + i;
        }
    }
    return NULL;
}
const char *action_type_name(action_type_t type) {
    const action_handler_t *handler = action_handler_find(type);
    return handler ? handler->name : "unknown";
}
