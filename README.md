# MYFS: Memory-Mapped Userspace POSIX File System

![C](https://img.shields.io/badge/Language-C11-blue.svg)
![FUSE](https://img.shields.io/badge/FUSE-libfuse3-green.svg)
![Platform](https://img.shields.io/badge/Platform-Linux%20%7C%20WSL2-lightgrey.svg)
![Course](https://img.shields.io/badge/Course-OS%20(CS303%20%2F%20CS353)-orange.svg)

**MYFS** is a persistent, block-aligned POSIX file system implemented in userspace over `libfuse3`. Unlike standalone CLI file system simulators, MYFS mounts onto a standard Linux directory via the kernel Virtual File System (VFS), allowing unmodified POSIX utilities (`ls`, `cat`, `cp`, `mv`, `rm`, `mkdir`, `rmdir`, `stat`, `df`, `md5sum`) and external scripts (Python, C++, Bash) to interact with it transparently.

By mapping a 4 MB backing disk image (`disk.img`) directly into virtual memory via `mmap(MAP_SHARED)`, MYFS eliminates secondary `pread()`/`pwrite()` system calls and double page caching, reducing metadata and payload operations to $O(1)$ pointer arithmetic in Ring 3.

---

## Key Features

* **Zero-Syscall Persistence Engine (`mmap` + `MAP_SHARED`):** Projects the 4 MB `disk.img` directly into the daemon's virtual address space at mount time (`myfs_init()`), letting the CPU MMU and Linux page cache handle dirty-page writeback asynchronously without secondary kernel traps.
* **$O(1)$ Zero-Copy Soft-Delete FSM:** Decouples logical file deletion from $O(B)$ bitmap clearing using a 3-state directory entry finite state machine (`valid` $\in \{0, 1, 2\}$). Running `rm` flips a single byte (`valid = 2`) in constant time, preserving data blocks for instant in-place recovery.
* **In-Band VFS Control & Telemetry Endpoints:** Intercepts virtual control files directly inside the path resolver without consuming disk inodes:
  * `/.diskstat` — Live ASCII storage monitor displaying superblock counters, trash queue count, and a 64-block allocation map.
  * `/.recover` — Restores all soft-deleted files (`valid == 2` $\rightarrow$ `1`) in-place.
  * `/.purge` — Permanently reclaims trashed files, zeroing data blocks and releasing bitmap bits (`valid == 2` $\rightarrow$ `0`).
* **Cache-Line & Page-Aligned On-Disk Layout:** Compile-time C11 `_Static_assert` checks lock `struct superblock` to 4,096 bytes, `struct inode` to 512 bytes (8 per 4 KB page), and `struct dir_entry` to 64 bytes (1 per x86_64 L1 cache line).
* **Lazy Allocation, Sparse Files & Atomic `O_TRUNC` Handling:** Empty files (`touch`) consume zero data blocks; partial or offset writes allocate and zero-fill 4 KB blocks on demand; shell redirection (`>`) intercepts `O_TRUNC` in `myfs_open()` to prevent leaked blocks.

---

## System Architecture

```text
[User Process: ls / cat / cp / rm / df / bench]
       │
       │ 1. Standard POSIX syscall (open, read, write, statvfs)
       ▼
[Linux Kernel: Virtual File System (VFS)]
       │
       │ 2. Forwarded to FUSE kernel driver -> serialized to /dev/fuse
       ▼
[Userspace Daemon: ./myfs (libfuse3 event loop)]
       │
       │ 3. Dispatched to my_ops callback (e.g., myfs_write / myfs_read)
       │ 4. Direct pointer mutation on mmap() region (MAP_SHARED)
       ▼
[Linux Page Cache] ═══(Asynchronous OS dirty-page flush)═══▶ [disk.img (4 MB)]
```

### 4 MB On-Disk Partition Layout (`1,024` Blocks × `4,096` Bytes)

| Blocks | Byte Offset | Component | Size / Capacity | Role |
| :--- | :--- | :--- | :--- | :--- |
| **Block 0** | `0x000000` | Superblock | 4 KB (1 block) | Magic (`0x5553464D`), total/free block & inode counters |
| **Block 1** | `0x001000` | Inode Bitmap | 4 KB (128 bits active) | Tracks allocation state of 128 inodes |
| **Block 2** | `0x002000` | Data Bitmap | 4 KB (1,005 bits active) | Tracks allocation state of 1,005 usable data blocks |
| **Blocks 3–18** | `0x003000` | Inode Table | 64 KB (16 blocks) | 128 inodes × 512 B (8 inodes per 4 KB page) |
| **Blocks 19–1023** | `0x013000` | Data Region | 4,020 KB (1,005 blocks) | Directory entry tables (64/block) & file data payloads |

> **Note:** On a fresh format, Inode `0` and Data Block `0` (physical Block `19`) are reserved for the root directory (`/`) containing `.` and `..`, leaving **1,004 free data blocks** and **127 free inodes**.

---

## Repository Structure

```text
kai_os/
├── fs_layout.h      # On-disk constants, magic number (0x5553464D), and aligned structs
├── mkfs_myfs.c      # Standalone 4 MB disk.img formatter and root directory initializer
├── myfs.c           # Core libfuse3 userspace daemon (14 POSIX VFS callbacks + telemetry)
├── bench.c          # Three-tier CLOCK_MONOTONIC write latency micro-benchmark harness
└── mnt/             # Mount point directory for the active file system
```

---

## Prerequisites

* **OS:** Linux (Ubuntu 22.04+ or WSL2 with native Linux kernel)
* **Compiler & Build Tools:** `gcc`, `pkg-config`
* **Libraries:** `libfuse3-dev` and `fuse3`

Install dependencies on Ubuntu/Debian:
```bash
sudo apt update
sudo apt install -y gcc pkg-config libfuse3-dev fuse3
```

---

## Build & Quick Start

1. **Compile the formatter, FUSE daemon, and benchmark suite:**
   ```bash
   gcc -Wall -O2 mkfs_myfs.c -o mkfs_myfs
   gcc -Wall -O2 myfs.c -o myfs $(pkg-config fuse3 --cflags --libs)
   gcc -Wall -O2 bench.c -o bench
   ```

2. **Format the 4 MB backing image (`disk.img`) and mount MYFS:**
   ```bash
   mkdir -p mnt
   ./mkfs_myfs
   ./myfs mnt -s
   ```
   *(Pass `-f` in addition to `-s` if you want to run the daemon in the foreground for debugging).*

3. **Interact using standard POSIX commands:**
   ```bash
   # Create nested directories and write files
   mkdir -p mnt/projects/os
   echo "Hello from userspace VFS!" > mnt/projects/os/notes.txt
   cat mnt/projects/os/notes.txt

   # Check live storage telemetry
   cat mnt/.diskstat
   ```

4. **Test $O(1)$ Soft-Delete & Zero-Copy Recovery:**
   ```bash
   # Logical delete (flips directory entry state to valid = 2)
   rm mnt/projects/os/notes.txt
   ls mnt/projects/os/

   # Restore deleted file in-place
   cat mnt/.recover
   cat mnt/projects/os/notes.txt

   # Permanently purge trashed files and free bitmap blocks
   rm mnt/projects/os/notes.txt
   cat mnt/.purge
   ```

5. **Unmount the file system:**
   ```bash
   fusermount3 -u mnt
   ```

---

## Performance Benchmarks

Evaluated on `LENOVOKai` (WSL2) over 1,000 iterations using `clock_gettime(CLOCK_MONOTONIC)` across 1 to 12 blocks (`4 KB` to `48 KB`):

| Payload Size | Native Ext4 (Ring 0) | MYFS (`mmap` FUSE) | Std FUSE (`pwrite` Emulated) | Speedup vs. Std FUSE |
| :--- | :---: | :---: | :---: | :---: |
| **4 KB** (1 blk) | `0.43 µs` | `44.03 µs` | `39.04 µs` | Baseline (VFS upcall bound) |
| **16 KB** (4 blks) | `1.48 µs` | `159.23 µs` | `162.19 µs` | **+1.8%** |
| **32 KB** (8 blks) | `3.44 µs` | `316.45 µs` | `385.19 µs` | **+17.8%** |
| **48 KB** (12 blks) | `5.22 µs` | `492.03 µs` | `544.89 µs` | **+9.7%** |

* **Why MYFS scales better on multi-block I/O:** Every userspace FUSE operation incurs a fixed ~$40~\mu\text{s}$ VFS-to-userspace context-switch cost across `/dev/fuse`. As payload size grows from 1 block to 12 blocks, `mmap(MAP_SHARED)` avoids $B$ secondary `pwrite()` Ring 3 $\rightarrow$ Ring 0 transitions, saving up to **$68.74~\mu\text{s}$** per write.

---

## Author

**Kairav Nandi**

(made as a project for my OS course)
