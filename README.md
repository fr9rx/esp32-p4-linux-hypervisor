# esp32-p4-emulator

A hand-written **M-mode hypervisor** for the ESP32-P4 that boots **NOMMU
Linux 6.8-rc1 as a U-mode guest** by trap-and-emulate.

The P4 is a dual-core RISC-V rv32imac with no MMU, no S-mode and no
hypervisor extension. There is no virtualisation hardware here at all. The
monitor runs in M-mode on core 1, puts Linux in U-mode, and emulates the
handful of devices Linux insists on — a CLINT, a PLIC and a 16550 — by
catching the access faults its loads and stores take.

Core 0 keeps running FreeRTOS and ESP-IDF as normal, so the board still has
USB, a panel driver and everything else IDF gives you.

```
core 0                       core 1
  FreeRTOS + ESP-IDF           M-mode monitor  (main/main.c)
  USB host, keyboard             |  trap-and-emulate
  UART0 owner                    v
                               U-mode: Linux 6.8-rc1, NOMMU, rv32
                                       romfs root, XIP from flash
```

The guest is a full Linux system: busybox, lua, nano, less, ncurses — and a
native rv32 toolchain it can build and run its own programs with.

---

## Where things are

| path | what |
|---|---|
| `main/hyp.h` | the contract between the monitor's modules — **read this first** |
| `main/main.c` | core 0's service loop, and nothing else |
| `main/hyp_*.c` | the monitor, split by topic — see "The monitor's source layout" |
| `main/hyp_vectors.S` | trap entry/exit |
| `dts/p4-phase6.dts` | the guest's device tree (the live one) |
| `partitions.csv` | flash layout — read the comments before changing it |
| `buildroot/board-esp32p4/` | everything that builds the guest userland |
| `buildroot/README.md` | **guest userland notes** — sizes, bFLT, the toolchain |
| `guest/` | prebuilt kernel and rootfs images, tracked so the board is flashable from a clean clone |
| `tools/hypmon.py` | the framed serial monitor TUI |
| `tools/hypvt.py` | the VT emulator it renders through |
| `native/` | a separate no-hypervisor Linux port; not the active line |
| `docs/HANDOFF.md` | **everything, for someone arriving cold** — start here |
| `docs/NOTES.md` | the same findings organised by subsystem |
| `docs/NATIVE-DRIVERS.md` | the plan for replacing emulated devices with real ones |
| `.claude/skills/run-esp32-p4-emulator/` | build/flash/drive procedures, all verified |

---

## Build, flash, run

Full detail is in `.claude/skills/run-esp32-p4-emulator/SKILL.md`; every
command there has been run. The short version:

**Monitor** (Windows PowerShell). `export.ps1` is broken in this install; dot
source the eim profile instead:

```powershell
. C:\Espressif\tools\Microsoft.v6.1-beta1.PowerShell_profile.ps1
idf.py build
```

**Guest userland** (WSL, buildroot at `~/buildroot-2026.05.1`). Note the board
directory is a **manual copy** of `buildroot/board-esp32p4/` — sync it first:

```bash
cp -a buildroot/board-esp32p4/. ~/buildroot-2026.05.1/board/esp32p4/
cd ~/buildroot-2026.05.1 && make -j3
python3 board/esp32p4/mkromfs.py output/target output/images/rootfs.romfs rootfs
python3 board/esp32p4/checkromfs.py output/images/rootfs.romfs output/target
```

`checkromfs.py` re-implements the kernel's romfs reader and diffs it against
the tree. It must print `VERDICT: OK` before you flash.

**Flash.** Close anything holding the serial port first — `idf.py flash` fails
on a busy port, the `parttool` steps afterwards succeed anyway, and the board
then runs the *previous* app with no sign anything went wrong.

```powershell
idf.py -p COM17 -b 921600 flash
parttool.py -p COM17 -b 921600 write_partition -n kernel --input guest\Image-romfs
parttool.py -p COM17 -b 921600 write_partition -n rootfs --input guest\rootfs.romfs
parttool.py -p COM17 -b 921600 write_partition -n dtb    --input dts\p4-phase6.dtb
```

**Drive it.** Scripted:

```powershell
python .claude\skills\run-esp32-p4-emulator\driver.py run --variant hypervisor "uname -a"
```

Interactive, with a framed TUI:

```powershell
python tools\hypmon.py
```

Login is `root` / `esp32p4`. Boot to login takes **about 2.5 s**, measured
from the DTR/RTS reset:

```
reset -> ESP-IDF app              1.03 s
ESP-IDF app -> kernel start       0.71 s
kernel start -> root mounted      0.26 s
root mounted -> init exec'd       0.00 s
init exec'd -> login prompt       0.51 s
                                  ------
                                  2.52 s   (min 2.50, max 2.53 over 2 runs)
```

It was 2.36 s before the SD card was added; the extra ~160 ms is card
initialisation on core 0 plus the guest's virtio-blk probe.

Measure it yourself with `python tools/boottime.py COM17 3` -- it times from
the reset rather than from kernel timestamps, which only start counting once
the kernel is already running and so hide the whole pre-Linux half.

Note that **nearly half the time is spent before Linux starts**: 1.02 s of ROM
and ESP-IDF init, then 0.67 s of hypervisor setup before the guest prints its
first line. The guest itself is quick -- 0.19 s from kernel start to a mounted
root, because romfs on memory-mapped flash needs no driver bring-up.

