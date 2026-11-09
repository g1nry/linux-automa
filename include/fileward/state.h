#ifndef FILEWARD_STATE_H
#define FILEWARD_STATE_H
/* Optional legacy success journal; JSONL remains the authoritative audit. */
typedef struct state_store state_store_t;
state_store_t *state_store_create(const char *path);
int state_store_append(state_store_t *, const char *root, const char *path, const char *event,
                       const char *action);
void state_store_destroy(state_store_t *);
#endif
