# OS-Jackfruit Implementation Guide

## Phase 1: Project Setup

### 1.1 Environment Setup
```bash
# On your Ubuntu 22.04/24.04 VM
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)

# Clone your forked repository
git clone https://github.com/<your-username>/OS-Jackfruit.git
cd OS-Jackfruit/boilerplate

# Run environment check
chmod +x environment-check.sh
sudo ./environment-check.sh
```

### 1.2 Prepare Root Filesystem
```bash
mkdir -p rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

# Create per-container copies
cp -a rootfs-base rootfs-alpha
cp -a rootfs-base rootfs-beta
```

### 1.3 Verify Build
```bash
make
# Should produce: engine, memory_hog, cpu_hog, io_pulse, monitor.ko
```

---

## Phase 2: Kernel Module Implementation (monitor.c)

### TODO 1: Define Linked-List Node Structure
```c
/* In monitor.c, replace the TODO 1 section */

struct monitored_entry {
    pid_t pid;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    char container_id[MONITOR_NAME_LEN];
    int soft_limit_warned;       /* Track if soft limit warning was emitted */
    struct list_head list;
};
```

### TODO 2: Declare Global List and Lock
```c
/* In monitor.c, replace the TODO 2 section */

static LIST_HEAD(monitored_list);
static DEFINE_MUTEX(list_mutex);
```

**Why mutex?** Because the timer callback and ioctl handlers run in process context (not interrupt context), so mutex is appropriate. Spinlocks would waste CPU spinning.

### TODO 3: Timer Callback - Periodic Monitoring
```c
static void timer_callback(struct timer_list *t)
{
    struct monitored_entry *entry, *tmp;
    long rss;

    mutex_lock(&list_mutex);

    list_for_each_entry_safe(entry, tmp, &monitored_list, list) {
        rss = get_rss_bytes(entry->pid);

        /* Process exited - remove entry */
        if (rss < 0) {
            printk(KERN_INFO "[container_monitor] Process %d exited, removing\n", entry->pid);
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        /* Check hard limit - kill and remove */
        if (rss > (long)entry->hard_limit_bytes) {
            printk(KERN_WARNING "[container_monitor] HARD LIMIT exceeded\n");
            kill_process(entry->container_id, entry->pid,
                         entry->hard_limit_bytes, rss);
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        /* Check soft limit - warn once */
        if (!entry->soft_limit_warned && rss > (long)entry->soft_limit_bytes) {
            log_soft_limit_event(entry->container_id, entry->pid,
                                 entry->soft_limit_bytes, rss);
            entry->soft_limit_warned = 1;
        }
    }

    mutex_unlock(&list_mutex);

    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);
}
```

### TODO 4: Register a PID (MONITOR_REGISTER ioctl)
```c
/* In monitor_ioctl, replace MONITOR_REGISTER section */

if (cmd == MONITOR_REGISTER) {
    struct monitored_entry *new_entry;

    new_entry = kmalloc(sizeof(*new_entry), GFP_KERNEL);
    if (!new_entry)
        return -ENOMEM;

    new_entry->pid = req.pid;
    new_entry->soft_limit_bytes = req.soft_limit_bytes;
    new_entry->hard_limit_bytes = req.hard_limit_bytes;
    new_entry->soft_limit_warned = 0;
    strncpy(new_entry->container_id, req.container_id, MONITOR_NAME_LEN - 1);
    new_entry->container_id[MONITOR_NAME_LEN - 1] = '\0';

    mutex_lock(&list_mutex);
    list_add_tail(&new_entry->list, &monitored_list);
    mutex_unlock(&list_mutex);

    printk(KERN_INFO "[container_monitor] Registered container=%s pid=%d\n",
           req.container_id, req.pid);
    return 0;
}
```

### TODO 5: Unregister a PID (MONITOR_UNREGISTER ioctl)
```c
/* In monitor_ioctl, replace MONITOR_UNREGISTER section */

if (cmd == MONITOR_UNREGISTER) {
    struct monitored_entry *entry, *tmp;
    int found = 0;

    mutex_lock(&list_mutex);
    list_for_each_entry_safe(entry, tmp, &monitored_list, list) {
        if (entry->pid == req.pid) {
            list_del(&entry->list);
            kfree(entry);
            found = 1;
            break;
        }
    }
    mutex_unlock(&list_mutex);

    if (found)
        return 0;
    return -ENOENT;
}
```

