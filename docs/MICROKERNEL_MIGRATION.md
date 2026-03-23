# Microkernel Migration Plan

## Goal

Transform VibeOS from a monolithic kernel into a service-oriented microkernel architecture with:

- functional BIOS boot path with MBR/VBR compatibility
- real disk partitioning
- FAT32 support for boot and system volumes
- storage support for IDE, SATA, and AHCI
- USB boot compatibility
- smaller privileged kernel core
- drivers and high-level services moved out of the kernel core over time

## Current Reality

The repository is still a monolithic kernel with early drivers linked into the core image. The immediate work is to create a migration path that preserves bootability while we progressively split responsibilities.

This document is a live checklist. Items are only marked complete when code lands in the tree.

## Phase 0: Foundation

- [x] Create migration document and tracked execution checklist
- [x] Add microkernel process/service/message scaffolding to the kernel tree
- [x] Add service registry bootstrap during kernel init
- [x] Define kernel-to-service ABI versioning
- [x] Define user-space driver/service launch protocol

## Phase 1: Kernel Core Minimization

- [ ] Reduce kernel core responsibilities to:
- [ ] scheduler
- [ ] low-level memory management
- [x] IPC/message passing
- [ ] interrupt routing
- [ ] process/service supervision
- [x] move filesystem logic behind a service boundary
- [x] move high-level storage management behind a service boundary
- [x] move console/video policy behind a service boundary
- [x] move input policy behind a service boundary
- [x] move socket/network control plane behind a service boundary
- [x] move audio control/data plane behind a service boundary

## Phase 2: Boot Chain

- [x] restore a robust `MBR -> VBR/stage1 -> loader -> kernel` path
- [x] support active partition boot on BIOSes that reject superfloppy layouts
- [x] keep boot-time diagnostics available without text mode dependency
- [x] define boot info handoff contract for the microkernel loader
- [x] add a dedicated loader stage that can load kernel + initial services from FAT32

## Phase 3: Partitioning and Filesystems

- [x] add MBR partition parsing
- [x] add FAT32 reader for boot volume
- [ ] add FAT32 writer support where safe
- [x] define system partition layout
- [x] define data/app partition layout
- [ ] keep backward-compatibility tooling for current raw image format during migration

## Phase 4: Storage Drivers

- [ ] keep legacy IDE path working
- [ ] add AHCI controller detection
- [ ] add AHCI read path
- [ ] add AHCI write path
- [ ] add SATA device enumeration through AHCI
- [x] unify storage devices behind one block-device abstraction
- [ ] add USB mass-storage boot/loading strategy

## Phase 5: Initial User-Space Services

- [ ] storage service
- [ ] filesystem service
- [ ] display service
- [ ] input service
- [ ] log/console service
- [ ] network service
- [ ] audio service
- [ ] process launcher / init service

## Phase 6: Compatibility and Validation

- [x] QEMU regression target
- [ ] Core 2 Duo regression target
- [ ] Pentium / Atom regression target
- [ ] BIOSes requiring active MBR partition
- [ ] USB boot validation matrix
- [ ] IDE/SATA/AHCI validation matrix

## Latest Completed Slice

- `MBR` now relocates itself safely, enables VESA early, selects the active partition, and chainloads a FAT32 `VBR`.
- The FAT32 `VBR` now loads a dedicated `stage2` loader from a reserved-sector slot that does not collide with FAT32 metadata (`FSInfo`, backup boot sector).
- `stage2` now locates `KERNEL.BIN` in the FAT32 root directory and loads it before handing off to the 32-bit kernel entry path.
- The in-kernel storage path now goes through a primary block-device abstraction, with the current ATA driver registered as one backend instead of exposing ATA-specific logic directly to the rest of the kernel.
- The disk image builder now emits a real two-partition image:
  - partition 1: bootable FAT32 volume with `KERNEL.BIN` visible for diagnostics
  - partition 2: raw data volume used by the current AppFS layout
