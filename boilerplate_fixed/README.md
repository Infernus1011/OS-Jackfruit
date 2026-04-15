# Multi-Container Runtime

**Team:** [Your Names and SRNs]

---

## Build, Load, and Run Instructions

### Prerequisites
- Ubuntu 22.04 or 24.04 VM (WSL will not work)
- Secure Boot OFF (required for loading unsigned kernel modules)
- Root access (sudo)

### Build Environment
```bash
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
# Start containers (background)
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
sudo ./engine start mem1 ./rootfs-alpha "/memory_hog 1 500" --soft-mib 40 --hard-mib 64
sleep 5
sudo dmesg | grep container_monitor

# Scheduling experiments (Task 5)
cp cpu_hog io_pulse ./rootfs-alpha/
sudo ./engine start cpu1 ./rootfs-alpha "/cpu_hog 10" --nice 10
sudo ./engine start cpu2 ./rootfs-alpha "/cpu_hog 10" --nice -10

# Blocking run test (New feature)
sudo ./engine run blocktest ./rootfs-alpha "/bin/sleep 3"
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
*[Placeholder: Include screenshot showing two or more containers running under one supervisor]*

### 2. Metadata Tracking
*[Placeholder: Include screenshot of `ps` command showing tracked container metadata]*

### 3. Bounded-Buffer Logging
*[Placeholder: Include screenshot of log file contents captured through logging pipeline]*

### 4. CLI and IPC
*[Placeholder: Include screenshot of CLI command being issued and supervisor responding]*

### 5. Soft-Limit Warning
*[Placeholder: Include screenshot of dmesg showing SOFT LIMIT warning event]*

### 6. Hard-Limit Enforcement
*[Placeholder: Include screenshot of dmesg showing HARD LIMIT enforcement and container marked as hard_limit_killed]*

### 7. Scheduling Experiment
*[Placeholder: Include screenshot of terminal output from scheduling experiment with different nice values]*

### 8. Clean Teardown
*[Placeholder: Include screenshot showing no zombies and clean shutdown]*

---

## Engineering Analysis

### 1. Isolation Mechanisms
Our runtime achieves robust process and filesystem isolation using Linux namespaces provided through the `clone()` system call:

- **PID Namespace (`CLONE_NEWPID`):** Ensures an isolated process tree where the container perceives itself as PID 1. The parent supervisor maps these to real host PIDs, retaining full lifecycle control while the container remains isolated.
- **UTS Namespace (`CLONE_NEWUTS`):** Provides a partitioned view of the hostname and domain name.
- **Mount Namespace (`CLONE_NEWNS`):** When coupled with `pivot_root()`, this ensures the container securely swaps out its base filesystem mapping confining it to the duplicated rootfs, fully preventing escapes via `/..` logic present in generic `chroot()` implementations!

### 2. Supervisor and Process Lifecycle
Our long-running background daemon manages the synchronization and lifecycles via the following pipeline:
- **Spawning:** Process creation handles pipeline bindings (routing out internal `pipe_stdout` streams) and provisions independent memory stacks mapped tightly into the `clone()` invocation.
- **Asynchronous Reaping:** When children exit, the kernel issues a `SIGCHLD`. Our trap handler informs the event loop, which safely extracts bounds (`waitpid`), unregisters the kernel monitor daemon, and triggers standard pipeline thread-joins (`pthread_join`) without freezing the server.
- **Leak Protection:** When shutting down, `cleanup_all()` securely sends `SIGTERM`, reaps all lingering children, and accurately sweeps `malloc`'d stack addresses and unattached threads to prevent memory leaks and ghost processes.

### 3. IPC, Threads, and Synchronization
| Shared Structure | Associated Threat | Lock Type |
|-------------|-----------------|----------|
| Container Metadata List | Linked-List Data Races during CLI edits | `pthread_mutex` wrapper (Blocking spin) |
| Log Message Buffer | Producer thread collision via pipe buffering | `pthread_mutex` + Cond Vars (`not_full`/`not_empty`) |
| Kernel Module | Race between Timer checks and CLI calls | `DEFINE_MUTEX` ensuring isolated enforcement |

*Design Decision:* The CLI pipeline was upgraded to specifically sidestep unbounded `strtok` formatting using strongly enforced `sscanf()` format parameters `"%*s %63s %lu %lu %d %255s %255[^\n]"`. This solves fatal vulnerabilities when command strings contained native UNIX spaces arguments such as `-c`.

We've additionally expanded the IPC system to natively implement synchronous `run` commands on the CLI wrapper, blocking visually by interrogating the runtime through socket-ping loops until the process enters an `exited` state!

### 4. Memory Management and Enforcement (LKM)
- **Soft Limits:** Checked per-second via Timer callback; generates kernel-bound warnings to alert the system of bloating configurations.
- **Hard Limits:** A strict threshold. Bypassing sends a lethal `SIGKILL` directly to the `pid` process execution and forcibly detaches it from the supervisor lifecycle.
- **Kernel-Space necessity:** User-space metrics suffer aggressively from TOCTOU (Time-Of-Check to Time-Of-Use) races where applications can surge memory utilization and release before being identified. The Linux Kernel Module intercepts RSS allocation directly.

### 5. Scheduling Behavior (Task 5 Experiments)
We utilize Linux's Completely Fair Scheduler (CFS) via explicit `nice` mappings enforced immediately upon container initialization.

#### CPU Bound Competition
- **Test:** `/cpu_hog 10`
- **Result:** We executed the process strictly on `nice=-10` (high priority) versus `nice=10` (low priority). The `-10` instance significantly dominated the CFS vruntime quota, absorbing far higher tick-quantums resulting in drastically lower completion times.

#### CPU vs IO Contention 
- **Test:** `/cpu_hog 20` against `/io_pulse`
- **Result:** Although `cpu_hog` dominated standard clock usage, `io_pulse` demonstrated highly responsive yields due to the nature of CFS favoring I/O-bound applications waking from sleep, preventing total CPU lockouts from heavy intensive tasks.