### TODO 6: Module Exit - Free All Entries
```c
/* In monitor_exit, replace the TODO 6 section */

static void __exit monitor_exit(void)
{
    struct monitored_entry *entry, *tmp;

    del_timer_sync(&monitor_timer);

    mutex_lock(&list_mutex);
    list_for_each_entry_safe(entry, tmp, &monitored_list, list) {
        list_del(&entry->list);
        kfree(entry);
    }
    mutex_unlock(&list_mutex);

    cdev_del(&c_dev);
    device_destroy(cl, dev_num);
    class_destroy(cl);
    unregister_chrdev_region(dev_num, 1);

    printk(KERN_INFO "[container_monitor] Module unloaded.\n");
}
```

### Rebuild Module
```bash
make monitor.ko
sudo insmod monitor.ko
ls -l /dev/container_monitor
dmesg | tail
```

---

## Phase 3: User-Space Runtime (engine.c)

### Task 1: Multi-Container Runtime with Parent Supervisor

#### Step 3.1: Create Log Directory
```c
/* Add to run_supervisor() */
mkdir(LOG_DIR, 0755);
```

#### Step 3.2: Implement Child Function (Container Entry Point)
```c
static int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;
    char *stack;
    char *child_stack;

    /* Allocate stack for child */
    stack = malloc(STACK_SIZE);
    if (!stack)
        return 1;
    child_stack = stack + STACK_SIZE;

    /* Unshare namespaces before exec */
    if (unshare(CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS) < 0) {
        perror("unshare");
        return 1;
    }

    /* Chroot into container rootfs */
    if (chroot(cfg->rootfs) < 0 || chdir("/") < 0) {
        perror("chroot");
        return 1;
    }

    /* Mount /proc for ps to work */
    mount("proc", "/proc", "proc", 0, NULL);

    /* Redirect stdout/stderr to the pipe from parent */
    /* Note: fd 1 and 2 are already set up by parent */

    /* Set nice value if requested */
    if (cfg->nice_value != 0)
        setpriority(PRIO_PROCESS, 0, cfg->nice_value);

    /* Parse and execute command */
    execl("/bin/sh", "/bin/sh", "-c", cfg->command, NULL);
    execl(cfg->command, cfg->command, NULL);

    /* If exec fails */
    return 1;
}
```

#### Step 3.3: Launch Container with Clone
```c
static int launch_container(supervisor_ctx_t *ctx, control_request_t *req)
{
    int pipe_stdout[2], pipe_stderr[2];
    pid_t pid;
    child_config_t cfg;
    container_record_t *rec;
    char log_path[PATH_MAX];

    /* Create pipes for stdout and stderr */
    if (pipe(pipe_stdout) < 0 || pipe(pipe_stderr) < 0)
        return -1;

    /* Setup log file path */
    snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_DIR, req->container_id);

    /* Prepare child config */
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.id, req->container_id, sizeof(cfg.id) - 1);
    strncpy(cfg.rootfs, req->rootfs, sizeof(cfg.rootfs) - 1);
    strncpy(cfg.command, req->command, sizeof(cfg.command) - 1);
    cfg.nice_value = req->nice_value;
    cfg.log_write_fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (cfg.log_write_fd < 0)
        return -1;

    /* Clone the child process */
    pid = clone(child_fn, child_stack + STACK_SIZE,
                CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD, &cfg);

    if (pid < 0)
        return -1;

    /* Parent: close child-side pipes, setup logging */

    /* Create container record */
    rec = malloc(sizeof(container_record_t));
    memset(rec, 0, sizeof(*rec));
    strncpy(rec->id, req->container_id, sizeof(rec->id) - 1);
    rec->host_pid = pid;
    rec->started_at = time(NULL);
    rec->state = CONTAINER_RUNNING;
    rec->soft_limit_bytes = req->soft_limit_bytes;
    rec->hard_limit_bytes = req->hard_limit_bytes;
    strncpy(rec->log_path, log_path, sizeof(rec->log_path) - 1);

    /* Add to list with mutex protection */
    pthread_mutex_lock(&ctx->metadata_lock);
    rec->next = ctx->containers;
    ctx->containers = rec;
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* Register with kernel monitor */
    register_with_monitor(ctx->monitor_fd, req->container_id,
                          pid, req->soft_limit_bytes, req->hard_limit_bytes);

    return 0;
}
```

