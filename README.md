# Multi-Container Runtime and Kernel Memory Monitor

This repository contains a lightweight Linux container runtime in C with:

- a long-running parent supervisor in user space
- a control-plane CLI over UNIX domain sockets
- concurrent bounded-buffer log capture for container stdout and stderr
- a Linux kernel module that enforces soft and hard memory limits
- scheduling experiment scripts and cleanup checks

This README is organized to match the assignment rubric in project-guide.md.

## 1. Team Information

- Member 1: <Name>, <SRN>
- Member 2: <Name>, <SRN>

## 2. Build, Load, and Run Instructions

### 2.1 Environment

- Ubuntu 22.04 or 24.04 VM
- Secure Boot OFF
- Not WSL

Install dependencies:

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

Optional preflight check:

```bash
cd boilerplate
chmod +x environment-check.sh
sudo ./environment-check.sh
```

### 2.2 Root Filesystem Setup

From repo root:

```bash
cd boilerplate
mkdir -p rootfs-base
wget -O alpine-minirootfs-3.20.3-x86_64.tar.gz \
  https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

# One writable copy per container
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta
```

### 2.3 Build

CI-safe user-space compile:

```bash
make -C boilerplate ci
```

Full build (user space + kernel module):

```bash
cd boilerplate
make
```

### 2.4 Load Kernel Module and Verify Device

```bash
cd boilerplate
sudo insmod monitor.ko
ls -l /dev/container_monitor
```

### 2.5 Start Supervisor and Use CLI

Start supervisor in one terminal:

```bash
cd boilerplate
sudo ./engine supervisor ./rootfs-base
```

In another terminal:

```bash
cd boilerplate
sudo ./engine start alpha ./rootfs-alpha /bin/sh --soft-mib 48 --hard-mib 80 --nice 0
sudo ./engine start beta  ./rootfs-beta  /bin/sh --soft-mib 64 --hard-mib 96 --nice 0

sudo ./engine ps
sudo ./engine logs alpha
sudo ./engine stop alpha
sudo ./engine stop beta
```

Foreground run mode example:

```bash
cd boilerplate
sudo ./engine run gamma ./rootfs-alpha "/bin/sh -c 'echo hello; exit 7'"
echo "engine run exit code: $?"
```

### 2.6 Workload Setup for Experiments

Build workloads:

```bash
cd boilerplate
make
```

Copy workload binaries into container rootfs copies before launch:

```bash
cp ./cpu_hog ./rootfs-alpha/
cp ./io_pulse ./rootfs-beta/
cp ./memory_hog ./rootfs-alpha/
```

### 2.7 Task 5 and Task 6 Scripts

Task 5 scheduler experiments:

```bash
cd boilerplate
sudo ./task5_experiments.sh
```

Expected output artifacts:

- experiments/task5_results.csv
- logs/e1_high.log
- logs/e1_low.log
- logs/e2_cpu.log
- logs/e2_io.log

Task 6 cleanup verification:

```bash
cd boilerplate
sudo ./task6_cleanup_check.sh
```

### 2.8 Shutdown and Cleanup

```bash
cd boilerplate
sudo pkill -f "./engine supervisor" || true
sudo rmmod monitor
make clean
```

## 3. Demo with Screenshots

Add one annotated screenshot for each required checkpoint.

1. Multi-container supervision
   - Show one supervisor process and at least two running containers
2. Metadata tracking
   - Show engine ps output with states, pids, limits, exit data, log path
3. Bounded-buffer logging
   - Show logs captured for a container and evidence of concurrent logging
4. CLI and IPC
   - Show a CLI request and supervisor response over control socket path
5. Soft-limit warning
   - Show dmesg entry for SOFT LIMIT event
6. Hard-limit enforcement
   - Show dmesg HARD LIMIT kill and engine ps showing hard_limit_killed
7. Scheduling experiment
   - Show experiment outputs or CSV values with observable differences
8. Clean teardown
   - Show no zombies, cleaned control socket, and supervisor shutdown

Place screenshots under docs/screenshots/ and reference them here.

- docs/screenshots/01-multi-container-supervision.png
- docs/screenshots/02-metadata-tracking-ps.png
- docs/screenshots/03-bounded-buffer-logging.png
- docs/screenshots/04-cli-and-ipc.png
- docs/screenshots/05-soft-limit-warning-dmesg.png
- docs/screenshots/06-hard-limit-enforcement.png
- docs/screenshots/07-scheduling-experiment-results.png
- docs/screenshots/08-clean-teardown-no-zombies.png

## 4. Engineering Analysis

### 4.1 Isolation Mechanisms

