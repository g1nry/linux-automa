#define _GNU_SOURCE
#include <fileward/state.h>
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void *append(void *store) {
    for (int i = 0; i < 100; i++) {
        assert(state_store_append(store, "/watch", "name.txt", "created", "log") == 0);
    }
    return NULL;
}
int main(void) {
    char folder[] = "/tmp/fw-state-XXXXXX";
    assert(mkdtemp(folder));
    char path[512], link[512], fifo[512];
    snprintf(path, sizeof(path), "%s/success.state", folder);
    snprintf(link, sizeof(link), "%s/link", folder);
    snprintf(fifo, sizeof(fifo), "%s/fifo", folder);
    state_store_t *store = state_store_create(path);
    assert(store);
    pthread_t threads[4];
    for (int i = 0; i < 4; i++) {
        assert(!pthread_create(threads + i, NULL, append, store));
    }
    for (int i = 0; i < 4; i++) {
        assert(!pthread_join(threads[i], NULL));
    }
    state_store_destroy(store);
    FILE *file = fopen(path, "r");
    assert(file);
    char line[512];
    int count = 0;
    while (fgets(line, sizeof(line), file)) {
        assert(strstr(line, "\tcreated\t/watch\tname.txt\tlog\n"));
        count++;
    }
    assert(count == 400 && !ferror(file));
    fclose(file);
    assert(!symlink(path, link));
    store = state_store_create(link);
    assert(store && state_store_append(store, "/", "a", "created", "log") == -1 && errno == ELOOP);
    state_store_destroy(store);
    assert(!mkfifo(fifo, 0600));
    store = state_store_create(fifo);
    assert(store && state_store_append(store, "/", "a", "created", "log") == -1);
    state_store_destroy(store);
    store = state_store_create(NULL);
    assert(store && !state_store_append(store, "/", "a", "created", "log"));
    state_store_destroy(store);
    assert(!unlink(path) && !unlink(link) && !unlink(fifo) && !rmdir(folder));
    puts("state tests passed");
}
