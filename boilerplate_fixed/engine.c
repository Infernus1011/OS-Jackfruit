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
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>

static int pivot_root(const char *new_root, const char *put_old) {
    return syscall(SYS_pivot_root, new_root, put_old);
}

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
    CONTAINER_EXITED,
    CONTAINER_HARD_LIMIT_KILLED
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
    int stop_requested;
    int pipe_stdout;
    pthread_t producer_thread;
    char log_path[PATH_MAX];
    char *stack;
    struct container_record *next;
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
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int pipe_stdout;
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    volatile int should_stop;
    volatile sig_atomic_t got_sigchld;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

static supervisor_ctx_t *g_ctx = NULL;

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
    case CONTAINER_HARD_LIMIT_KILLED:
        return "hard_limit_killed";
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

int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count >= LOG_BUFFER_CAPACITY && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);
    }

    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return 1;
    }

    memcpy(&buffer->items[buffer->tail], item, sizeof(log_item_t));
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);

    return 0;
}

int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == 0 && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);
    }

    if (buffer->count == 0) {
        pthread_mutex_unlock(&buffer->mutex);
        return 1;
    }

    memcpy(item, &buffer->items[buffer->head], sizeof(log_item_t));
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);

    return 0;
}

void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    bounded_buffer_t *buffer = &ctx->log_buffer;
    log_item_t item;

    while (1) {
        if (bounded_buffer_pop(buffer, &item) != 0) {
            break;
        }

        if (item.length == 0) {
            break;
        }

        char log_path[PATH_MAX];
        snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_DIR, item.container_id);

        int fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            write(fd, item.data, item.length);
            close(fd);
        }
    }

    return NULL;
}

static void *container_producer(void *arg)
{
    container_record_t *rec = (container_record_t *)arg;
    bounded_buffer_t *buffer = &g_ctx->log_buffer;
    log_item_t item;
    char buf[LOG_CHUNK_SIZE];
    ssize_t n;

    strncpy(item.container_id, rec->id, sizeof(item.container_id) - 1);

    while ((n = read(rec->pipe_stdout, buf, sizeof(buf))) > 0) {
        item.length = (size_t)n;
        memcpy(item.data, buf, n);
        if (bounded_buffer_push(buffer, &item) != 0)
            break;
    }

    return NULL;
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

static int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    /* Make all mount points private to this new mount namespace so host isn't affected */
    if (mount(NULL, "/", NULL, MS_PRIVATE | MS_REC, NULL) < 0) {
        perror("mount private");
        return 1;
    }

    /* Bind mount the new rootfs to itself so it's a mount point for pivot_root */
    if (mount(cfg->rootfs, cfg->rootfs, "bind", MS_BIND | MS_REC, NULL) < 0) {
        perror("mount bind rootfs");
        return 1;
    }

    if (chdir(cfg->rootfs) < 0) {
        perror("chdir rootfs");
        return 1;
    }

    /* Create temporary hold directory for old root */
    mkdir("put_old", 0777);

    if (pivot_root(".", "put_old") < 0) {
        perror("pivot_root");
        return 1;
    }

    if (chdir("/") < 0) {
        perror("chdir /");
        return 1;
    }

    /* Detach and remove old root */
    if (umount2("/put_old", MNT_DETACH) < 0) {
        perror("umount2 put_old");
        return 1;
    }
    rmdir("/put_old");

    mount("proc", "/proc", "proc", 0, NULL);

    dup2(cfg->pipe_stdout, STDOUT_FILENO);
    dup2(cfg->pipe_stdout, STDERR_FILENO);
    close(cfg->pipe_stdout);

    if (cfg->nice_value != 0)
        setpriority(PRIO_PROCESS, 0, cfg->nice_value);

    execl("/bin/sh", "/bin/sh", "-c", cfg->command, NULL);
    execl(cfg->command, cfg->command, NULL);

    return 1;
}

static container_record_t *find_container(supervisor_ctx_t *ctx, const char *id)
{
    container_record_t *rec = ctx->containers;
    while (rec) {
        if (strcmp(rec->id, id) == 0)
            return rec;
        rec = rec->next;
    }
    return NULL;
}

