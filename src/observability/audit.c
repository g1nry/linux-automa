#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <fileward/audit.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef FILEWARD_VERSION
#define FILEWARD_VERSION "unknown"
#endif
#define LINE_SIZE 131072
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static int output = -1, lock_fd = -1, sync_records;
void audit_set_sync(int enabled) {
    sync_records = enabled;
}
static atomic_int failed;
static uint64_t sequence, maximum;
static char journal_path[PATH_MAX], run_id[33], host_id[80], host_name[256];
static _Thread_local struct {
    char root[PATH_MAX], path[PATH_MAX], event[32], parent[AUDIT_ID_SIZE], rule[64], action[32];
    char destination[PATH_MAX];
    int exit_code, signal, metadata_valid;
    struct stat metadata;
    unsigned generation;
    int dry_run;
} context;

const char *audit_error_code(int error) {
    switch (error) {
    case 0:
        return "FW_OK";
    case ENOENT:
        return "FW_FILE_NOT_FOUND";
    case EACCES:
    case EPERM:
        return "FW_PERMISSION_DENIED";
    case EEXIST:
        return "FW_DESTINATION_EXISTS";
    case ENOSPC:
    case EDQUOT:
        return "FW_DISK_FULL";
    case ENAMETOOLONG:
        return "FW_PATH_TOO_LONG";
    case EXDEV:
        return "FW_CROSS_DEVICE_MOVE";
    case EINVAL:
        return "FW_INVALID_ARGUMENT";
    case ELOOP:
        return "FW_SYMLINK_REJECTED";
    case ENOTSUP:
        return "FW_UNSUPPORTED_OBJECT";
    case EAGAIN:
        return "FW_SOURCE_CHANGED";
    case ESTALE:
        return "FW_SOURCE_REPLACED";
    case ECANCELED:
        return "FW_CANCELLED";
    case ETIMEDOUT:
        return "FW_EXEC_TIMEOUT";
    case ECHILD:
        return "FW_EXEC_FAILED";
    case EMFILE:
    case ENFILE:
        return "FW_FD_LIMIT";
    case ENOMEM:
        return "FW_MEMORY_LIMIT";
    case EBUSY:
    case ETXTBSY:
        return "FW_RESOURCE_BUSY";
    default:
        return "FW_IO_ERROR";
    }
}

