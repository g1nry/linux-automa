#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <fileward/audit.h>
#include <fileward/log.h>
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
#include <stdarg.h>

#ifndef FILEWARD_VERSION
#define FILEWARD_VERSION "unknown"
#endif
#define LINE_SIZE AUDIT_RECORD_MAX
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
    char executable[PATH_MAX];
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
    int saved = errno;
    if (output >= 0) {
        if (fsync(output)) {
            int error = errno;
            atomic_store(&failed, 1);
            log_failure(3, &(log_failure_t){.kind = "audit_error", .code = "FW_JOURNAL_SYNC",
                        .message = "fsync before closing journal failed", .error = error,
                        .destination = journal_path, .exit_code = -1});
        }
        close(output);
        output = -1;
    }
    if (lock_fd >= 0) {
        close(lock_fd);
        lock_fd = -1;
    }
    errno = saved;
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
void audit_executable(const char *path) {
    snprintf(context.executable, sizeof(context.executable), "%s", path ? path : "");
}
void audit_rule(const char *rule, const char *action) {
    context.destination[0] = 0;
    context.exit_code = -1;
    context.signal = 0;
    context.executable[0] = 0;
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
/* One bounded record, owned by the journal mutex. No per-record allocation. */
typedef struct {
    char *data;
    size_t length;
    int broken;
} json_buffer_t;
static void bytes(json_buffer_t *buffer, const void *data, size_t length) {
    if (buffer->broken) {
        return;
    }
    if (length > LINE_SIZE - buffer->length) {
        buffer->broken = 1;
        return;
    }
    memcpy(buffer->data + buffer->length, data, length);
    buffer->length += length;
}
static void raw(json_buffer_t *buffer, const char *text) {
    bytes(buffer, text, strlen(text));
}
static void format(json_buffer_t *buffer, const char *text, ...) __attribute__((format(printf, 2, 3)));
static void format(json_buffer_t *buffer, const char *text, ...) {
    if (buffer->broken) {
        return;
    }
    va_list arguments;
    va_start(arguments, text);
    int count = vsnprintf(buffer->data + buffer->length, LINE_SIZE + 1 - buffer->length, text, arguments);
    va_end(arguments);
    if (count < 0 || (size_t)count > LINE_SIZE - buffer->length) {
        buffer->broken = 1;
    } else {
        buffer->length += (size_t)count;
    }
}
static void string(json_buffer_t *buffer, const char *value) {
    static const char hex[] = "0123456789abcdef";
    raw(buffer, "\"");
    for (const unsigned char *p = (const unsigned char *)value; *p && !buffer->broken;) {
        const unsigned char *start = p;
        while (*p >= 32 && *p < 128 && *p != '"' && *p != '\\') {
            p++;
        }
        bytes(buffer, start, (size_t)(p - start));
        if (!*p) {
            break;
        }
        if (*p == '"' || *p == '\\') {
            char escaped[] = {'\\', (char)*p++};
            bytes(buffer, escaped, sizeof(escaped));
        } else if (*p < 32) {
            char escaped[] = {'\\', 'u', '0', '0', hex[*p >> 4], hex[*p & 15]};
            bytes(buffer, escaped, sizeof(escaped));
            p++;
        } else {
            int length = utf8_length(p);
            if (!length) {
                raw(buffer, "\\ufffd");
                p++;
            } else {
                bytes(buffer, p, (size_t)length);
                p += length;
            }
        }
    }
    raw(buffer, "\"");
}
static void field(json_buffer_t *buffer, const char *key, const char *value) {
    raw(buffer, ",\"");
    raw(buffer, key);
    raw(buffer, "\":");
    string(buffer, value ? value : "");
}
static void base64(json_buffer_t *buffer, const unsigned char *value) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t length = strlen((const char *)value);
    if (buffer->broken) {
        return;
    }
    if (length > LINE_SIZE / 4 * 3) {
        buffer->broken = 1;
        return;
    }
    size_t encoded = 4 * ((length + 2) / 3) + 2;
    if (encoded > LINE_SIZE - buffer->length) {
        buffer->broken = 1;
        return;
    }
    char *out = buffer->data + buffer->length;
    *out++ = '"';
    for (size_t i = 0; i < length; i += 3) {
        unsigned bits = (unsigned)value[i] << 16;
        if (i + 1 < length) {
            bits |= (unsigned)value[i + 1] << 8;
        }
        if (i + 2 < length) {
            bits |= value[i + 2];
        }
        *out++ = alphabet[(bits >> 18) & 63];
        *out++ = alphabet[(bits >> 12) & 63];
        *out++ = i + 1 < length ? alphabet[(bits >> 6) & 63] : '=';
        *out++ = i + 2 < length ? alphabet[bits & 63] : '=';
    }
    *out = '"';
    buffer->length += encoded;
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

static void report(const char *kind, const char *outcome, const char *code, int error,
                   const char *message, int attempt, long long duration, const char *id) {
    /* The final action_result carries exec status too; avoid reporting the same
     * command failure twice. diagnostic already has its own stderr writer. */
    if (!strcmp(kind, "exec_status") || !strcmp(kind, "diagnostic")) return;
    int priority = !strcmp(outcome, "failure") ? 3
                   : (!strcmp(outcome, "warning") || !strcmp(outcome, "retry")) ? 4 : 0;
    if (!priority) return;
    log_failure(priority, &(log_failure_t){.kind = kind, .code = code, .error = error,
                .message = message, .root = context.root, .path = context.path, .event = context.event,
                .rule = context.rule, .action = context.action, .destination = context.destination,
                .executable = context.executable, .event_id = id, .parent_id = context.parent,
                .attempt = attempt, .exit_code = context.exit_code, .signal = context.signal,
                .generation = context.generation, .duration_ms = duration});
}

void audit_emit(const char *kind, const char *outcome, const char *code, int error, const char *message,
                int attempt, long long duration_ms, char id[AUDIT_ID_SIZE]) {
    int saved = errno;
    int journal_error = 0;
    const char *operation = NULL;
    pthread_mutex_lock(&mutex);
    if (output < 0 || atomic_load(&failed)) {
        if (id) {
            id[0] = 0;
        }
        pthread_mutex_unlock(&mutex);
        report(kind, outcome, code, error, message, attempt, duration_ms, NULL);
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
    static char line[LINE_SIZE + 1];
    json_buffer_t record = {.data = line};
    json_buffer_t *file = &record;
    format(file, "{\"schema_version\":1,\"sequence\":%llu,\"timestamp\":\"%s.%03ldZ\"",
           (unsigned long long)sequence, timestamp, now.tv_nsec / 1000000);
    field(file, "provider", "fileward");
    field(file, "program_version", FILEWARD_VERSION);
    format(file, ",\"daemon_pid\":%ld", (long)getpid());
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
    raw(file, ",\"path_base64\":");
    base64(file, (const unsigned char *)context.path);
    field(file, "rule_name", context.rule);
    field(file, "action", context.action);
    field(file, "message", message);
    field(file, "destination", context.destination);
    raw(file, ",\"destination_base64\":");
    base64(file, (const unsigned char *)context.destination);
    if (context.metadata_valid) {
        format(file, ",\"file_size\":%lld,\"file_mode\":%u,\"mtime_unix\":%lld",
               (long long)context.metadata.st_size, (unsigned)context.metadata.st_mode,
               (long long)context.metadata.st_mtime);
    } else {
        raw(file, ",\"file_size\":null,\"file_mode\":null,\"mtime_unix\":null");
    }
    format(file, ",\"exit_code\":%d,\"signal\":%d", context.exit_code, context.signal);
    format(file,
           ",\"errno\":%d,\"attempt\":%d,\"duration_ms\":%lld,\"config_generation\":%u,\"dry_run\":%s}\n",
           error, attempt, duration_ms, context.generation, context.dry_run ? "true" : "false");
    size_t length = record.length;
    if (record.broken) {
        journal_error = EMSGSIZE;
        operation = "serialize record: AUDIT_RECORD_MAX exceeded";
        atomic_store(&failed, 1);
        goto finish;
    }
    if (rotate(length)) {
        journal_error = errno;
        operation = "rotate journal";
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
            journal_error = count < 0 ? errno : EIO;
            operation = "append record to journal";
            atomic_store(&failed, 1);
            break;
        }
        done += (size_t)count;
    }
    if (!atomic_load(&failed) && sync_records && fdatasync(output)) {
        journal_error = errno;
        operation = "fdatasync journal record";
        atomic_store(&failed, 1);
    }
finish:
    pthread_mutex_unlock(&mutex);
    report(kind, outcome, code, error, message, attempt, duration_ms, event_id);
    if (journal_error) {
        log_failure(3, &(log_failure_t){.kind = "audit_error", .code = "FW_JOURNAL_WRITE",
                    .message = operation, .error = journal_error, .destination = journal_path,
                    .event_id = event_id, .exit_code = -1});
    }
    errno = saved;
}

void audit_diagnostic(const char *level, const char *message) {
    audit_emit("diagnostic", level, strcmp(level, "error") == 0 ? "FW_DIAGNOSTIC_ERROR" : "FW_OK", 0, message,
               0, 0, NULL);
}
