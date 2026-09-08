# esp32-p4-linux-hypervisor

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
| `main/main.c` | the monitor: traps, device emulation, PMP, guest load |
| `main/hyp_vectors.S` | trap entry/exit |
| `main/hyp_kbd.c` | USB keyboard, decoded on core 0 into the 16550 RX ring |
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

Login is `root` / `esp32p4`. Boot to login takes **about 2.4 s**, measured
from the DTR/RTS reset:

```
reset -> ESP-IDF app              1.02 s
ESP-IDF app -> kernel start       0.67 s
kernel start -> root mounted      0.19 s
root mounted -> init exec'd       0.00 s
init exec'd -> login prompt       0.51 s
                                  ------
                                  2.36 s   (min 2.33, max 2.38 over 3 runs)
```

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

## State

Working and verified on hardware:

* Linux 6.8-rc1 boots to a shell in ~2.4 s, 29 MB of RAM.
* romfs root executed in place out of flash, so program text costs no RAM.
* Console over the emulated 16550, full-screen apps included (nano, less,
  ncurses). About 80 kB/s, roughly 1.56 traps per character.
* USB keyboard input, decoded on core 0.
* On-board assembler, linker and packager.

Known gaps, all in `docs/HANDOFF.md`:

* `has_colors()` returns false in the guest and nothing explains it yet.
* No persistent writable storage; `/home` is tmpfs.
* Every emulated device costs traps. `docs/NATIVE-DRIVERS.md` is the plan for
  replacing them with real hardware, and the enabling fact is already
  established: **the guest can reach ESP32-P4 peripheral registers directly**.

---

## Conventions

* `guest/` images are tracked because nothing in this repo can rebuild them —
  they come out of the buildroot tree in WSL.
* `native/images/` and `native/linux-native/build/` are **not** tracked.
* Commit only when asked.
