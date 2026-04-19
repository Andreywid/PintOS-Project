# PintOS – Complete Operating System Implementation

This repository contains a complete implementation of the four instructional
projects of PintOS, a teaching operating system originally developed at Stanford.

The implementation extends the base kernel to support preemptive thread
scheduling, user program execution, virtual memory with swapping, and a
fully functional file system with buffering and hierarchical directories.

The objective of this work was not only to satisfy the official test suite,
but to design and integrate the core subsystems of a small monolithic kernel
while preserving correctness, synchronization safety, and performance.

---

## System Architecture Overview

The final system consists of four tightly integrated subsystems:

- Kernel thread scheduler
- User process management layer
- Virtual memory subsystem
- Persistent file system with buffer cache

Each project incrementally extends the kernel while preserving compatibility
with previously implemented functionality. Particular care was taken to avoid
regressions across subsystems (e.g., VM interacting safely with file I/O,
or priority scheduling interacting correctly with synchronization primitives).

---

## Project 1 – Threads and Scheduling

### Core Extensions
- Elimination of busy-waiting in `timer_sleep`
- Strict priority-based scheduling
- Nested priority donation
- Multi-Level Feedback Queue Scheduler (MLFQS)

### Design Approach

Sleeping threads are stored in an ordered structure indexed by wake-up tick.
Timer interrupts wake only the necessary threads, ensuring minimal overhead.

The ready queue is priority-ordered to guarantee that the highest-priority
thread is always selected. Priority donation propagates recursively through
lock dependencies to prevent priority inversion.

The MLFQS implementation follows the PintOS specification precisely, using
fixed-point arithmetic for `load_avg` and `recent_cpu`, since floating-point
operations are not supported in kernel mode.

The scheduler was designed to remain deterministic and compliant with all
official timing-based tests.

### Contributors
- Andreywid Souza ([@Andreywid](https://github.com/Andreywid))
- Luiz Gustavo ([@lzgustavo13](https://github.com/lzgustavo13))

---

## Project 2 – User Programs

### Core Extensions
- Full system call interface
- Safe user–kernel boundary handling
- Process lifecycle management
- File descriptor abstraction per process

### Design Approach

System calls are dispatched through interrupt `0x30`, with argument extraction
performed directly from the user stack after validation.

User pointers are validated before dereferencing to prevent kernel crashes.
Invalid memory accesses result in controlled process termination rather than
kernel failure.

Each process maintains an isolated file descriptor table. File system access
is serialized using a global lock to ensure correctness under concurrency.

Parent–child synchronization is implemented via semaphores to guarantee
correct load reporting and exit status propagation.

### Contributors
- Andreywid Souza ([@Andreywid](https://github.com/Andreywid))
- Luiz Gustavo ([@lzgustavo13](https://github.com/lzgustavo13))
- Artur Vinicius Pereira Fernandes ([@Arturvpf](https://github.com/Arturvpf))

---

## Project 3 – Virtual Memory

### Core Extensions
- Supplemental Page Table (per process)
- Lazy loading of executable segments
- Global frame table with eviction
- Swap space management
- Dynamic stack growth
- Memory-mapped files (`mmap`, `munmap`)
- Page pinning during I/O

### Design Approach

Each process owns a hash-based Supplemental Page Table (SPT) that records
metadata necessary to reconstruct non-resident pages.

Physical frames are managed globally. When memory is exhausted, eviction
is performed using the Clock (second-chance) algorithm, balancing fairness
and implementation complexity.

Evicted pages are written either to swap or back to their originating file,
depending on page type and dirty state.

The page fault handler centralizes decision-making: loading from file,
loading from swap, stack growth, or process termination.

Special care was taken to avoid deadlocks between the VM subsystem and
the file system during page-in and page-out operations.

### Contributors
- Andreywid Souza ([@Andreywid](https://github.com/Andreywid))
- Luiz Gustavo ([@lzgustavo13](https://github.com/lzgustavo13))
- Artur Vinicius Pereira Fernandes ([@Arturvpf](https://github.com/Arturvpf))

---

## Project 4 – File System

### Core Extensions
- Indexed, extensible file structure
- Hierarchical directories
- Relative and absolute path resolution
- Per-thread current working directory
- Fixed-size buffer cache with write-behind
- Clock-based cache replacement

### Design Approach

The file system was redesigned to support file growth through indexed block
structures, eliminating the original fixed-size limitation.

Directory support includes full path parsing, traversal, and isolation of
per-thread working directories.

A buffer cache layer intercepts all disk operations. Each cache entry tracks
access and dirty state. Replacement follows the Clock policy, and write-behind
reduces disk traffic while preserving consistency.

Synchronization was carefully designed to avoid race conditions between
cache eviction, inode updates, and concurrent directory operations.

### Contributors
- Andreywid Souza ([@Andreywid](https://github.com/Andreywid))
- Luiz Gustavo ([@lzgustavo13](https://github.com/lzgustavo13))
- Artur Vinicius Pereira Fernandes ([@Arturvpf](https://github.com/Arturvpf))

---

## Testing

All official PintOS tests were executed with the following results:

- Project 1 (Threads): 129 / 129 tests passed
- Project 2 (User Programs): 80 / 80 tests passed
- Project 3 (Virtual Memory): 113 / 113 tests passed
- Project 4 (File System): 159 / 159 tests passed

The implementation maintains compatibility across all subsystems and
passes the full regression suite without modification to the test harness.

---

## Repository Structure

```text
src/
 ├── threads/    Kernel thread scheduler and synchronization primitives
 ├── userprog/   User process management and system calls
 ├── vm/         Virtual memory subsystem
 └── filesys/    File system and buffer cache
```
