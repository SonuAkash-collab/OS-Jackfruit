/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Intentionally partial starter:
 *   - command-line shape is defined
 *   - key runtime data structures are defined
 *   - bounded-buffer skeleton is defined
 *   - supervisor / client split is outlined
 *
 * Students are expected to design:
 *   - the control-plane IPC implementation
 *   - container lifecycle and metadata synchronization
 *   - clone + namespace setup for each container
 *   - producer/consumer behavior for log buffering
 *   - signal handling and graceful shutdown
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    char log_path[PATH_MAX];
    int stop_requested;
    void *child_stack;
    int in_use;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
    size_t container_count;
    size_t container_capacity;
} supervisor_ctx_t;

static volatile sig_atomic_t g_supervisor_stop = 0;
static volatile sig_atomic_t g_supervisor_sigchld = 0;

static void supervisor_signal_handler(int signo)
{
    if (signo == SIGCHLD) {
        g_supervisor_sigchld = 1;
        return;
    }

    g_supervisor_stop = 1;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag,
                          const char *value,
                          unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }

    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }

    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                int argc,
                                char *argv[],
                                int start_index)
{
    int i;

    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }

        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }

        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }

    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }

    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING:
        return "starting";
    case CONTAINER_RUNNING:
        return "running";
    case CONTAINER_STOPPED:
        return "stopped";
    case CONTAINER_KILLED:
        return "killed";
    case CONTAINER_EXITED:
        return "exited";
    default:
        return "unknown";
    }
}

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;

    memset(buffer, 0, sizeof(*buffer));

    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0)
        return rc;

    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

/*
 * TODO:
 * Implement producer-side insertion into the bounded buffer.
 *
 * Requirements:
 *   - block or fail according to your chosen policy when the buffer is full
 *   - wake consumers correctly
 *   - stop cleanly if shutdown begins
 */
int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    (void)buffer;
    (void)item;
    return -1;
}

/*
 * TODO:
 * Implement consumer-side removal from the bounded buffer.
 *
 * Requirements:
 *   - wait correctly while the buffer is empty
 *   - return a useful status when shutdown is in progress
 *   - avoid races with producers and shutdown
 */
int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    (void)buffer;
    (void)item;
    return -1;
}

/*
 * TODO:
 * Implement the logging consumer thread.
 *
 * Suggested responsibilities:
 *   - remove log chunks from the bounded buffer
 *   - route each chunk to the correct per-container log file
 *   - exit cleanly when shutdown begins and pending work is drained
 */
void *logging_thread(void *arg)
{
    (void)arg;
    return NULL;
}

/*
 * TODO:
 * Implement the clone child entrypoint.
 *
 * Required outcomes:
 *   - isolated PID / UTS / mount context
 *   - chroot or pivot_root into rootfs
 *   - working /proc inside container
 *   - stdout / stderr redirected to the supervisor logging path
 *   - configured command executed inside the container
 */
int child_fn(void *arg)
{
    child_config_t *cfg = arg;

    if (cfg->nice_value != 0 && setpriority(PRIO_PROCESS, 0, cfg->nice_value) < 0)
        perror("setpriority");

    if (sethostname(cfg->id, strnlen(cfg->id, sizeof(cfg->id))) < 0)
        perror("sethostname");

    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0)
        perror("mount-private-root");

    if (chdir(cfg->rootfs) < 0) {
        perror("chdir(rootfs)");
        return 1;
    }

    if (chroot(".") < 0) {
        perror("chroot");
        return 1;
    }

    if (chdir("/") < 0) {
        perror("chdir(/)");
        return 1;
    }

    if (mkdir("/proc", 0555) < 0 && errno != EEXIST) {
        perror("mkdir(/proc)");
        return 1;
    }

    if (mount("proc", "/proc", "proc", 0, NULL) < 0) {
        perror("mount(/proc)");
        return 1;
    }

    if (dup2(cfg->log_write_fd, STDOUT_FILENO) < 0 ||
        dup2(cfg->log_write_fd, STDERR_FILENO) < 0) {
        perror("dup2(log fd)");
        return 1;
    }

    close(cfg->log_write_fd);

    execl("/bin/sh", "sh", "-c", cfg->command, (char *)NULL);
    perror("execl");
    return 127;
}

int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;

    return 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;

    return 0;
}

