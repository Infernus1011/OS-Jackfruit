# Multi-Container Runtime

**Team:**
| Name | SRN |
|------|-----|
| Satvik Das | PES2UG24CS448 |
| Shashank Verma | PES2UG24CS462 |

**Course:** UE24CS242B – Operating Systems | PES University, Jan–May 2026
**Guide:** Prof. Gokulakrishnan S, Associate Professor, Dept. of CSE

---

## Overview

A lightweight, Linux-native **multi-container runtime** built from scratch in C. It demonstrates core OS concepts — process isolation via Linux namespaces, supervisor lifecycle management, IPC via UNIX domain sockets, bounded-buffer logging, memory enforcement via a Linux Kernel Module (LKM), and CPU scheduling experiments using CFS nice values.

---

## Build, Load, and Run Instructions

### Prerequisites
- Ubuntu 22.04 or 24.04 VM (WSL will **not** work)
- Secure Boot **OFF** (required for loading unsigned kernel modules)
- Root access (`sudo`)
- `gcc`, `make`, and kernel headers installed:
  ```bash
  sudo apt install build-essential linux-headers-$(uname -r)
  ```

### Build Everything
```bash
make clean
make
```
This builds:
- `engine` — user-space runtime + supervisor
- `monitor.ko` — kernel module (LKM)
- `cpu_hog`, `io_pulse`, `memory_hog` — test workload binaries

### Prepare Root Filesystems
```bash
mkdir -p rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base
cp -a rootfs-base rootfs-alpha
cp -a rootfs-base rootfs-beta
```

### Load Kernel Module
```bash
sudo insmod monitor.ko
ls -l /dev/container_monitor
```

### Start Supervisor
```bash
# Terminal 1
sudo ./engine supervisor ./rootfs-base
```

### CLI Commands (Terminal 2)
```bash
# Start containers (background)
sudo ./engine start alpha ./rootfs-alpha /bin/sh
sudo ./engine start beta  ./rootfs-beta  /bin/sh

# List all containers
sudo ./engine ps

# View logs for a container
sudo ./engine logs alpha

# Stop containers
sudo ./engine stop alpha
sudo ./engine stop beta

# Memory limit testing
cp memory_hog ./rootfs-alpha/
sudo ./engine start mem1 ./rootfs-alpha "/memory_hog 1 500" --soft-mib 40 --hard-mib 64
sleep 5
sudo dmesg | grep container_monitor

# Scheduling experiments (Task 5)
cp cpu_hog io_pulse ./rootfs-alpha/
sudo ./engine start cpu1 ./rootfs-alpha "/cpu_hog 10" --nice 10
sudo ./engine start cpu2 ./rootfs-alpha "/cpu_hog 10" --nice -10

# Blocking run (synchronous — waits for container to exit)
sudo ./engine run blocktest ./rootfs-alpha "/bin/sleep 3"
```

### Cleanup
```bash
# Stop supervisor: press Ctrl+C in Terminal 1

# Unload kernel module
sudo rmmod monitor

# Remove rootfs copies
rm -rf rootfs-alpha rootfs-beta rootfs-base
```

---

## Verification Commands

```bash
# Check for zombie processes after shutdown
ps aux | grep defunct

# Check kernel module is fully unloaded
lsmod | grep monitor

# Check kernel log for module unload message
dmesg | grep container_monitor | tail -5

# Confirm control socket was removed
ls -l /tmp/mini_runtime.sock
```

---

## Demo Screenshots

### 1. Multi-Container Supervision
*[Include screenshot showing two or more containers running under one supervisor — `engine ps` output]*

### 2. Metadata Tracking
*[Include screenshot of `ps` command showing tracked container metadata: name, PID, state, uptime]*

### 3. Bounded-Buffer Logging
*[Include screenshot of log file contents showing captured stdout from a container]*

### 4. CLI and IPC
*[Include screenshot of CLI command being issued and supervisor responding via UNIX socket]*

### 5. Soft-Limit Warning
*[Include screenshot of `dmesg` showing `SOFT LIMIT` warning event from the kernel module]*

### 6. Hard-Limit Enforcement
*[Include screenshot of `dmesg` showing `HARD LIMIT` enforcement and container marked as `hard_limit_killed`]*

### 7. Scheduling Experiment
*[Include screenshot of terminal output from scheduling experiment with `nice=-10` vs `nice=10`]*

### 8. Clean Teardown
*[Include screenshot showing no zombie processes and clean shutdown — no `<defunct>` entries in `ps`]*

---

## Engineering Analysis

### 1. Isolation Mechanisms

