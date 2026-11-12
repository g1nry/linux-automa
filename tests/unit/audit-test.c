#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <fileward/audit.h>
#include <fileward/log.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define CHECK(c)                                                                                             \
    do {                                                                                                     \
        if (!(c)) {                                                                                          \
            fprintf(stderr, "audit check failed at %d: %s\n", __LINE__, #c);                                 \
            exit(1);                                                                                         \
        }                                                                                                    \
    } while (0)
static int fail_write, interruptions, short_writes, zero_write, fail_fsync, fail_fdatasync;
ssize_t __real_write(int, const void *, size_t);
int __real_fsync(int);
int __real_fdatasync(int);
int __wrap_fsync(int fd) {
    if (fail_fsync) { errno = EIO; return -1; }
    return __real_fsync(fd);
}
int __wrap_fdatasync(int fd) {
    if (fail_fdatasync) { errno = EIO; return -1; }
    return __real_fdatasync(fd);
}
ssize_t __wrap_write(int fd, const void *data, size_t length) {
    if (zero_write) return 0;
    if (fail_write) {
        errno = ENOSPC;
        return -1;
    }
    if (interruptions > 0) {
        interruptions--;
        errno = EINTR;
        return -1;
    }
    if (short_writes && length > 7) {
        length = 7;
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
    interruptions = 2;
    short_writes = 1;
    errno = EACCES;
    audit_emit("action_result", "success", "FW_OK", 0, "partial writes", 1, 0, NULL);
    CHECK(!audit_failed() && errno == EACCES && !interruptions);
    short_writes = 0;
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
    CHECK(audit_open(path, 0) == 0);
    audit_emit("probe", "success", "FW_OK", 0, "", 0, 0, NULL);
    struct stat before, after;
    CHECK(!stat(path, &before) && before.st_size > 0);
    size_t message_size = AUDIT_RECORD_MAX - (size_t)before.st_size;
    char *message = malloc(message_size + 2);
    CHECK(message);
    memset(message, 'a', message_size + 1);
    message[message_size] = 0;
    audit_emit("probe", "success", "FW_OK", 0, message, 0, 0, NULL);
    CHECK(!audit_failed());
    CHECK(!stat(path, &after) && after.st_size == before.st_size + AUDIT_RECORD_MAX);
    before = after;
    message[message_size] = 'a';
    message[message_size + 1] = 0;
    audit_emit("probe", "success", "FW_OK", 0, message, 0, 0, NULL);
    CHECK(audit_failed());
    CHECK(!stat(path, &after) && after.st_size == before.st_size);
    free(message);
    audit_close();
    CHECK(audit_open(path, 0) == 0); /* Oversize rejection left a complete tail. */
    audit_close();
    /* Reporting audit failures cannot depend on another successful audit write.
     * Capture their stage, actual errno and target path, including zero-write. */
    FILE *diagnostics = tmpfile();
    CHECK(diagnostics);
    int original_stderr = dup(STDERR_FILENO);
    CHECK(original_stderr >= 0);
    CHECK(dup2(fileno(diagnostics), STDERR_FILENO) >= 0);
    int sync_failed, close_failed, zero_failed, preserved;
    audit_open(path, 0);
    fail_fdatasync = 1;
    audit_set_sync(1);
    errno = ERANGE;
    audit_emit("probe", "success", "FW_OK", 0, "sync failure", 0, 0, NULL);
    sync_failed = audit_failed();
    preserved = errno == ERANGE;
    fail_fdatasync = 0;
    audit_set_sync(0);
    audit_close();
    audit_open(path, 0);
    fail_fsync = 1;
    errno = EDOM;
    audit_close();
    close_failed = audit_failed();
    preserved &= errno == EDOM;
    fail_fsync = 0;
    audit_open(path, 0);
    zero_write = 1;
    errno = EACCES;
    audit_emit("probe", "success", "FW_OK", 0, "zero write", 0, 0, NULL);
    zero_failed = audit_failed();
    preserved &= errno == EACCES;
    zero_write = 0;
    audit_close();
    char long_detail[5001];
    memset(long_detail, 'A', sizeof(long_detail) - 1);
    long_detail[sizeof(long_detail) - 1] = 0;
    log_failure(3, &(log_failure_t){.kind = "test", .code = "FW_IO_ERROR", .message = long_detail,
                                  .event_id = "still-visible", .exit_code = -1});
    fflush(stderr);
    CHECK(dup2(original_stderr, STDERR_FILENO) >= 0);
    close(original_stderr);
    CHECK(sync_failed && close_failed && zero_failed && preserved);
    rewind(diagnostics);
    char text[8192];
    size_t used = fread(text, 1, sizeof(text) - 1, diagnostics);
    text[used] = 0;
    fclose(diagnostics);
    CHECK(strstr(text, "FW_JOURNAL_WRITE:") && strstr(text, "FW_JOURNAL_SYNC:"));
    CHECK(strstr(text, "fdatasync journal record") && strstr(text, "fsync before closing journal"));
    CHECK(strstr(text, "append record to journal") && strstr(text, "errno=5") && strstr(text, path));
    CHECK(strstr(text, "[truncated]") && strstr(text, "event_id=\"still-visible\"") && used < 5000);
    unlink(path);
    char lock[256];
    snprintf(lock, sizeof(lock), "%s.lock", path);
    unlink(lock);
    puts("audit tests passed");
    return 0;
}
