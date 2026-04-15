# Multi-Container Runtime

**Team:** [Your Names and SRNs]

---

## Build, Load, and Run Instructions

### Prerequisites
- Ubuntu 22.04 or 24.04 VM (WSL will not work)
- Secure Boot OFF
- Root access (sudo)

### Build

```bash
cd ~/OS-Jackfruit/boilerplate
make clean
make
```

### Prepare Root Filesystem

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
# Start containers
sudo ./engine start alpha ./rootfs-alpha /bin/sh
sudo ./engine start beta ./rootfs-beta /bin/sh

# List containers
sudo ./engine ps

# View logs
sudo ./engine logs alpha

# Stop containers
sudo ./engine stop alpha
sudo ./engine stop beta

# Memory limit testing
cp memory_hog ./rootfs-alpha/
sudo ./engine start mem1 ./rootfs-alpha "/memory_hog 1 500"
sleep 5
sudo dmesg | grep container_monitor

# Scheduling experiments
cp cpu_hog io_pulse ./rootfs-alpha/
sudo ./engine start cpu1 ./rootfs-alpha "/cpu_hog 10" --nice 10
sudo ./engine start cpu2 ./rootfs-alpha "/cpu_hog 10" --nice -10
```

### Cleanup

```bash
# Stop supervisor (Ctrl+C in Terminal 1)
# Unload module
sudo rmmod monitor
```

---

## Demo Screenshots

### 1. Multi-Container Supervision
[Include screenshot showing two or more containers running under one supervisor]

### 2. Metadata Tracking
[Include screenshot of `ps` command showing tracked container metadata]

### 3. Bounded-Buffer Logging
[Include screenshot of log file contents captured through logging pipeline]

### 4. CLI and IPC
[Include screenshot of CLI command being issued and supervisor responding]

### 5. Soft-Limit Warning
[Include screenshot of dmesg showing SOFT LIMIT warning event]

### 6. Hard-Limit Enforcement
[Include screenshot of dmesg showing HARD LIMIT enforcement and container marked as hard_limit_killed]

### 7. Scheduling Experiment
[Include screenshot of terminal output from scheduling experiment with different nice values]

### 8. Clean Teardown
[Include screenshot showing no zombies and clean shutdown]

---

## Engineering Analysis

### 1. Isolation Mechanisms

Our runtime achieves process and filesystem isolation through Linux namespaces:

**PID Namespace (`CLONE_NEWPID`):** Creates an isolated process tree where each container sees its own PIDs starting from 1. The parent process outside the container maintains the real host PIDs, allowing the supervisor to track and manage containers while each container believes it owns PID 1.

**UTS Namespace (`CLONE_NEWUTS`):** Provides isolated hostname and domain name. Containers can have their own hostname without affecting the host or other containers.

**Mount Namespace (`CLONE_NEWNS`):** Allows each container to have its own mount table. Combined with `chroot()`, this restricts the container's view of the filesystem to only its rootfs directory.

**What the kernel still shares:**
- CPU time (though scheduling priorities can be adjusted)
- Memory (enforced via kernel module soft/hard limits)
- Network interfaces (by default - network namespace not isolated)
- IPC mechanisms (by default)

### 2. Supervisor and Process Lifecycle

The long-running parent supervisor serves several critical purposes:

**Process Creation:** The supervisor uses `clone()` with namespace flags to create isolated child processes. Each clone() call creates a new container with its own set of namespaces.

**Parent-Child Relationships:** The supervisor maintains the parent-child relationship with all containers. This allows:
- Signal delivery (SIGTERM for graceful stop, SIGKILL for forced termination)
- Child reaping via `waitpid()` to prevent zombie processes
- Lifecycle management (start, stop, monitor)

**SIGCHLD Handling:** When a container exits, the kernel sends SIGCHLD to the supervisor. Our signal handler sets a flag, and the event loop reaps the child, updates container state, and unregisters from the kernel monitor.

**Metadata Tracking:** The supervisor maintains an in-memory linked list of container records, protected by a mutex for thread safety. Each record contains PID, state, limits, log paths, and exit information.

### 3. IPC, Threads, and Synchronization

Our project uses two IPC mechanisms and a bounded-buffer logging design:

**Control Channel (UNIX Domain Socket):**
- CLI processes connect to the supervisor via `/tmp/mini_runtime.sock`
- Commands are text-based (start, stop, ps, logs)
- Simple request-response pattern

**Logging Channel (Pipe + Thread):**
- Container stdout/stderr connected to supervisor via pipes
- Producer thread reads from pipe and pushes to bounded buffer
- Consumer thread pops from buffer and writes to log files

**Race Conditions and Synchronization:**

| Shared Data | Race Condition | Solution |
|-------------|----------------|----------|
| Container list | Concurrent add/remove by CLI threads | `pthread_mutex` protects all list operations |
| Bounded buffer | Multiple producers, one consumer | `pthread_mutex` + `not_empty`/`not_full` condition variables |
| Kernel module list | Timer callback vs ioctl calls | `DEFINE_MUTEX` protects list traversal and modification |

**Synchronization Justification:**
- **Mutex:** Appropriate for protecting data accessed by multiple threads in process context. Allows blocking, saving CPU compared to spinlocks.
- **Condition Variables:** Used with mutex for the bounded buffer to efficiently wait when buffer is empty (consumer) or full (producer).
- **No Semaphores Needed:** The mutex + condition variable pattern provides all necessary synchronization.

### 4. Memory Management and Enforcement

**RSS (Resident Set Size):** Measures the physical memory (RAM) currently resident for a process. This includes:
- Text (code)
- Data (initialized/uninitialized)
- Heap
- Stack

**What RSS does NOT measure:**
- Memory-mapped files that haven't been accessed
- Swapped-out pages
- Kernel memory used by the process

**Soft vs Hard Limits:**
- **Soft limit (40 MiB default):** Triggers a warning event when exceeded. The process continues running. Used to alert operators before hard limits are hit.
- **Hard limit (64 MiB default):** Terminates the process with SIGKILL when exceeded. Used to enforce resource caps and prevent memory exhaustion.

**Why kernel-space enforcement:**
User-space cannot reliably enforce memory limits because:
1. **TOCTOU Race:** A process could allocate memory between a user-space check and the actual enforcement.
2. **Untrusted Process:** A misbehaving process could manipulate its memory reporting.
3. **Timer Granularity:** User-space polling has latency; kernel timers run precisely.
4. **Privilege:** Only kernel code can safely deliver SIGKILL to any process.

Our kernel module uses a timer callback that runs every second, checking RSS via `get_mm_rss()` and enforcing limits atomically.

### 5. Scheduling Behavior

Linux uses the Completely Fair Scheduler (CFS) with priority support via nice values.

**Scheduling Goals:**
- **Fairness:** Each process gets a fair share of CPU time proportional to its weight
- **Responsiveness:** Interactive processes should wake up quickly
- **Throughput:** Batch processes should complete as fast as possible

**Nice Values (-20 to +19):**
- Lower nice = higher priority = more CPU time
- Higher nice (e.g., +10) = lower priority = less CPU time
- Default nice = 0

**Experiment Results:**

When running `cpu_hog` with different nice values:
- **nice=-10 (high priority):** Process gets more CPU time slices
- **nice=+10 (low priority):** Process gets fewer CPU time slices

The CFS weights processes based on nice values, so a higher-priority process accumulates "virtual time" more slowly, receiving more actual CPU time.

For I/O-bound vs CPU-bound:
- I/O-bound processes (like `io_pulse`) sleep frequently, yielding CPU
- CPU-bound processes (like `cpu_hog`) compete continuously for CPU cycles
- Running both together: I/O-bound gets good responsiveness despite CPU-bound hogging, because it only needs CPU in short bursts

---

## Design Decisions and Tradeoffs

### Namespace Isolation

**Choice:** Used `unshare()` with `CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS` + `chroot()`

**Tradeoff:** Simpler than `pivot_root()` but allows `/proc` escape via `..` traversal

**Justification:** Adequate for educational purposes. `pivot_root()` would be more secure but adds complexity. For production, additional security measures (seccomp, capabilities) would be needed.

### Supervisor Architecture

**Choice:** Long-running daemon with UNIX domain socket IPC

**Tradeoff:** More complex than simple fork/exec, but enables:
- Multi-container management
- Centralized logging
- Coordinated shutdown

**Justification:** Required for the multi-container requirement. Socket-based CLI is cleaner than signals or shared memory for this use case.

### Bounded Buffer

**Choice:** Mutex + 2 condition variables (not_empty, not_full)

**Tradeoff:** More code than a simple global buffer, but prevents:
- Busy-waiting (wastes CPU)
- Lost wakeups (race conditions)
- Deadlock (proper condition variable usage)

**Justification:** Standard producer-consumer pattern. Clean shutdown capability built-in.

### Kernel Monitor

**Choice:** Timer-based polling with mutex-protected linked list

**Tradeoff:** Uses kernel timer resources, but simple and reliable

**Justification:** Sufficient for the project requirements. More advanced implementations could use cgroups for automatic enforcement.

---

## Scheduler Experiment Results

### Experiment 1: CPU-Bound with Different Priorities

**Setup:**
- Container 1: `cpu_hog 10` with nice=10 (low priority)
- Container 2: `cpu_hog 10` with nice=-10 (high priority)

**Observation:**
The high-priority container (nice=-10) completed faster because it received more CPU time slices. The low-priority container (nice=10) took longer as it was scheduled less frequently.

### Experiment 2: CPU-Bound vs I/O-Bound

**Setup:**
- Container 1: `cpu_hog 20` (CPU intensive)
- Container 2: `io_pulse 20` (I/O intensive)

**Observation:**
The I/O-bound process remained responsive despite CPU-bound competition because:
1. It sleeps between I/O operations
2. CFS scheduler favors runnable interactive processes
3. CPU-bound processes yield CPU more frequently than expected

---

## Project Structure

```
boilerplate/
├── engine.c          # User-space runtime and supervisor
├── monitor.c         # Kernel module for memory monitoring
├── monitor_ioctl.h   # Shared ioctl definitions
├── cpu_hog.c         # CPU-bound workload
├── io_pulse.c        # I/O-bound workload
├── memory_hog.c      # Memory pressure workload
├── Makefile          # Build system
├── environment-check.sh
└── rootfs-*/        # Container filesystems (not in git)
```

---

## References

- Linux Namespaces: https://man7.org/linux/man-pages/man7/namespaces.7.html
- Clone System Call: https://man7.org/linux/man-pages/man2/clone.2.html
- Linux Kernel Module Programming: https://tldp.org/LDP/lkmpg/2.6/lkmpg.pdf
- CFS Scheduler: https://www.kernel.org/doc/html/latest/scheduler/sched-design-CFS.html
