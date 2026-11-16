#define _GNU_SOURCE
#include <fileward/audit.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(c)                                                                                             \
    do {                                                                                                     \
        if (!(c)) {                                                                                          \
            fprintf(stderr, "query check %d: %s\n", __LINE__, #c);                                           \
            exit(1);                                                                                         \
        }                                                                                                    \
    } while (0)
static char active[4096], retained[4096];
static int mode;
int __real_open(const char *, int, ...);
int __real_open64(const char *, int, ...);
int __real_lstat(const char *, struct stat *);
int __real_lstat64(const char *, struct stat64 *);
ssize_t __real_pread(int, void *, size_t, off_t);
ssize_t __real_pread64(int, void *, size_t, off64_t);
static void record(const char *path, const char *id) {
    FILE *f = fopen(path, "w");
    CHECK(f);
    fprintf(f,
            "{\"schema_version\":1,\"kind\":\"action_result\",\"outcome\":\"success\",\"code\":\"FW_OK\","
            "\"event_id\":\"%s\"}\n",
            id);
    CHECK(!fclose(f));
}
static void opening(const char *path) {
    if (mode == 1 && !strcmp(path, retained)) {
        mode = 0;
        CHECK(!rename(active, retained));
        record(active, "new");
    }
}
int __wrap_open(const char *path, int flags, ...) {
    opening(path);
    return __real_open(path, flags);
}
int __wrap_open64(const char *path, int flags, ...) {
    opening(path);
    return __real_open64(path, flags);
}
int __wrap_lstat(const char *path, struct stat *st) {
    int result = __real_lstat(path, st);
    if (!result && mode == 4)
        st->st_ino++;
    return result;
}
int __wrap_lstat64(const char *path, struct stat64 *st) {
    int result = __real_lstat64(path, st);
    if (!result && mode == 4)
        st->st_ino++;
    return result;
}
static void reading(void) {
    if (mode == 2) {
        mode = 0;
        CHECK(!unlink(retained));
    }
    if (mode == 3) {
        mode = 0;
        CHECK(!truncate(active, 0));
    }
}
ssize_t __wrap_pread(int fd, void *out, size_t n, off_t pos) {
    reading();
    return __real_pread(fd, out, n, pos);
}
ssize_t __wrap_pread64(int fd, void *out, size_t n, off64_t pos) {
    reading();
    return __real_pread64(fd, out, n, pos);
}
static void query(int scenario, int expected, const char *count) {
    FILE *out = tmpfile();
    CHECK(out);
    int saved = dup(1);
    CHECK(saved >= 0 && dup2(fileno(out), 1) >= 0);
    mode = scenario;
    int result = audit_query(active, 1, 0, 20, NULL);
    mode = 0;
    fflush(stdout);
    CHECK(dup2(saved, 1) >= 0);
    close(saved);
    CHECK(result == expected);
    rewind(out);
    char text[4096];
    CHECK(fgets(text, sizeof(text), out));
    fclose(out);
    CHECK(strstr(text, count));
}
int main(void) {
    char root[] = "/tmp/fw-query-test-XXXXXX";
    CHECK(mkdtemp(root));
    snprintf(active, sizeof(active), "%s/events", root);
    snprintf(retained, sizeof(retained), "%s/events.1", root);
    record(active, "old");
    query(1, 0, "\"records\":2,");
    query(2, 0, "\"records\":2,"); /* Deleted names remain readable through pinned fds. */
    query(4, 3, "\"malformed_records\":1");
    query(3, 3, "\"malformed_records\":1");
    unlink(active);
    unlink(retained);
    rmdir(root);
    puts("query rotation, pinned segments, truncation and bounded retries passed");
}