### Task 2: Supervisor CLI and Signal Handling

#### Step 3.4: Implement Control Socket IPC
```c
/* Create UNIX domain socket for control channel */
static int create_control_socket(const char *path)
{
    int fd;
    struct sockaddr_un addr;

    unlink(path);  /* Remove existing socket */

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

/* Supervisor event loop */
static int supervisor_event_loop(supervisor_ctx_t *ctx)
{
    fd_set read_fds;
    int max_fd = ctx->server_fd;

    while (!ctx->should_stop) {
        FD_ZERO(&read_fds);
        FD_SET(ctx->server_fd, &read_fds);

        struct timeval tv = {1, 0};  /* 1 second timeout */
        int ready = select(max_fd + 1, &read_fds, NULL, NULL, &tv);

        if (ready < 0)
            continue;

        if (FD_ISSET(ctx->server_fd, &read_fds)) {
            int client_fd = accept(ctx->server_fd, NULL, NULL);
            if (client_fd >= 0) {
                handle_client_request(ctx, client_fd);
                close(client_fd);
            }
        }
    }

    return 0;
}

/* Handle individual client requests */
static void handle_client_request(supervisor_ctx_t *ctx, int client_fd)
{
    char buffer[1024];
    ssize_t n = read(client_fd, buffer, sizeof(buffer) - 1);

    if (n <= 0)
        return;

    buffer[n] = '\0';

    /* Parse command type (first word) */
    char cmd[64];
    sscanf(buffer, "%s", cmd);

    if (strcmp(cmd, "start") == 0) {
        /* Parse start command */
        char id[32], rootfs[256], command[256];
        unsigned long soft = DEFAULT_SOFT_LIMIT, hard = DEFAULT_HARD_LIMIT;
        int nice = 0;

        sscanf(buffer, "%*s %s %s %[^\n]", id, rootfs, command);
        /* Parse optional flags... */

        control_request_t req = {
            .kind = CMD_START,
        };
        strncpy(req.container_id, id, sizeof(req.container_id) - 1);
        strncpy(req.rootfs, rootfs, sizeof(req.rootfs) - 1);
        strncpy(req.command, command, sizeof(req.command) - 1);
        req.soft_limit_bytes = soft;
        req.hard_limit_bytes = hard;
        req.nice_value = nice;

        launch_container(ctx, &req);
        write(client_fd, "OK\n", 3);

    } else if (strcmp(cmd, "ps") == 0) {
        /* List all containers */
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *rec = ctx->containers;
        char response[4096] = "";
        char line[256];

        while (rec) {
            snprintf(line, sizeof(line), "%s pid=%d state=%s started=%ld\n",
                     rec->id, rec->host_pid,
                     state_to_string(rec->state),
                     (long)rec->started_at);
            strcat(response, line);
            rec = rec->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        write(client_fd, response, strlen(response));

    } else if (strcmp(cmd, "stop") == 0) {
        char id[32];
        sscanf(buffer, "%*s %s", id);
        stop_container(ctx, id);
        write(client_fd, "STOPPED\n", 8);

    } else if (strcmp(cmd, "logs") == 0) {
        char id[32];
        sscanf(buffer, "%*s %s", id);
        /* Read and send log file contents */
        send_log_file(client_fd, id);
    }
}

/* Client-side: send request to supervisor */
static int send_control_request(const control_request_t *req)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return 1;

    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        fprintf(stderr, "Error: Supervisor not running. Start with 'engine supervisor'\n");
        return 1;
    }

    /* Format command based on req->kind */
    char buffer[1024];
    int len;

    if (req->kind == CMD_START || req->kind == CMD_RUN) {
        len = snprintf(buffer, sizeof(buffer),
                      "%s %s %s %s --soft-mib %lu --hard-mib %lu --nice %d\n",
                      req->kind == CMD_START ? "start" : "run",
                      req->container_id,
                      req->rootfs,
                      req->command,
                      req->soft_limit_bytes / (1UL << 20),
                      req->hard_limit_bytes / (1UL << 20),
                      req->nice_value);
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

    /* Read response */
    char response[4096];
    ssize_t n = read(fd, response, sizeof(response) - 1);
    if (n > 0) {
        response[n] = '\0';
        printf("%s", response);
    }

    close(fd);
    return 0;
}
```