static int launch_container(supervisor_ctx_t *ctx, control_request_t *req)
{
    int pipe_stdout[2];
    pid_t pid;
    child_config_t cfg;
    container_record_t *rec;
    char log_path[PATH_MAX];
    char *stack;

    if (pipe(pipe_stdout) < 0)
        return -1;

    snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_DIR, req->container_id);

    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.id, req->container_id, sizeof(cfg.id) - 1);
    strncpy(cfg.rootfs, req->rootfs, sizeof(cfg.rootfs) - 1);
    strncpy(cfg.command, req->command, sizeof(cfg.command) - 1);
    cfg.nice_value = req->nice_value;
    cfg.pipe_stdout = pipe_stdout[1];

    stack = malloc(STACK_SIZE);
    if (!stack) {
        close(pipe_stdout[0]);
        close(pipe_stdout[1]);
        return -1;
    }

    pid = clone(child_fn, stack + STACK_SIZE,
                CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD, &cfg);

    if (pid < 0) {
        free(stack);
        close(pipe_stdout[0]);
        close(pipe_stdout[1]);
        return -1;
    }

    close(pipe_stdout[1]);

    rec = malloc(sizeof(container_record_t));
    memset(rec, 0, sizeof(*rec));
    strncpy(rec->id, req->container_id, sizeof(rec->id) - 1);
    rec->host_pid = pid;
    rec->started_at = time(NULL);
    rec->state = CONTAINER_RUNNING;
    rec->soft_limit_bytes = req->soft_limit_bytes;
    rec->hard_limit_bytes = req->hard_limit_bytes;
    rec->pipe_stdout = pipe_stdout[0];
    rec->stack = stack;
    strncpy(rec->log_path, log_path, sizeof(rec->log_path) - 1);

    pthread_mutex_lock(&ctx->metadata_lock);
    rec->next = ctx->containers;
    ctx->containers = rec;
    pthread_mutex_unlock(&ctx->metadata_lock);

    register_with_monitor(ctx->monitor_fd, req->container_id,
                          pid, req->soft_limit_bytes, req->hard_limit_bytes);

    pthread_create(&rec->producer_thread, NULL, container_producer, rec);

    return 0;
}

static int stop_container(supervisor_ctx_t *ctx, const char *id)
{
    container_record_t *rec = find_container(ctx, id);
    if (!rec)
        return -1;

    rec->stop_requested = 1;
    kill(rec->host_pid, SIGTERM);

    return 0;
}

static void reap_children(supervisor_ctx_t *ctx)
{
    pid_t pid;
    int status;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&ctx->metadata_lock);

        container_record_t *rec = ctx->containers;
        while (rec) {
            if (rec->host_pid == pid) {
                if (rec->stop_requested) {
                    rec->state = CONTAINER_STOPPED;
                } else if (WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL) {
                    rec->state = CONTAINER_HARD_LIMIT_KILLED;
                } else {
                    rec->state = CONTAINER_EXITED;
                }
                rec->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                rec->exit_signal = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
                break;
            }
            rec = rec->next;
        }

        pthread_mutex_unlock(&ctx->metadata_lock);

        if (rec) {
            unregister_from_monitor(ctx->monitor_fd, rec->id, pid);
            pthread_join(rec->producer_thread, NULL);
            close(rec->pipe_stdout);
            rec->pipe_stdout = -1;
            free(rec->stack);
            rec->stack = NULL;
        }
    }
}

static void sigchld_handler(int sig)
{
    (void)sig;
    if (g_ctx)
        g_ctx->got_sigchld = 1;
}

static void sigterm_handler(int sig)
{
    (void)sig;
    if (g_ctx)
        g_ctx->should_stop = 1;
}