Our runtime achieves robust process and filesystem isolation using Linux namespaces via the `clone()` system call:

- **PID Namespace (`CLONE_NEWPID`):** Ensures an isolated process tree where the container perceives itself as PID 1. The parent supervisor maps these to real host PIDs, retaining full lifecycle control.
- **UTS Namespace (`CLONE_NEWUTS`):** Provides a partitioned view of the hostname and domain name, so each container has its own identity.
- **Mount Namespace (`CLONE_NEWNS`):** Coupled with `chroot()`, this ensures the container's filesystem view is confined to its own duplicated rootfs, preventing escapes via `/..` traversal.

> **Design Note:** For a course project demonstrating the concept, `chroot()` is sufficient and significantly easier to implement correctly. Production runtimes (e.g., `runc`) use `pivot_root()` for stronger security hardening.

---

### 2. Supervisor and Process Lifecycle

The supervisor is a long-running background daemon managing container spawning, monitoring, and cleanup:

- **Spawning:** `clone()` creates an isolated child. Pipe bindings route the container's stdout to a producer thread. An independent stack is `malloc`'d and passed into `clone()`.
- **Asynchronous Reaping:** On child exit the kernel delivers `SIGCHLD`. The signal handler sets a flag; the event loop calls `waitpid(-1, WNOHANG)`, unregisters the container from the kernel monitor, and joins the producer thread — all without blocking the server.
- **Leak Protection:** `cleanup_all()` sends `SIGTERM` to all containers, reaps all children, joins all threads, frees malloc'd stacks, unlinks the control socket, and closes the monitor device FD.

---

### 3. IPC, Threads, and Synchronization

| Shared Structure | Race Condition | Protection |
|---|---|---|
| Container Metadata List | Concurrent CLI edits vs. supervisor reads | `pthread_mutex_t` (blocking) |
| Log Message Buffer | Producer thread vs. logger consumer | `pthread_mutex` + condition vars (`not_full` / `not_empty`) |
| Kernel Module `container_info` list | Timer callback vs. CLI ioctl calls | `DEFINE_MUTEX` — kernel mutex |

**Design Decision:** The CLI parser was upgraded from `strtok` to strongly typed `sscanf()` with the format `"%*s %63s %lu %lu %d %255s %255[^\n]"`. This eliminates fatal truncation bugs when command arguments contained embedded spaces (e.g., shell `-c` invocations).

The IPC system also implements a **synchronous `run` command**: after sending the start request to the supervisor, the CLI wrapper polls via socket until the container enters the `exited` state, blocking the caller visually — similar to `docker run` (non-detached mode).

---

### 4. Memory Management and Enforcement (LKM)

The kernel module (`monitor.ko`) provides RSS-based memory limiting that user-space cannot replicate safely:

- **Soft Limits:** Checked every second via a kernel `timer_list` callback. Generates a `pr_warn` to `dmesg` when a container's RSS exceeds its soft threshold.
- **Hard Limits:** A strict ceiling. Exceeding it triggers `send_sig(SIGKILL, task, 1)` directly from kernel space, then marks the container as `hard_limit_killed` in the engine's metadata.
- **Why Kernel Space?** User-space polling suffers from TOCTOU (Time-Of-Check to Time-Of-Use) races: a process can burst past a memory threshold and release before user-space samples it. Intercepting RSS inside the kernel eliminates this window.

**Resource Cleanup Summary:**

| Resource | Cleanup Mechanism | Evidence |
|---|---|---|
| Container child processes | `SIGCHLD` → `waitpid(-1, WNOHANG)` | No `<defunct>` entries in `ps aux` |
| Producer threads | Pipe EOF on container exit; thread returns naturally | Thread joined / detached |
| Logger consumer thread | `bounded_buffer_begin_shutdown()` → `pthread_join(log_thread)` | Supervisor waits for logger |
| UNIX socket file | `unlink(CONTROL_PATH)` before bind and on exit | `/tmp/mini_runtime.sock` removed |
| Monitor device FD | `close(monitor_fd)` in shutdown path | Confirmed via `/proc/<pid>/fd` |
| Kernel `container_info` list | `del_timer_sync()` + `list_for_each_entry_safe` + `kfree()` in `monitor_exit()` | `dmesg`: "Module unloaded, no leaks" |

---

### 5. Scheduling Experiments (Task 5)

Priority is set via `setpriority(PRIO_PROCESS, 0, nice_value)` inside `child_fn()` before `exec()`.

> **Design Note:** Using `nice`/`setpriority()` is the most direct way to observe CFS weight-based scheduling without additional cgroup configuration.

