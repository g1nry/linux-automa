#ifndef FILEWARD_CONTROL_H
#define FILEWARD_CONTROL_H
#include <stddef.h>
typedef struct control_server control_server_t;
typedef int (*control_handler_t)(void *, const char *, char *, size_t);
int control_default_path(char *out, size_t size);
control_server_t *control_open(const char *path);
void control_poll(control_server_t *, control_handler_t, void *);
void control_close(control_server_t *);
int control_request(const char *path, const char *request, char *response, size_t size);
#endif