static int create_control_socket(const char *path)
{
    int fd;
    struct sockaddr_un addr;

    unlink(path);

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(fd, 10) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static void handle_client(supervisor_ctx_t *ctx, int client_fd)
{
    char buffer[4096];
    ssize_t n = read(client_fd, buffer, sizeof(buffer) - 1);

    if (n <= 0) {
        close(client_fd);
        return;
    }
    buffer[n] = '\0';

    char cmd[64], id[64], rootfs[256], command[256];
    unsigned long soft = DEFAULT_SOFT_LIMIT, hard = DEFAULT_HARD_LIMIT;
    int nice = 0;

    sscanf(buffer, "%63s", cmd);

    if (strcmp(cmd, "start") == 0 || strcmp(cmd, "run") == 0) {
        unsigned long soft_mib = 0, hard_mib = 0;
        sscanf(buffer, "%*s %63s %lu %lu %d %255s %255[^\n]", 
               id, &soft_mib, &hard_mib, &nice, rootfs, command);
               
        if (soft_mib == 0) soft_mib = DEFAULT_SOFT_LIMIT / (1UL << 20);
        if (hard_mib == 0) hard_mib = DEFAULT_HARD_LIMIT / (1UL << 20);

        control_request_t req = {
            .kind = (strcmp(cmd, "run") == 0) ? CMD_RUN : CMD_START,
        };
        strncpy(req.container_id, id, sizeof(req.container_id) - 1);
        strncpy(req.rootfs, rootfs, sizeof(req.rootfs) - 1);
        strncpy(req.command, command, sizeof(req.command) - 1);
        req.soft_limit_bytes = soft_mib * (1UL << 20);
        req.hard_limit_bytes = hard_mib * (1UL << 20);
        req.nice_value = nice;

        if (find_container(ctx, req.container_id)) {
            write(client_fd, "ERROR: Container already exists\n", 31);
        } else if (launch_container(ctx, &req) == 0) {
            write(client_fd, "OK\n", 3);
        } else {
            write(client_fd, "ERROR: Failed to launch\n", 24);
        }
    } else if (strcmp(cmd, "ps") == 0) {
        pthread_mutex_lock(&ctx->metadata_lock);
        char response[8192] = "";
        char line[256];

        container_record_t *rec = ctx->containers;
        if (!rec) {
            strcpy(response, "No containers running\n");
        }
        while (rec) {
            snprintf(line, sizeof(line), "%-10s pid=%-6d state=%-15s started=%ld\n",
                     rec->id, rec->host_pid,
                     state_to_string(rec->state),
                     (long)rec->started_at);
            strcat(response, line);
            rec = rec->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        write(client_fd, response, strlen(response));

    } else if (strcmp(cmd, "stop") == 0) {
        sscanf(buffer, "%*s %s", id);
        if (stop_container(ctx, id) == 0) {
            write(client_fd, "STOPPED\n", 8);
        } else {
            write(client_fd, "ERROR: Container not found\n", 25);
        }

    } else if (strcmp(cmd, "logs") == 0) {
        sscanf(buffer, "%*s %s", id);
        container_record_t *rec = find_container(ctx, id);
        if (rec) {
            char log_path[PATH_MAX];
            snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_DIR, id);

            int fd = open(log_path, O_RDONLY);
            if (fd >= 0) {
                char buf[4096];
                ssize_t r;
                while ((r = read(fd, buf, sizeof(buf))) > 0) {
                    write(client_fd, buf, r);
                }
                close(fd);
            } else {
                write(client_fd, "No logs available\n", 18);
            }
        } else {
            write(client_fd, "ERROR: Container not found\n", 25);
        }

    } else {
        write(client_fd, "ERROR: Unknown command\n", 22);
    }

    close(client_fd);
}

static int supervisor_event_loop(supervisor_ctx_t *ctx)
{
    fd_set read_fds;
    int max_fd = ctx->server_fd;

    while (!ctx->should_stop) {
        if (ctx->got_sigchld) {
            ctx->got_sigchld = 0;
            reap_children(ctx);
        }

        FD_ZERO(&read_fds);
        FD_SET(ctx->server_fd, &read_fds);

        struct timeval tv = {1, 0};
        int ready = select(max_fd + 1, &read_fds, NULL, NULL, &tv);

        if (ready < 0)
            continue;

        if (ready > 0 && FD_ISSET(ctx->server_fd, &read_fds)) {
            int client_fd = accept(ctx->server_fd, NULL, NULL);
            if (client_fd >= 0) {
                handle_client(ctx, client_fd);
            }
        }
    }

    return 0;
}

static void cleanup_all(supervisor_ctx_t *ctx)
{
    pthread_mutex_lock(&ctx->metadata_lock);

    container_record_t *rec = ctx->containers;
    while (rec) {
        if (rec->state == CONTAINER_RUNNING) {
            rec->stop_requested = 1;
            kill(rec->host_pid, SIGTERM);
        }
        rec = rec->next;
    }

    pthread_mutex_unlock(&ctx->metadata_lock);

    sleep(1);

    reap_children(ctx);

    pthread_mutex_lock(&ctx->metadata_lock);
    while (ctx->containers) {
        container_record_t *rec = ctx->containers;
        ctx->containers = rec->next;
        if (rec->pipe_stdout >= 0)
            close(rec->pipe_stdout);
        if (rec->stack)
            free(rec->stack);
        free(rec);
    }
    pthread_mutex_unlock(&ctx->metadata_lock);
}

static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    int rc;

    (void)rootfs;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;

    g_ctx = &ctx;

    mkdir(LOG_DIR, 0755);

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) {
        errno = rc;
        perror("pthread_mutex_init");
        return 1;
    }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) {
        errno = rc;
        perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0) {
        perror("open /dev/container_monitor");
        bounded_buffer_destroy(&ctx.log_buffer);
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    ctx.server_fd = create_control_socket(CONTROL_PATH);
    if (ctx.server_fd < 0) {
        perror("create_control_socket");
        close(ctx.monitor_fd);
        bounded_buffer_destroy(&ctx.log_buffer);
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGCHLD, &sa, NULL);

    sa.sa_handler = sigterm_handler;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    printf("Supervisor running. PID=%d\n", getpid());
    fflush(stdout);

    supervisor_event_loop(&ctx);

    printf("Shutting down supervisor...\n");
    fflush(stdout);

    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);

    cleanup_all(&ctx);

    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);

    close(ctx.monitor_fd);
    close(ctx.server_fd);
    unlink(CONTROL_PATH);

    printf("Supervisor exited cleanly.\n");
    fflush(stdout);

    return 0;
}