static int parents(const char *path) {
    char copy[PATH_MAX];
    if (strlen(path) >= sizeof(copy)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(copy, path);
    for (char *p = copy + 1; *p; p++) {
        if (*p != '/') {
            continue;
        }
        *p = 0;
        if (mkdir(copy, 0700) != 0) {
            struct stat st;
            if (errno != EEXIST || stat(copy, &st) || !S_ISDIR(st.st_mode)) {
                return -1;
            }
        }
        *p = '/';
    }
    return 0;
}

static int open_regular(const char *path) {
    int fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    struct stat st;
    if (fd >= 0 && (fstat(fd, &st) || !S_ISREG(st.st_mode))) {
        close(fd);
        errno = EINVAL;
        return -1;
    }
    return fd;
}

int audit_open(const char *path, uint64_t max_bytes) {
    if (!path || !*path || strlen(path) >= sizeof(journal_path) - 8) {
        errno = EINVAL;
        return -1;
    }
    if (parents(path)) {
        return -1;
    }
    snprintf(journal_path, sizeof(journal_path), "%s", path);
    char lock_path[PATH_MAX];
    snprintf(lock_path, sizeof(lock_path), "%s.lock", path);
    lock_fd = open_regular(lock_path);
    if (lock_fd < 0) {
        return -1;
    }
    if (flock(lock_fd, LOCK_EX | LOCK_NB)) {
        close(lock_fd);
        lock_fd = -1;
        return -1;
    }
    output = open_regular(path);
    if (output < 0) {
        close(lock_fd);
        lock_fd = -1;
        return -1;
    }
    /* Refuse an incomplete tail rather than joining two different JSON records. */
    int reader = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat st;
    char last;
    int broken = reader < 0 || fstat(reader, &st) ||
                 (st.st_size && (pread(reader, &last, 1, st.st_size - 1) != 1 || last != '\n'));
    if (reader >= 0) {
        close(reader);
    }
    if (broken) {
        audit_close();
        errno = EILSEQ;
        return -1;
    }
    unsigned char random[16];
    if (getrandom(random, sizeof(random), 0) != sizeof(random)) {
        audit_close();
        errno = EIO;
        return -1;
    }
    for (int i = 0; i < 16; i++) {
        snprintf(run_id + i * 2, 3, "%02x", random[i]);
    }
    gethostname(host_name, sizeof(host_name) - 1);
    FILE *machine = fopen("/etc/machine-id", "re");
    if (machine) {
        if (!fgets(host_id, sizeof(host_id), machine)) {
            host_id[0] = 0;
        }
        fclose(machine);
    }
    host_id[strcspn(host_id, "\r\n")] = 0;
    if (!host_id[0]) {
        snprintf(host_id, sizeof(host_id), "%s", run_id);
    }
    maximum = max_bytes;
    sequence = 0;
    atomic_store(&failed, 0);
    audit_context(NULL, NULL, NULL, 0, 0, NULL);
    return 0;
}

void audit_close(void) {
    if (output >= 0) {
        if (fsync(output)) {
            atomic_store(&failed, 1);
            fprintf(stderr, "FW_JOURNAL_SYNC: %s\n", strerror(errno));
        }
        close(output);
        output = -1;
    }
    if (lock_fd >= 0) {
        close(lock_fd);
        lock_fd = -1;
    }
}
int audit_failed(void) {
    return atomic_load(&failed);
}

void audit_context(const char *root, const char *path, const char *event, unsigned generation, int dry_run,
                   const char *parent) {
    memset(&context, 0, sizeof(context));
    snprintf(context.root, sizeof(context.root), "%s", root ? root : "");
    snprintf(context.path, sizeof(context.path), "%s", path ? path : "");
    snprintf(context.event, sizeof(context.event), "%s", event ? event : "");
    snprintf(context.parent, sizeof(context.parent), "%s", parent ? parent : "");
    context.exit_code = -1;
    context.generation = generation;
    context.dry_run = dry_run;
}
void audit_metadata(const struct stat *metadata) {
    context.metadata_valid = metadata != NULL;
    if (metadata) {
        context.metadata = *metadata;
    }
}
void audit_destination(const char *path) {
    snprintf(context.destination, sizeof(context.destination), "%s", path ? path : "");
}
void audit_exec_status(int exit_code, int signal) {
    context.exit_code = exit_code;
    context.signal = signal;
}
void audit_rule(const char *rule, const char *action) {
    context.destination[0] = 0;
    context.exit_code = -1;
    context.signal = 0;
    snprintf(context.rule, sizeof(context.rule), "%s", rule ? rule : "");
    snprintf(context.action, sizeof(context.action), "%s", action ? action : "");
}

/* Preserve valid UTF-8; replace invalid bytes for display. path_base64 is lossless. */
static int utf8_length(const unsigned char *p) {
    unsigned a = p[0];
    if (a < 128) {
        return 1;
    }
    int length = a >= 0xc2 && a <= 0xdf ? 2 : a >= 0xe0 && a <= 0xef ? 3 : a >= 0xf0 && a <= 0xf4 ? 4 : 0;
    for (int i = 1; i < length; i++) {
        if (!p[i] || (p[i] & 0xc0) != 0x80) {
            return 0;
        }
    }
    if (length == 3 && ((a == 0xe0 && p[1] < 0xa0) || (a == 0xed && p[1] >= 0xa0))) {
        return 0;
    }
    if (length == 4 && ((a == 0xf0 && p[1] < 0x90) || (a == 0xf4 && p[1] >= 0x90))) {
        return 0;
    }
    return length;
}
static void string(FILE *file, const char *value) {
    fputc('"', file);
    for (const unsigned char *p = (const unsigned char *)value; *p;) {
        if (*p == '"' || *p == '\\') {
            fputc('\\', file);
            fputc(*p++, file);
        } else if (*p < 32) {
            fprintf(file, "\\u%04x", *p++);
        } else {
            int length = utf8_length(p);
            if (!length) {
                fputs("\\ufffd", file);
                p++;
            } else {
                fwrite(p, 1, (size_t)length, file);
                p += length;
            }
        }
    }
    fputc('"', file);
}
static void field(FILE *file, const char *key, const char *value) {
    fprintf(file, ",\"%s\":", key);
    string(file, value ? value : "");
}
static void base64(FILE *file, const unsigned char *value) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t length = strlen((const char *)value);
    fputc('"', file);
    for (size_t i = 0; i < length; i += 3) {
        unsigned bits = (unsigned)value[i] << 16;
        if (i + 1 < length) {
            bits |= (unsigned)value[i + 1] << 8;
        }
        if (i + 2 < length) {
            bits |= value[i + 2];
        }
        fputc(alphabet[(bits >> 18) & 63], file);
        fputc(alphabet[(bits >> 12) & 63], file);
        fputc(i + 1 < length ? alphabet[(bits >> 6) & 63] : '=', file);
        fputc(i + 2 < length ? alphabet[bits & 63] : '=', file);
    }
    fputc('"', file);
}