#### Experiment A — CPU-Bound Competition
- **Setup:** Two instances of `/cpu_hog 10`, one at `nice=-10`, one at `nice=10`
- **Result:** The `nice=-10` instance dominated CFS vruntime quota, absorbing far higher tick-quanta and completing significantly faster than the `nice=10` instance.

#### Experiment B — CPU vs I/O Contention
- **Setup:** `/cpu_hog 20` (CPU-intensive) against `/io_pulse` (I/O-bound)
- **Result:** Although `cpu_hog` dominated raw clock usage, `io_pulse` demonstrated highly responsive yields. CFS favours I/O-bound tasks waking from sleep, preventing full CPU lockout from the compute-heavy task.

---

## Design Decisions

### `chroot()` vs `pivot_root()`
For a course project demonstrating the concept, `chroot()` is sufficient and significantly easier to implement correctly. Production runtimes (e.g., `runc`) use `pivot_root()` for security hardening.

### Single Supervisor + Threads
A thread-per-container model scales linearly in memory (~8 MB stack per thread). For small container counts (≤ `MAX_CONTAINERS = 32`) this is negligible and maps cleanly to the producer-consumer pattern. For thousands of containers, an `epoll`-based event-driven model would be preferred.

### UNIX Domain Socket for IPC
A stream socket (`SOCK_STREAM`) at `/tmp/mini_runtime.sock` provides full-duplex, connection-oriented communication with backpressure — the same mechanism used by the Docker daemon socket. A named FIFO would be simpler but cannot handle concurrent clients or request/response framing naturally.

### Timer-Based vs Notification-Based Kernel Monitoring
Periodic 1-second polling introduces up to 1 second of latency between a limit breach and enforcement. A notification-based approach using kernel memory event hooks would be more responsive but significantly more complex. For demo purposes, 1-second granularity is sufficient and reproducible.

---

## Repository Structure

```
.
├── engine.c              # User-space runtime: supervisor, CLI, container lifecycle
├── monitor.c             # Kernel module (LKM): RSS-based memory enforcement
├── monitor_ioctl.h       # Shared ioctl command definitions
├── cpu_hog.c             # Test workload: CPU-bound spinning
├── io_pulse.c            # Test workload: I/O-bound read/write bursts
├── memory_hog.c          # Test workload: controlled memory allocation
├── Makefile              # Unified build for user-space + kernel module
├── environment-check.sh  # Checks build environment prerequisites
└── README.md             # This file
```

---

## References

1. Kerrisk, M. (2010). *The Linux Programming Interface*. No Starch Press. Ch. 28, 44, 57.
2. Love, R. (2010). *Linux Kernel Development* (3rd ed.). Addison-Wesley. Ch. 3, 8, 11.
3. Linux man-pages: `clone(2)`, `chroot(2)`, `pivot_root(2)`, `setpriority(2)`, `ioctl(2)`, `waitpid(2)`. https://man7.org/linux/man-pages/
4. Linux Kernel Documentation – Namespaces: https://www.kernel.org/doc/html/latest/admin-guide/namespaces.html
5. Linux Kernel Documentation – CFS Scheduler: https://www.kernel.org/doc/html/latest/scheduler/sched-design-CFS.html
6. Docker Inc. (2024). Understanding Docker architecture. https://docs.docker.com/get-started/docker-overview/
7. PES University – UE24CS242B Operating Systems Course Materials. Jan–May 2026.

---

## Glossary

| Term | Definition |
|------|-----------|
| CFS | Completely Fair Scheduler – Linux default CPU scheduler |
| chroot | Change root – restricts process's visible filesystem |
| clone() | Linux syscall to create a child with selectable shared/unshared resources |
| FD | File Descriptor – integer handle for open files, sockets, pipes |
| ioctl | Input/Output Control – device-specific operations outside read/write |
| LKM | Linux Kernel Module – object code loaded into the running kernel |
| namespace | Kernel abstraction providing isolated views of global system resources |
| nice | Priority adjustment (-20 to 19); lower = higher priority in CFS |
| PID | Process Identifier – unique integer assigned to each process |
| RSS | Resident Set Size – physical memory currently held by a process |
| SIGCHLD | Signal delivered to parent when a child process terminates |
| SIGKILL | Signal 9 – unconditional kill; cannot be caught or ignored |
| SIGTERM | Signal 15 – polite termination request; can be caught for cleanup |
| UTS | UNIX Time-sharing System – namespace type isolating hostname |
| UNIX domain socket | IPC mechanism for same-host communication via filesystem path |
| WSL | Windows Subsystem for Linux – **not supported** for this project |