/*
 * TODO:
 * Implement the long-running supervisor process.
 *
 * Suggested responsibilities:
 *   - create and bind the control-plane IPC endpoint
 *   - initialize shared metadata and the bounded buffer
 *   - start the logging thread
 *   - accept control requests and update container state
 *   - reap children and respond to signals
 */
static int ensure_metadata_capacity(supervisor_ctx_t *ctx)
{
    size_t new_capacity;
    container_record_t *new_array;

    if (ctx->container_count < ctx->container_capacity)
        return 0;

    new_capacity = (ctx->container_capacity == 0) ? 16 : ctx->container_capacity * 2;
    new_array = realloc(ctx->containers, new_capacity * sizeof(*new_array));
    if (!new_array)
        return -1;

    memset(new_array + ctx->container_capacity,
           0,
           (new_capacity - ctx->container_capacity) * sizeof(*new_array));

    ctx->containers = new_array;
    ctx->container_capacity = new_capacity;
    return 0;
}

static container_record_t *find_container_by_id_locked(supervisor_ctx_t *ctx, const char *id)
{
    size_t i;

    for (i = 0; i < ctx->container_count; i++) {
        if (ctx->containers[i].in_use && strncmp(ctx->containers[i].id, id, CONTAINER_ID_LEN) == 0)
            return &ctx->containers[i];
    }

    return NULL;
}

static container_record_t *find_container_by_pid_locked(supervisor_ctx_t *ctx, pid_t pid)
{
    size_t i;

    for (i = 0; i < ctx->container_count; i++) {
        if (ctx->containers[i].in_use && ctx->containers[i].host_pid == pid)
            return &ctx->containers[i];
    }

    return NULL;
}

static int add_container_record(supervisor_ctx_t *ctx,
                                const char *id,
                                pid_t pid,
                                unsigned long soft_limit_bytes,
                                unsigned long hard_limit_bytes,
                                const char *log_path,
                                void *child_stack)
{
    container_record_t *record;

    if (ensure_metadata_capacity(ctx) != 0)
        return -1;

    record = &ctx->containers[ctx->container_count++];
    memset(record, 0, sizeof(*record));

    strncpy(record->id, id, sizeof(record->id) - 1);
    record->host_pid = pid;
    record->started_at = time(NULL);
    record->state = CONTAINER_RUNNING;
    record->soft_limit_bytes = soft_limit_bytes;
    record->hard_limit_bytes = hard_limit_bytes;
    record->exit_code = -1;
    record->exit_signal = 0;
    record->stop_requested = 0;
    record->child_stack = child_stack;
    record->in_use = 1;
    strncpy(record->log_path, log_path, sizeof(record->log_path) - 1);

    return 0;
}

static void reap_children(supervisor_ctx_t *ctx)
{
    int status;
    pid_t child;

    while ((child = waitpid(-1, &status, WNOHANG)) > 0) {
        container_record_t *record = NULL;

        pthread_mutex_lock(&ctx->metadata_lock);
        record = find_container_by_pid_locked(ctx, child);
        if (record) {
            if (WIFEXITED(status)) {
                record->state = CONTAINER_EXITED;
                record->exit_code = WEXITSTATUS(status);
                record->exit_signal = 0;
            } else if (WIFSIGNALED(status)) {
                record->exit_signal = WTERMSIG(status);
                record->exit_code = 128 + record->exit_signal;
                if (record->stop_requested)
                    record->state = CONTAINER_STOPPED;
                else
                    record->state = CONTAINER_KILLED;
            }
            free(record->child_stack);
            record->child_stack = NULL;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
    }
}

static int launch_container(supervisor_ctx_t *ctx,
                            const char *id,
                            const char *rootfs,
                            const char *command,
                            unsigned long soft_limit_bytes,
                            unsigned long hard_limit_bytes,
                            int nice_value)
{
    child_config_t *cfg;
    void *child_stack;
    int log_fd;
    char log_path[PATH_MAX];
    pid_t child_pid;

    if (mkdir(LOG_DIR, 0755) < 0 && errno != EEXIST) {
        perror("mkdir(logs)");
        return -1;
    }

    if (snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_DIR, id) >= (int)sizeof(log_path)) {
        fprintf(stderr, "Log path too long for container %s\n", id);
        return -1;
    }

    log_fd = open(log_path, O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (log_fd < 0) {
        perror("open(log)");
        return -1;
    }

    cfg = calloc(1, sizeof(*cfg));
    child_stack = malloc(STACK_SIZE);
    if (!cfg || !child_stack) {
        perror("alloc child config/stack");
        free(cfg);
        free(child_stack);
        close(log_fd);
        return -1;
    }

    strncpy(cfg->id, id, sizeof(cfg->id) - 1);
    strncpy(cfg->rootfs, rootfs, sizeof(cfg->rootfs) - 1);
    strncpy(cfg->command, command, sizeof(cfg->command) - 1);
    cfg->nice_value = nice_value;
    cfg->log_write_fd = log_fd;

    child_pid = clone(child_fn,
                      (char *)child_stack + STACK_SIZE,
                      CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                      cfg);
    if (child_pid < 0) {
        perror("clone");
        free(cfg);
        free(child_stack);
        close(log_fd);
        return -1;
    }

    close(log_fd);
    free(cfg);

    pthread_mutex_lock(&ctx->metadata_lock);
    if (find_container_by_id_locked(ctx, id) != NULL) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        kill(child_pid, SIGTERM);
        fprintf(stderr, "Container id already exists: %s\n", id);
        return -1;
    }

    if (add_container_record(ctx,
                             id,
                             child_pid,
                             soft_limit_bytes,
                             hard_limit_bytes,
                             log_path,
                             child_stack) != 0) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        kill(child_pid, SIGTERM);
        fprintf(stderr, "Failed to store metadata for %s\n", id);
        return -1;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    printf("Started container id=%s pid=%d rootfs=%s command=%s\n",
           id,
           child_pid,
           rootfs,
           command);
    return 0;
}

