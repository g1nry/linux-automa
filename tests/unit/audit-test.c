#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <fileward/audit.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(c)                                                                                             \
    do {                                                                                                     \
        if (!(c)) {                                                                                          \
            fprintf(stderr, "audit check failed at %d: %s\n", __LINE__, #c);                                 \
            exit(1);                                                                                         \
        }                                                                                                    \
    } while (0)
static int fail_write;
ssize_t __real_write(int, const void *, size_t);
ssize_t __wrap_write(int fd, const void *data, size_t length) {
    if (fail_write) {
        errno = ENOSPC;
        return -1;
    }
    return __real_write(fd, data, length);
}
int main(void) {
    char path[] = "/tmp/fileward-audit-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    close(fd);
    CHECK(audit_open(path, 4096) == 0);
    audit_context("/watch", "quote\"\n\xff", "created", 3, 0, NULL);
    audit_emit("file_observed", "observed", "FW_OK", 0, "message", 0, 0, NULL);
    CHECK(!audit_failed());
    fail_write = 1;
    audit_emit("action_result", "failure", "FW_DISK_FULL", ENOSPC, "full", 0, 0, NULL);
    CHECK(audit_failed());
    fail_write = 0;
    audit_close();
    CHECK(audit_open(path, 4096) == 0);
    CHECK(!audit_failed());
    audit_close();
    FILE *file = fopen(path, "a");
    CHECK(file);
    fputs("{partial", file);
    fclose(file);
    CHECK(audit_open(path, 4096) < 0 && errno == EILSEQ);
    unlink(path);
    char lock[256];
    snprintf(lock, sizeof(lock), "%s.lock", path);
    unlink(lock);
    puts("audit tests passed");
    return 0;
}