#### Step 3.5: Signal Handling
```c
/* Global flag for signal handling */
static volatile sig_atomic_t got_sigchld = 0;

static void sigchld_handler(int sig)
{
    (void)sig;
    got_sigchld = 1;
}

static void sigterm_handler(int sig)
{
    (void)sig;
    ctx->should_stop = 1;
}

/* In run_supervisor(), install signal handlers */
static int run_supervisor(const char *rootfs)
{
    /* ... setup code ... */

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    sa.sa_handler = sigterm_handler;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    /* ... rest of supervisor setup ... */
}

/* Reap dead children */
static void reap_children(supervisor_ctx_t *ctx)
{
    pid_t pid;
    int status;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&ctx->metadata_lock);

        container_record_t *rec = ctx->containers;
        while (rec) {
            if (rec->host_pid == pid) {
                rec->state = CONTAINER_EXITED;
                rec->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                rec->exit_signal = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
                break;
            }
            rec = rec->next;
        }

        pthread_mutex_unlock(&ctx->metadata_lock);

        /* Unregister from kernel monitor */
        unregister_from_monitor(ctx->monitor_fd, rec->id, pid);
    }
}
```

### Task 3: Bounded-Buffer Logging

#### Step 3.6: Implement Bounded Buffer
```c
int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    /* Wait while buffer is full (unless shutting down) */
    while (buffer->count >= LOG_BUFFER_CAPACITY && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);
    }

    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return 1;  /* Indicate shutdown */
    }

    /* Add item to buffer */
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

    /* Wait while buffer is empty (unless shutting down) */
    while (buffer->count == 0 && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);
    }

    if (buffer->count == 0) {
        pthread_mutex_unlock(&buffer->mutex);
        return 1;  /* Indicate shutdown or empty */
    }

    /* Remove item from buffer */
    memcpy(item, &buffer->items[buffer->head], sizeof(log_item_t));
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);

    return 0;
}
```

#### Step 3.7: Logging Thread
```c
void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    bounded_buffer_t *buffer = &ctx->log_buffer;
    log_item_t item;
    int fd = -1;

    while (1) {
        if (bounded_buffer_pop(buffer, &item) != 0) {
            /* Shutdown or error */
            break;
        }

        /* Open log file for this container */
        char log_path[PATH_MAX];
        snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_DIR, item.container_id);

        fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            write(fd, item.data, item.length);
            close(fd);
        }
    }

    return NULL;
}
```

#### Step 3.8: Producer Thread for Container Output
```c
static void *container_output_producer(void *arg)
{
    container_record_t *rec = (container_record_t *)arg;
    bounded_buffer_t *buffer = &global_ctx.log_buffer;
    log_item_t item;
    char buf[LOG_CHUNK_SIZE];
    ssize_t n;

    strncpy(item.container_id, rec->id, sizeof(item.container_id) - 1);

    /* Read from pipe (rec->pipe_fd) until EOF */
    while ((n = read(rec->pipe_fd, buf, sizeof(buf))) > 0) {
        item.length = (size_t)n;
        memcpy(item.data, buf, n);

        if (bounded_buffer_push(buffer, &item) != 0)
            break;  /* Shutdown */
    }

    /* Signal end of stream */
    item.length = 0;
    bounded_buffer_push(buffer, &item);

    return NULL;
}
```

---

## Phase 4: Full Supervisor Integration