static void print_metadata(supervisor_ctx_t *ctx)
{
    size_t i;

    pthread_mutex_lock(&ctx->metadata_lock);
    printf("%-16s %-8s %-12s %-12s %-12s %-10s %-10s %-20s\n",
           "id",
           "pid",
           "state",
           "soft(MiB)",
           "hard(MiB)",
           "exit_code",
           "signal",
           "log_path");

    for (i = 0; i < ctx->container_count; i++) {
        const container_record_t *r = &ctx->containers[i];
        if (!r->in_use)
            continue;

        printf("%-16s %-8d %-12s %-12lu %-12lu %-10d %-10d %-20s\n",
               r->id,
               r->host_pid,
               state_to_string(r->state),
               r->soft_limit_bytes >> 20,
               r->hard_limit_bytes >> 20,
               r->exit_code,
               r->exit_signal,
               r->log_path);
    }
    pthread_mutex_unlock(&ctx->metadata_lock);
}

static void stop_container(supervisor_ctx_t *ctx, const char *id)
{
    container_record_t *record;

    pthread_mutex_lock(&ctx->metadata_lock);
    record = find_container_by_id_locked(ctx, id);
    if (!record) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        fprintf(stderr, "No such container: %s\n", id);
        return;
    }

    record->stop_requested = 1;
    if (record->state == CONTAINER_RUNNING || record->state == CONTAINER_STARTING)
        kill(record->host_pid, SIGTERM);
    pthread_mutex_unlock(&ctx->metadata_lock);
}

static void stop_all_running(supervisor_ctx_t *ctx)
{
    size_t i;

    pthread_mutex_lock(&ctx->metadata_lock);
    for (i = 0; i < ctx->container_count; i++) {
        container_record_t *record = &ctx->containers[i];
        if (!record->in_use)
            continue;
        if (record->state == CONTAINER_RUNNING || record->state == CONTAINER_STARTING) {
            record->stop_requested = 1;
            kill(record->host_pid, SIGTERM);
        }
    }
    pthread_mutex_unlock(&ctx->metadata_lock);
}