---

## Writing programs on the board

The guest carries `as`, `objcopy` and a linker written in Lua:

```
# cp /usr/share/hello-onboard.S /home && cd /home
# mkflt hello-onboard.S && ./hello-onboard
Hello from rv32 assembly, assembled and linked
on the ESP32-P4 itself ...
```

GNU `ld` is deliberately absent — it does not *run* here, for reasons in
`buildroot/README.md`. Write ordinary freestanding assembly; `lla`, `call`,
branches, `.data` and `.bss` all work. `/usr/share/stress.S` is the regression
test.

---

## Storage

The microSD card is a **virtio-mmio block device** the guest drives itself, so
`/dev/vda` is an ordinary Linux block device:

```
# mount -t ext2 /dev/vda1 /mnt
# echo hello > /mnt/hello.txt && sync
# cat /mnt/hello.txt        # still there after a power cycle
hello
```

Root stays romfs-on-flash, executed in place, so program text still costs no
RAM. The card is where writes go.

**Why virtio.** The guest kernel already has `CONFIG_VIRTIO_MMIO=y` and
`CONFIG_VIRTIO_BLK=y`, so the entire guest-side cost is a device-tree node --
`virtio@10001000` in `dts/p4-phase6.dts`. No out-of-tree driver, no kernel
patch, nothing to maintain. The device is emulated in `main/hyp_virtio_blk.c`
and reports Version 1 (legacy), so the guest uses the single-`QueuePFN` ring.

**Why the card is driven by ESP-IDF and not by Linux.** The native
(non-hypervisor) line of this project tried Linux's own `dw_mmc` driver on this
silicon and could not get a reliable filesystem out of it: raw block transfers
worked at any size, but the scattered multi-block transfers a filesystem
generates hung the controller reproducibly, and the watchdog then reset the
board. ESP-IDF's driver drives the same hardware and works. Borrowing it is
what a hypervisor is *for*.

**How a request crosses the cores.** The guest and every MMIO trap are on core
1, which has no FreeRTOS; ESP-IDF's SD driver blocks on semaphores and ISR
dispatch and can only run on core 0. So a queue notify on core 1 records a
kick, core 0's service task does the I/O and publishes the used ring, and core
1 turns that into a guest interrupt on its next device poll. `hyp_virtio_blk.c`
documents the handshake and the memory ordering it needs.

The service task runs at priority 0 and yields rather than sleeping: at
priority 0 it round-robins with the idle task, so the task watchdog still gets
fed, while a yield-based loop responds in microseconds. `vTaskDelay(1)` would
be 10 ms at `CONFIG_FREERTOS_HZ=100`, which would make every block request
glacial.

One caveat: the filesystem on the card was made by busybox `mkfs.ext2`, which
does not set the clean flag, so every mount prints `mounting unchecked fs`.
Harmless, and there is no `e2fsck` in the guest to silence it with.

---

## The monitor's source layout

`main.c` was 2,300 lines. It is now 90 -- core 0's service loop -- and the rest
is split by topic, with `main/hyp.h` carrying the contract between the modules
and the map of which file does what:

| file | what |
|---|---|
| `hyp_decode.c` | instruction fetch, RVC expansion, register access |
| `hyp_csr.c` | shadow CSR bank, `csr`/`mret`/`wfi` emulation |
| `hyp_clint.c` | emulated CLINT, the guest's timer |
| `hyp_plic.c` | emulated PLIC |
| `hyp_uart.c` | emulated 16550, the guest's console |
| `hyp_intr.c` | shadow `mip`, interrupt injection, host device servicing |
| `hyp_mmio.c` | device table and the load/store decoder |
| `hyp_trace.c` | trap history, statistics, panic |
| `hyp_core.c` | trap dispatch -- the entry point from `hyp_vectors.S` |
| `hyp_boot.c` | guest loading, PMP, core-1 bring-up |
| `hyp_sd.c` | the microSD card as raw sectors (core 0 only) |
| `hyp_virtio_blk.c` | the virtio-mmio block device |

A symbol earns a place in `hyp.h` only if more than one `.c` file uses it;
everything else stays `static`. Adding the block device meant adding one row to
`mmio_devices[]` -- the load/store decoder did not change, which is what that
table is for.

---

## State

Working and verified on hardware:

* Linux 6.8-rc1 boots to a shell in ~2.5 s, 29 MB of RAM.
* romfs root executed in place out of flash, so program text costs no RAM.
* Console over the emulated 16550, full-screen apps included (nano, less,
  ncurses). About 80 kB/s, roughly 1.56 traps per character.
* **Persistent writable storage on the microSD card**, as a virtio-mmio block
  device. See "Storage" below.
* On-board assembler, linker and packager.

Known gaps, all in `docs/HANDOFF.md`:

* `has_colors()` returns false in the guest and nothing explains it yet.
* Every emulated device costs traps. `docs/NATIVE-DRIVERS.md` is the plan for
  replacing them with real hardware, and the enabling fact is already
  established: **the guest can reach ESP32-P4 peripheral registers directly**.

---

## Conventions

* `guest/` images are tracked because nothing in this repo can rebuild them —
  they come out of the buildroot tree in WSL.
* `native/images/` and `native/linux-native/build/` are **not** tracked.
* Commit only when asked.