### Complete run_supervisor Implementation
```c
static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    pthread_t logger_tid;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;

    pthread_mutex_init(&ctx.metadata_lock, NULL);
    bounded_buffer_init(&ctx.log_buffer);

    /* Open kernel monitor device */
    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0) {
        perror("open /dev/container_monitor");
        return 1;
    }

    /* Create control socket */
    ctx.server_fd = create_control_socket(CONTROL_PATH);
    if (ctx.server_fd < 0) {
        perror("create_control_socket");
        return 1;
    }

    /* Create log directory */
    mkdir(LOG_DIR, 0755);

    /* Start logging thread */
    pthread_create(&logger_tid, NULL, logging_thread, &ctx);

    /* Install signal handlers */
    struct sigaction sa = {.sa_handler = sigchld_handler};
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGCHLD, &sa, NULL);

    sa.sa_handler = sigterm_handler;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    printf("Supervisor running. PID=%d\n", getpid());

    /* Event loop */
    while (!ctx.should_stop) {
        if (got_sigchld) {
            got_sigchld = 0;
            reap_children(&ctx);
        }

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(ctx.server_fd, &fds);

        struct timeval tv = {1, 0};
        int ret = select(ctx.server_fd + 1, &fds, NULL, NULL, &tv);

        if (ret > 0 && FD_ISSET(ctx.server_fd, &fds)) {
            int client = accept(ctx.server_fd, NULL, NULL);
            if (client >= 0) {
                handle_client_request(&ctx, client);
                close(client);
            }
        }
    }

    /* Cleanup: stop all containers */
    cleanup_all_containers(&ctx);

    /* Shutdown logging */
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(logger_tid, NULL);

    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);

    close(ctx.monitor_fd);
    close(ctx.server_fd);
    unlink(CONTROL_PATH);

    printf("Supervisor exited cleanly.\n");
    return 0;
}
```

---

## Phase 5: Testing and Demo Commands

### Test Sequence
```bash
# Build
make clean && make

# Load module
sudo insmod monitor.ko
dmesg | tail

# Start supervisor (Terminal 1)
sudo ./engine supervisor ./rootfs-base

# Terminal 2: Create container rootfs copies
cp -a rootfs-base rootfs-alpha
cp -a rootfs-base rootfs-beta

# Start containers
sudo ./engine start alpha ./rootfs-alpha /bin/sh
sudo ./engine start beta ./rootfs-beta /bin/sh

# List containers
sudo ./engine ps

# View logs
sudo ./engine logs alpha

# Stop container
sudo ./engine stop alpha

# Cleanup
sudo rmmod monitor
```

---

## Phase 6: Engineering Analysis Points

### 1. Isolation Mechanisms
- **Namespaces (PID, UTS, Mount)**: `unshare()` creates isolated process trees, hostname/view, and filesystem view
- **chroot/pivot_root**: Restricts filesystem access to container rootfs
- **What kernel still shares**: CPU, memory, network, IPC namespaces by default

### 2. Supervisor and Process Lifecycle
- **Long-running parent**: Reaps zombies, manages metadata, handles signals
- **SIGCHLD handling**: Async notification when child exits
- **Metadata tracking**: Container state machine (starting → running → stopped/exited/killed)

### 3. IPC and Synchronization
| Shared Data | Race Condition | Solution |
|-------------|-----------------|----------|
| Container list | Concurrent add/remove | mutex |
| Log buffer | Multiple producers, one consumer | mutex + condition variables |
| Kernel module list | Timer vs ioctl | mutex |

### 4. Memory Management
- **RSS**: Physical memory pages resident in RAM
- **Soft limit**: Warning only, doesn't kill
- **Hard limit**: SIGKILL when exceeded
- **Kernel enforcement**: Required because user-space can't reliably check (TOCTOU race)

### 5. Scheduling Behavior
- **Nice values**: Lower nice = higher priority, affects CPU time slices
- **CPU-bound**: CPU-bound workloads compete for CPU cycles
- **I/O-bound**: Sleep frequently, yield CPU to others

---

## Phase 7: Common Issues and Debugging

### Issue: "Permission denied" or "Operation not permitted"
```bash
# Ensure running as root
sudo ./engine supervisor ./rootfs-base
```

### Issue: Module won't load (Secure Boot)
```bash
# Disable Secure Boot in UEFI settings, or sign the module
```

### Issue: Containers share the same rootfs
```bash
# MUST create separate copies before running
cp -a rootfs-base rootfs-alpha
cp -a rootfs-base rootfs-beta
```

### Debug dmesg
```bash
sudo dmesg -w | grep container_monitor
```

### Check for zombies
```bash
ps aux | grep defunct
```