static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    int rc;
    struct sigaction sa;
    char line[1024];

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) {
        errno = rc;
        perror("pthread_mutex_init");
        return 1;
    }

    ctx.container_capacity = 16;
    ctx.containers = calloc(ctx.container_capacity, sizeof(*ctx.containers));
    if (!ctx.containers) {
        perror("calloc(containers)");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = supervisor_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;

    if (sigaction(SIGINT, &sa, NULL) < 0 ||
        sigaction(SIGTERM, &sa, NULL) < 0 ||
        sigaction(SIGCHLD, &sa, NULL) < 0) {
        perror("sigaction");
        free(ctx.containers);
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    printf("Supervisor started. Base rootfs template: %s\n", rootfs);
    printf("Commands:\n");
    printf("  start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n");
    printf("  ps\n");
    printf("  stop <id>\n");
    printf("  quit\n");

    while (!ctx.should_stop) {
        if (g_supervisor_stop)
            ctx.should_stop = 1;

        if (g_supervisor_sigchld) {
            g_supervisor_sigchld = 0;
            reap_children(&ctx);
        }

        if (ctx.should_stop)
            break;

        printf("supervisor> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin))
            break;

        if (line[0] == '\n' || line[0] == '\0')
            continue;

        line[strcspn(line, "\n")] = '\0';

        if (strncmp(line, "ps", 2) == 0 && (line[2] == '\0' || line[2] == ' ')) {
            print_metadata(&ctx);
            continue;
        }

        if (strncmp(line, "stop ", 5) == 0) {
            char *id = line + 5;
            while (*id == ' ' || *id == '\t')
                id++;
            if (*id == '\0') {
                fprintf(stderr, "Usage: stop <id>\n");
                continue;
            }
            stop_container(&ctx, id);
            continue;
        }

        if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0) {
            ctx.should_stop = 1;
            break;
        }

        if (strncmp(line, "start ", 6) == 0) {
            char *argv_local[32];
            int argc_local = 0;
            char *saveptr = NULL;
            char *tok = strtok_r(line, " \t", &saveptr);
            control_request_t req;

            while (tok && argc_local < (int)(sizeof(argv_local) / sizeof(argv_local[0]))) {
                argv_local[argc_local++] = tok;
                tok = strtok_r(NULL, " \t", &saveptr);
            }

            if (argc_local < 4) {
                fprintf(stderr,
                        "Usage: start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n");
                continue;
            }

            memset(&req, 0, sizeof(req));
            req.kind = CMD_START;
            strncpy(req.container_id, argv_local[1], sizeof(req.container_id) - 1);
            strncpy(req.rootfs, argv_local[2], sizeof(req.rootfs) - 1);
            strncpy(req.command, argv_local[3], sizeof(req.command) - 1);
            req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
            req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

            if (parse_optional_flags(&req, argc_local, argv_local, 4) != 0)
                continue;

            if (launch_container(&ctx,
                                 req.container_id,
                                 req.rootfs,
                                 req.command,
                                 req.soft_limit_bytes,
                                 req.hard_limit_bytes,
                                 req.nice_value) != 0) {
                fprintf(stderr, "Failed to start container %s\n", req.container_id);
            }
            continue;
        }

        fprintf(stderr, "Unknown command: %s\n", line);
    }

    stop_all_running(&ctx);
    while (waitpid(-1, NULL, 0) > 0)
        ;

    pthread_mutex_lock(&ctx.metadata_lock);
    for (size_t i = 0; i < ctx.container_count; i++) {
        free(ctx.containers[i].child_stack);
        ctx.containers[i].child_stack = NULL;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    free(ctx.containers);
    pthread_mutex_destroy(&ctx.metadata_lock);
    return 0;
}

/*
 * TODO:
 * Implement the client-side control request path.
 *
 * The CLI commands should use a second IPC mechanism distinct from the
 * logging pipe. A UNIX domain socket is the most direct option, but a
 * FIFO or shared memory design is also acceptable if justified.
 */
static int send_control_request(const control_request_t *req)
{
    (void)req;
    fprintf(stderr, "Control-plane client path not implemented.\n");
    return 1;
}

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;

    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;

    /*
     * TODO:
     * The supervisor should respond with container metadata.
     * Keep the rendering format simple enough for demos and debugging.
     */
    printf("Expected states include: %s, %s, %s, %s, %s\n",
           state_to_string(CONTAINER_STARTING),
           state_to_string(CONTAINER_RUNNING),
           state_to_string(CONTAINER_STOPPED),
           state_to_string(CONTAINER_KILLED),
           state_to_string(CONTAINER_EXITED));
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0)
        return cmd_start(argc, argv);

    if (strcmp(argv[1], "run") == 0)
        return cmd_run(argc, argv);

    if (strcmp(argv[1], "ps") == 0)
        return cmd_ps();

    if (strcmp(argv[1], "logs") == 0)
        return cmd_logs(argc, argv);

    if (strcmp(argv[1], "stop") == 0)
        return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