The runtime creates containers with clone using CLONE_NEWPID, CLONE_NEWUTS, and CLONE_NEWNS.
Inside the child, the mount namespace is privatized and the process chroots into the container-specific rootfs.
Then proc is mounted inside that namespace so tools such as ps operate on container PID view.
The kernel itself remains shared among all containers, so this is OS-level isolation, not VM-level isolation.

### 4.2 Supervisor and Process Lifecycle

The supervisor is a persistent parent that accepts control requests from short-lived CLI clients.
It tracks container metadata, reaps children via SIGCHLD + waitpid, and records terminal states.
Manual stop requests set stop_requested before signaling the container, enabling clear attribution between manual stop and hard-limit kill.

### 4.3 IPC, Threads, and Synchronization

The design uses two separate IPC paths.

- Control plane: UNIX domain socket at /tmp/mini_runtime.sock for CLI to supervisor commands
- Logging plane: per-container pipe from child stdout and stderr to supervisor producer thread

Producer threads push chunks into a bounded buffer protected by mutex + condition variables.
Consumer thread pops chunks and appends to per-container log files.
Without these primitives, races can corrupt buffer indices, lose log chunks, or deadlock producers and consumers.

### 4.4 Memory Management and Enforcement

The kernel module periodically measures RSS of monitored PIDs.
RSS indicates resident physical pages but not all virtual memory mappings.
Soft limit emits one warning per entry when first exceeded.
Hard limit triggers SIGKILL and removal from monitor list.
Enforcement belongs in kernel space because process memory accounting and reliable kill authority are kernel responsibilities.

### 4.5 Scheduling Behavior

Experiments compare priority-differentiated CPU-bound containers and mixed CPU/I-O workloads.
Observed completion differences are consistent with Linux CFS goals:

- higher-priority (lower nice) CPU workload receives greater CPU share
- I-O-bound workload remains responsive due to frequent sleeps/yields
- fairness and throughput depend on runnable mix and priority weights

## 5. Design Decisions and Tradeoffs

1. Namespace + chroot isolation
   - Decision: CLONE_NEWPID/NEWUTS/NEWNS + chroot + proc mount.
   - Tradeoff: simpler than pivot_root, but chroot can be less strict against some escape patterns.
   - Justification: faster implementation with required assignment isolation behavior.

2. Supervisor architecture
   - Decision: one long-running daemon and short-lived command clients.
   - Tradeoff: requires explicit IPC protocol and lifecycle management.
   - Justification: clean separation of stateful control and stateless CLI UX.

3. Logging and bounded buffer
   - Decision: producer/consumer with mutex + condition variables.
   - Tradeoff: more complexity than direct writes, but avoids slow I/O blocking producers.
   - Justification: robust concurrent capture with bounded memory and controlled shutdown.

4. Kernel monitor locking
   - Decision: mutex over shared monitored list.
   - Tradeoff: potential contention during timer traversal.
   - Justification: ioctl and timer paths are process context and can sleep; mutex keeps implementation simple and safe.

5. Scheduler experiments
   - Decision: automated scripts with repeatable container IDs and CSV output.
   - Tradeoff: synthetic workloads may not represent all real-world behavior.
   - Justification: reproducible, rubric-aligned comparisons with measurable outcomes.

## 6. Scheduler Experiment Results

Run:

```bash
cd boilerplate
sudo ./task5_experiments.sh
cat experiments/task5_results.csv
```

Paste your measured results table below (example schema):

| experiment | container_id | nice | workload | completion_ms |
|---|---|---:|---|---:|
| exp1_cpu_vs_cpu_priority | e1_high | -5 | cpu_hog | <fill> |
| exp1_cpu_vs_cpu_priority | e1_low  | 10 | cpu_hog | <fill> |
| exp2_cpu_vs_io_same_priority | e2_cpu | 0 | cpu_hog | <fill> |
| exp2_cpu_vs_io_same_priority | e2_io  | 0 | io_pulse | <fill> |

Interpretation notes:

- Explain whether lower nice value completed faster in exp1 and why.
- Explain responsiveness/latency behavior of I-O workload in exp2.
- Relate outcomes to fairness, responsiveness, and throughput.

## 7. File Map

- boilerplate/engine.c: user-space supervisor, CLI control path, logging pipeline, namespace launch
- boilerplate/monitor.c: kernel monitor module, memory checks, soft/hard enforcement
- boilerplate/monitor_ioctl.h: shared ioctl request and command IDs
- boilerplate/cpu_hog.c, boilerplate/io_pulse.c, boilerplate/memory_hog.c: workloads
- boilerplate/task5_experiments.sh: scheduler experiment automation
- boilerplate/task6_cleanup_check.sh: teardown and zombie checks

## 8. Notes for Evaluators

- Use unique writable rootfs directories for concurrent live containers.
- engine run returns the child exit code (or 128 + signal for signaled exit).
- stop attribution is tracked with stop_requested and shown in metadata state.