static int rotate(size_t length) {
    struct stat st;
    if (fstat(output, &st)) {
        return -1;
    }
    if (!maximum || (uint64_t)st.st_size + length <= maximum || !st.st_size) {
        return 0;
    }
    if (fsync(output)) {
        return -1;
    }
    for (int i = 3; i >= 1; i--) {
        char from[PATH_MAX + 8], to[PATH_MAX + 8];
        if (i == 1) {
            snprintf(from, sizeof(from), "%s", journal_path);
        } else {
            snprintf(from, sizeof(from), "%s.%d", journal_path, i - 1);
        }
        snprintf(to, sizeof(to), "%s.%d", journal_path, i);
        if (rename(from, to) && errno != ENOENT) {
            return -1;
        }
    }
    close(output);
    output = open_regular(journal_path);
    return output < 0 ? -1 : 0;
}

void audit_emit(const char *kind, const char *outcome, const char *code, int error, const char *message,
                int attempt, long long duration_ms, char id[AUDIT_ID_SIZE]) {
    int saved = errno;
    pthread_mutex_lock(&mutex);
    if (output < 0 || atomic_load(&failed)) {
        if (id) {
            id[0] = 0;
        }
        pthread_mutex_unlock(&mutex);
        errno = saved;
        return;
    }
    char event_id[AUDIT_ID_SIZE], timestamp[40];
    struct timespec now;
    struct tm utc;
    clock_gettime(CLOCK_REALTIME, &now);
    gmtime_r(&now.tv_sec, &utc);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S", &utc);
    snprintf(event_id, sizeof(event_id), "%s-%llu", run_id, (unsigned long long)++sequence);
    if (id) {
        snprintf(id, AUDIT_ID_SIZE, "%s", event_id);
    }
    char *line = NULL;
    size_t length = 0;
    FILE *file = open_memstream(&line, &length);
    if (!file) {
        atomic_store(&failed, 1);
        goto finish;
    }
    fprintf(file, "{\"schema_version\":1,\"sequence\":%llu,\"timestamp\":\"%s.%03ldZ\"",
            (unsigned long long)sequence, timestamp, now.tv_nsec / 1000000);
    field(file, "provider", "fileward");
    field(file, "program_version", FILEWARD_VERSION);
    fprintf(file, ",\"daemon_pid\":%ld", (long)getpid());
    field(file, "event_id", event_id);
    field(file, "run_id", run_id);
    field(file, "parent_id", context.parent);
    field(file, "host_id", host_id);
    field(file, "host_name", host_name);
    field(file, "kind", kind);
    field(file, "outcome", outcome);
    field(file, "code", code);
    field(file, "event_type", context.event);
    field(file, "watch_root", context.root);
    field(file, "relative_path", context.path);
    fputs(",\"path_base64\":", file);
    base64(file, (const unsigned char *)context.path);
    field(file, "rule_name", context.rule);
    field(file, "action", context.action);
    field(file, "message", message);
    field(file, "destination", context.destination);
    fputs(",\"destination_base64\":", file);
    base64(file, (const unsigned char *)context.destination);
    if (context.metadata_valid) {
        fprintf(file, ",\"file_size\":%lld,\"file_mode\":%u,\"mtime_unix\":%lld",
                (long long)context.metadata.st_size, (unsigned)context.metadata.st_mode,
                (long long)context.metadata.st_mtime);
    } else {
        fputs(",\"file_size\":null,\"file_mode\":null,\"mtime_unix\":null", file);
    }
    fprintf(file, ",\"exit_code\":%d,\"signal\":%d", context.exit_code, context.signal);
    fprintf(file,
            ",\"errno\":%d,\"attempt\":%d,\"duration_ms\":%lld,\"config_generation\":%u,\"dry_run\":%s}\n",
            error, attempt, duration_ms, context.generation, context.dry_run ? "true" : "false");
    int broken = ferror(file);
    if (fclose(file)) {
        broken = 1;
    }
    if (broken || length > LINE_SIZE || rotate(length)) {
        free(line);
        atomic_store(&failed, 1);
        goto finish;
    }
    size_t done = 0;
    while (done < length) {
        ssize_t count = write(output, line + done, length - done);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            atomic_store(&failed, 1);
            break;
        }
        done += (size_t)count;
    }
    free(line);
    if (!atomic_load(&failed) && sync_records && fdatasync(output)) {
        atomic_store(&failed, 1);
    }
finish:
    if (atomic_load(&failed)) {
        fprintf(stderr, "FW_JOURNAL_WRITE: audit output failed; stopping execution\n");
    }
    pthread_mutex_unlock(&mutex);
    errno = saved;
}

void audit_diagnostic(const char *level, const char *message) {
    audit_emit("diagnostic", level, strcmp(level, "error") == 0 ? "FW_DIAGNOSTIC_ERROR" : "FW_OK", 0, message,
               0, 0, NULL);
}