- The kernel storage layer now parses the MBR, discovers the data partition, and exposes it as a logical block volume.
- AppFS, persistence, DOOM assets, and Craft textures are now addressed relative to the data partition instead of absolute disk LBAs.
- A shared `bootinfo` contract now lives in the tree and is populated by the `MBR` for VESA + partition metadata, replacing scattered magic-address parsing in the kernel.
- A new microkernel launch contract now describes bootstrap services/drivers explicitly, captures boot partition metadata for launched tasks, and routes the built-in `init` userland payload through the same scheduler/service bootstrap path intended for later extracted services.
- The launch contract now honors per-service stack sizing and exposes the active launch context to userland through a dedicated syscall, so extracted services can introspect how they were started without scraping boot-time globals.
- Storage syscalls now enter through a microkernel storage-service dispatch layer that resolves the storage service via the service registry, builds request/reply message envelopes, and moves bulk data through kernel-managed transfer buffers instead of raw payload pointers.
- Filesystem syscalls now resolve through a local filesystem service handler with transfer-buffer-backed request/reply dispatch for `open/read/write/close/lseek/stat/fstat`, so the whole current VFS ABI now crosses an explicit service boundary even though the handler still runs in-kernel.
- Video syscalls now resolve through a local video service handler, including text blits, palette transfers, and mode/capability queries, so `syscall.c` no longer talks to the graphics backend directly for the current userland ABI.
- Input and console syscalls now also traverse local service handlers, so keyboard/mouse polling, keymap control, debug output, and text console operations no longer call concrete drivers directly from the syscall table.
- `network` and `audio` now exist as first-class microkernel services with explicit request/reply ABIs and local stub handlers, and VibeOS now exposes BSD-shaped `sys/socket.h`, `sys/uio.h`, and `sys/audioio.h` compatibility headers derived from `compat/sys` so extracted services can preserve familiar contracts without trying to port the BSD kernel wholesale.
- The microkernel service registry now supports process-backed workers over an in-kernel request/reply IPC path, and the current `network`/`audio` stubs run as real scheduled service tasks instead of only inline local handlers.
- `storage`, `filesystem`, `video`, `input`, and `console` now also run as scheduled service workers over that same IPC path, so the main syscall surface no longer resolves through inline handlers for the current service set.
- Transfer buffers now carry explicit owner PID metadata plus per-service grants, so process-backed services only touch request payload memory after the caller shares the specific buffer with read/write permissions.
- The service registry now retains worker launch metadata and can relaunch an offline built-in worker on demand before dispatching a request, adding a first supervision layer without changing the current boot ABI.
- QEMU boot validation now reaches the built-in `init` banner path again after fixing resumed-task register restore in `context_switch`, deferring heavy bootstrap asset discovery / initial FS sync work out of the first boot path, and using direct bootstrap fast paths for serial debug plus legacy text-console syscalls while richer console service plumbing stays available for non-bootstrap request/reply traffic.
- The FAT32 `stage2` loader now retries BIOS extended reads during `KERNEL.BIN` loading, and the current image builder keeps the boot kernel file physically contiguous so the simple linear stage2 path stays reliable while a fully general FAT32 chain loader is still pending.
- Video capability queries now return both the existing mode bitmask and the concrete mode list in the ABI, so userland can enumerate supported resolutions without hardcoding them.
- The kernel now carries only a minimal built-in `init` bootstrap entry plus a tiny AppFS launcher runtime instead of the full embedded userland tree, keeping the BIOS-loaded `kernel.bin` under the low-memory ceiling while apps stay split into independent binaries loaded one at a time.
- The SMP scheduler now tracks per-task CPU preference/history, distributes new tasks toward the least-loaded online core, and filters runnable selection so the same process is not dispatched simultaneously on multiple CPUs during multiprocessor bring-up.
- The LAPIC SMP bring-up path now avoids waiting on PIT ticks with interrupts masked during `INIT/SIPI`, and QEMU `-smp 4` currently reaches `smp: online cpus=4/4` before returning to the `init` bootstrap path.
- QEMU validation currently reaches the kernel and userland path with early debug markers:
  - `M a b d V J S L 2 F K A P 10 11 12 13 14 15 16 17`

## First Implementation Slice

The first slice implemented in-tree is intentionally modest:

1. Introduce structured message envelopes.
2. Introduce a service registry owned by the kernel.
3. Extend `process_t` so services can be represented explicitly.
4. Keep the existing monolithic behavior while creating the interfaces needed for extraction.

This does not finish the migration, but it starts replacing ad-hoc coupling with explicit microkernel-oriented primitives.