static int send_control_request(const control_request_t *req)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;

    if (fd < 0)
        return 1;

    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        fprintf(stderr, "Error: Supervisor not running. Start with 'engine supervisor'\n");
        return 1;
    }

    char buffer[4096];
    int len;

    if (req->kind == CMD_START || req->kind == CMD_RUN) {
        len = snprintf(buffer, sizeof(buffer),
                      "%s %s %lu %lu %d %s %s\n",
                      req->kind == CMD_START ? "start" : "run",
                      req->container_id,
                      req->soft_limit_bytes / (1UL << 20),
                      req->hard_limit_bytes / (1UL << 20),
                      req->nice_value,
                      req->rootfs,
                      req->command);
    } else if (req->kind == CMD_PS) {
        strcpy(buffer, "ps\n");
        len = 3;
    } else if (req->kind == CMD_LOGS) {
        len = snprintf(buffer, sizeof(buffer), "logs %s\n", req->container_id);
    } else if (req->kind == CMD_STOP) {
        len = snprintf(buffer, sizeof(buffer), "stop %s\n", req->container_id);
    } else {
        close(fd);
        return 1;
    }

    write(fd, buffer, len);

    char response[8192];
    ssize_t n = read(fd, response, sizeof(response) - 1);
    if (n > 0) {
        response[n] = '\0';
        printf("%s", response);
    }

    close(fd);
    return 0;
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
    req.kind = CMD_RUN; /* Passes through dynamically to the supervisor parser! */
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    /* Issue the launch command */
    send_control_request(&req);

    /* Poll loop to monitor the container state */
    while (1) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un addr;
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            close(fd);
            break;
        }

        write(fd, "ps\n", 3);
        char response[8192];
        ssize_t n = read(fd, response, sizeof(response) - 1);
        close(fd);

        if (n > 0) {
            response[n] = '\0';
            char search_token[128];
            snprintf(search_token, sizeof(search_token), "%-10s pid=", req.container_id);
            char *pos = strstr(response, search_token);
            
            if (!pos) break; /* Container missing means it was purged */
            
            if (strstr(pos, "exited") || strstr(pos, "killed") || strstr(pos, "stopped")) {
                break;
            }
        } else {
            break;
        }
        sleep(1);
    }

    /* Print out the logs once it finishes */
    req.kind = CMD_LOGS;
    send_control_request(&req);

    return 0;
}

static int cmd_ps(void)
{
    control_request_t req;

    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;

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
