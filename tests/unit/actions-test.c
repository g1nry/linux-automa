#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <fileward/action.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#define CHECK(condition)                                                                                     \
    do {                                                                                                     \
        if (!(condition)) {                                                                                  \
            fprintf(stderr, "%s:%d: %s (errno %d)\n", __FILE__, __LINE__, #condition, errno);                \
            exit(1);                                                                                         \
        }                                                                                                    \
    } while (0)

static int busy_failures, rename_calls, inject_exdev, fail_write;
static const char *mutate_source;
ssize_t __real_write(int, const void *, size_t);
ssize_t __wrap_write(int fd, const void *data, size_t length) {
    if (fail_write) {
        errno = ENOSPC;
        return -1;
    }
    return __real_write(fd, data, length);
}
ssize_t __real_read(int, void *, size_t);
ssize_t __wrap_read(int fd, void *data, size_t length) {
    ssize_t result = __real_read(fd, data, length);
    if (result > 0 && mutate_source) {
        FILE *file = fopen(mutate_source, "a");
        CHECK(file != NULL);
        CHECK(fputs("changed", file) >= 0);
        CHECK(fclose(file) == 0);
        mutate_source = NULL;
    }
    return result;
}
int __real_renameat2(int, const char *, int, const char *, unsigned);
int __wrap_renameat2(int olddir, const char *oldpath, int newdir, const char *newpath, unsigned flags) {
    rename_calls++;
    if (busy_failures > 0) {
        busy_failures--;
        errno = EBUSY;
        return -1;
    }
    if (inject_exdev && strstr(oldpath, ".fileward-") == NULL) {
        errno = EXDEV;
        return -1;
    }
    return __real_renameat2(olddir, oldpath, newdir, newpath, flags);
}

static void write_text(const char *name, const char *text) {
    FILE *file = fopen(name, "w");
    CHECK(file != NULL);
    CHECK(fputs(text, file) >= 0);
    CHECK(fclose(file) == 0);
}

static void expect_text(const char *name, const char *expected) {
    char text[256] = {0};
    FILE *file = fopen(name, "r");
    CHECK(file != NULL);
    size_t count = fread(text, 1, sizeof(text) - 1, file);
    CHECK(!ferror(file));
    CHECK(fclose(file) == 0);
    CHECK(count == strlen(expected) && strcmp(text, expected) == 0);
}

int main(void) {
    CHECK(action_handler_count() == 5);
    for (size_t i = 0; i < action_handler_count(); i++) {
        const action_handler_t *handler = action_handler_at(i);
        CHECK(handler && handler->execute && handler->capabilities);
        CHECK(action_handler_find(handler->type) == handler);
        CHECK(action_handler_named(handler->name) == handler);
        for (size_t j = 0; j < i; j++) {
            CHECK(action_handler_at(j)->type != handler->type);
            CHECK(strcmp(action_handler_at(j)->name, handler->name) != 0);
        }
    }
    CHECK(!action_handler_at(action_handler_count()));
    CHECK(!action_handler_named("undefined"));
    char out[PATH_MAX];
    CHECK(action_expand_template("{root}/{dir}/{stem}.{ext}:{name}:{relative}:{event}:{{literal}}", "/root",
                                 "a/x.txt", EVENT_MOVED_TO, out, sizeof(out)) == 0);
    CHECK(strcmp(out, "/root/a/x.txt:x.txt:a/x.txt:moved_to:{literal}") == 0);
    CHECK(action_expand_template("{stem}:{ext}:{dir}", "/root", ".env", EVENT_CREATED, out, sizeof(out)) ==
          0);
    CHECK(strcmp(out, ".env::") == 0);
    CHECK(action_expand_template("{name}", "/root", "{date}.txt", EVENT_CREATED, out, sizeof(out)) == 0);
    CHECK(strcmp(out, "{date}.txt") == 0); /* Never expand substituted input again. */
    CHECK(action_validate_template("{invalid}") != 0);
    CHECK(action_validate_template("{name") != 0);
    CHECK(action_validate_template("name}") != 0);
    CHECK(action_expand_template("{name}", "/root", "long", EVENT_CREATED, out, 2) != 0);
    char storage[1024], *arguments[FILEWARD_MAX_ARGS + 1];
    CHECK(action_split_command("/bin/echo 'a b' \"\" c\\ d '$HOME;$(id)'", storage, sizeof(storage),
                               arguments) == 5);
    CHECK(strcmp(arguments[1], "a b") == 0 && !arguments[2][0]);
    CHECK(strcmp(arguments[3], "c d") == 0 && strcmp(arguments[4], "$HOME;$(id)") == 0);
    CHECK(action_split_command("/bin/echo 'oops", storage, sizeof(storage), arguments) < 0);
    CHECK(action_split_command("/bin/echo \\", storage, sizeof(storage), arguments) < 0);
    CHECK(action_split_command("''", storage, sizeof(storage), arguments) < 0);

    char directory[] = "/tmp/fileward-actions-test-XXXXXX";
    CHECK(mkdtemp(directory) != NULL);
    CHECK(chdir(directory) == 0);
    CHECK(mkdir("inbox", 0700) == 0);
    write_text("inbox/a.txt", "original\n");
    action_t action = {0};
    action.type = ACTION_COPY;
    action.retries = 2;
    action.backoff_ms = 1;
    action.timeout_seconds = 1;
    strcpy(action.target, "output/{ext}");
    CHECK(execute_action(&action, "inbox", "a.txt", EVENT_CREATED, 1) == 0);
    CHECK(access("output", F_OK) != 0);
    CHECK(execute_action(&action, "inbox", "a.txt", EVENT_CREATED, 0) == 0);
    expect_text("output/txt/a.txt", "original\n");
    expect_text("inbox/a.txt", "original\n");
    write_text("inbox/a.txt", "new\n");
    CHECK(execute_action(&action, "inbox", "a.txt", EVENT_CREATED, 0) < 0);
    expect_text("output/txt/a.txt", "original\n");
    action.conflict = CONFLICT_SKIP;
    CHECK(execute_action(&action, "inbox", "a.txt", EVENT_CREATED, 0) == ACTION_SKIPPED);
    action.conflict = CONFLICT_RENAME;
    CHECK(execute_action(&action, "inbox", "a.txt", EVENT_CREATED, 0) == 0);
    expect_text("output/txt/a.txt.1", "new\n");
    action.conflict = CONFLICT_OVERWRITE;
    CHECK(execute_action(&action, "inbox", "a.txt", EVENT_CREATED, 0) == 0);
    expect_text("output/txt/a.txt", "new\n");
    strcpy(action.target, "inbox");
    CHECK(execute_action(&action, "inbox", "a.txt", EVENT_CREATED, 0) < 0);
    expect_text("inbox/a.txt", "new\n");

    /* Destination symlink is replaced, never followed, under explicit overwrite. */
    write_text("victim", "keep\n");
    CHECK(symlink("../../victim", "output/txt/link.txt") == 0);
    write_text("inbox/link.txt", "replacement\n");
    strcpy(action.target, "output/{ext}");
    CHECK(execute_action(&action, "inbox", "link.txt", EVENT_CREATED, 0) == 0);
    expect_text("victim", "keep\n");
    expect_text("output/txt/link.txt", "replacement\n");
    CHECK(symlink("../victim", "inbox/symlink") == 0);
    CHECK(execute_action(&action, "inbox", "symlink", EVENT_CREATED, 0) < 0);
    CHECK(mkfifo("inbox/fifo", 0600) == 0);
    CHECK(execute_action(&action, "inbox", "fifo", EVENT_CREATED, 0) < 0);
    CHECK(execute_action(&action, "inbox", "../victim", EVENT_CREATED, 0) < 0);

    /* Failed writes and changing input never replace an existing destination. */
    write_text("inbox/a.txt", "new\n");
    fail_write = 1;
    CHECK(execute_action(&action, "inbox", "a.txt", EVENT_CREATED, 0) < 0 && errno == ENOSPC);
    fail_write = 0;
    expect_text("output/txt/a.txt", "new\n");
    mutate_source = "inbox/a.txt";
    action.retries = 0;
    CHECK(execute_action(&action, "inbox", "a.txt", EVENT_CREATED, 0) < 0 && errno == EAGAIN);
    expect_text("output/txt/a.txt", "new\n");
    action.retries = 2;
    write_text("inbox/a.txt", "new\n");

    action.type = ACTION_ARCHIVE;
    strcpy(action.target, "compressed");
    CHECK(execute_action(&action, "inbox", "a.txt", EVENT_CREATED, 0) == 0);
    gzFile compressed = gzopen("compressed/a.txt.gz", "rb");
    CHECK(compressed != NULL);
    char contents[20] = {0};
    CHECK(gzread(compressed, contents, sizeof(contents)) == 4);
    CHECK(gzclose(compressed) == Z_OK && strcmp(contents, "new\n") == 0);
    expect_text("inbox/a.txt", "new\n");

    action.type = ACTION_MOVE;
    action.conflict = CONFLICT_FAIL;
    strcpy(action.target, "moved");
    busy_failures = 2;
    rename_calls = 0;
    CHECK(execute_action(&action, "inbox", "a.txt", EVENT_CREATED, 0) == 0);
    CHECK(rename_calls == 3 && busy_failures == 0);
    CHECK(access("inbox/a.txt", F_OK) != 0);
    expect_text("moved/a.txt", "new\n");
    write_text("inbox/exdev.txt", "cross-device\n");
    inject_exdev = 1;
    CHECK(execute_action(&action, "inbox", "exdev.txt", EVENT_CREATED, 0) == 0);
    inject_exdev = 0;
    CHECK(access("inbox/exdev.txt", F_OK) != 0);
    expect_text("moved/exdev.txt", "cross-device\n");
    write_text("inbox/exhaust.txt", "retain\n");
    busy_failures = 10;
    rename_calls = 0;
    CHECK(execute_action(&action, "inbox", "exhaust.txt", EVENT_CREATED, 0) < 0);
    CHECK(rename_calls == 3);
    expect_text("inbox/exhaust.txt", "retain\n");
    busy_failures = 0;
    volatile sig_atomic_t running = 0;
    action_set_running_flag(&running);
    CHECK(action_cancelled() == 1 && errno == ECANCELED);
    CHECK(execute_action(&action, "inbox", "exhaust.txt", EVENT_CREATED, 0) < 0 && errno == ECANCELED);
    action_set_running_flag(NULL);

    action.type = ACTION_EXEC;
    strcpy(action.command, "/bin/true");
    CHECK(execute_action(&action, "inbox", "a.txt", EVENT_CREATED, 0) == 0);
    strcpy(action.command, "/bin/false");
    CHECK(execute_action(&action, "inbox", "a.txt", EVENT_CREATED, 0) < 0);
    strcpy(action.command, "/bin/sleep 3");
    CHECK(execute_action(&action, "inbox", "a.txt", EVENT_CREATED, 0) < 0 && errno == ETIMEDOUT);
    /* Test files live in a disposable directory; remove without a shell. */
    char absolute[PATH_MAX];
    CHECK(getcwd(absolute, sizeof(absolute)) != NULL);
    CHECK(chdir("/") == 0);
    char command[PATH_MAX + 64];
    snprintf(command, sizeof(command), "/bin/rm -rf -- %s", absolute);
    CHECK(strlen(command) < sizeof(action.command));
    strcpy(action.command, command);
    action.timeout_seconds = 5;
    CHECK(execute_action(&action, "/", "cleanup", EVENT_CREATED, 0) == 0);
    puts("actions tests passed");
    return 0;
}
