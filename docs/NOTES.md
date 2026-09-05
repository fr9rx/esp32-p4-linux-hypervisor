# Notes

Things that cost real time to find, written down so they cost it once. Facts
here were measured on the board or read out of the source named, not inferred.

Guest *userland* notes — binary sizes, bFLT, the on-board toolchain, romfs —
live in `buildroot/README.md` instead. This file is the monitor, the boot path,
the build environment and the tooling.

---

## The monitor

### Core 1 is diverted through `__wrap_xPortStartScheduler`, not the weak hook

The obvious hook is `esp_startup_start_app_other_cores()` in `startup.c`. It
cannot be used: `freertos/app_startup.c` also defines it, **strongly**, so
overriding collides at link time as soon as that object is in the link — and it
is, because core 0 boots into FreeRTOS normally. `--wrap` does not help either,
because `--wrap` only redirects *undefined* references and `startup.c` both
defines the symbol weakly and calls it in the same object.

The symptom of getting this wrong is not a link error. The board boots, core 0
comes all the way up, the panel initialises, and core 1 quietly runs FreeRTOS
instead of the guest with nothing printed to say so.

`xPortStartScheduler()` is wrappable: it lives in the FreeRTOS port and is
called from `app_startup.c` and `tasks.c`, never from its own object. Both
cores pass through the wrap and it dispatches on core id.

### Core 1 must claim `port_uxCoreStartupDone` itself

`main_task()` blocks in `wait_for_all_cores_ready()` until every core sets its
flag, and the real `xPortStartScheduler()` is what normally sets it — which
core 1 no longer reaches. Without claiming it by hand the two cores deadlock:
core 0 in `wait_for_all_cores_ready()`, core 1 on `guest_image_ready`. The only
symptom is a boot that stops after `main_task: Started on CPU0`.

### Keep flash access out of FreeRTOS tasks on core 0

See the memory note of the same name. Core 0 runs FreeRTOS; core 1 is diverted
and never enters the scheduler.

### PMP layout

Set by IDF's `esp_cpu_configure_region_protection()`
(`components/esp_hw_support/port/esp32p4/cpu_region_protect.c`) and then by the
monitor. Entries resolve by **lowest matching index**.

| entry | region | set by | perms |
|---|---|---|---|
| 0–4 | IRAM/DRAM boundaries | IDF, locked | various |
| 5 | guest window `0x48000000` + 32 MB | **monitor**, `pmp_set_rwx_region()` | NAPOT RWX |
| 6 | flash cache `0x40000000`–`0x44000000` | IDF, locked | R+X |
| 15 (rev 1) / 27 (rev 3) | peripherals `0x50000000`–`0x50100000` | IDF, locked | **RW** |

Free entries on rev 1.0 are 5 and 7–10/12–14. The guest window has to be
entry **5** specifically and not one of the others, because entry 6 covers the
flash window and a higher-numbered entry can never override a lower one.

**PMP permissions apply to U-mode whether or not the L bit is set; the L bit
only extends them to M-mode.** So the locked peripheral entry grants the
*guest* read/write access to every ESP32-P4 peripheral register. That is the
enabling fact for native drivers — see `docs/NATIVE-DRIVERS.md`.

### The guest writes PMP CSRs and they have to be shadowed

`_start_kernel`'s "allow access to all memory" block writes `pmpaddr0 = -1`
then `pmpcfg0 = A_NAPOT|R|W|X`. Executing either for real would be fatal.
`main.c` shadows `pmpcfg0..3` and `pmpaddr0..15` so the reads back are
self-consistent, which is what the kernel checks.

### Emulated devices

| base | size | device |
|---|---|---|
| `0x02000000` | CLINT | `mtime`, `mtimecmp`, `msip` |
| `0x0C000000` | PLIC | one context, hart 0 M-external, `riscv,ndev = 31` |
| `0x10000000` | 16550 UART | the console |

Dispatch is a linear scan of `mmio_devices[]` on every access fault. The guest
console tops out at about **80 kB/s** against ~390 kB/s on the wire, at roughly
**1.56 traps per character** — a write to THR traps, and the status poll that
follows traps too.

### The DTB is patched at boot, by byte pattern

`esp_partition_mmap()` picks a free MMU slot, so the rootfs address is not
knowable at build time. The DTS carries a placeholder `reg` pair and
`patch_dtb_window()` finds those eight big-endian bytes in the blob and
overwrites them.

The pattern must appear **exactly once**. `guest_windows[]` is a table because
more than one window is possible; if you add a second, give it its own distinct
placeholder or the first node gets patched twice and the second is left
pointing into guest RAM.

---

## Flash and partitions

### A guest filesystem partition must be a power of two

`drivers/mtd/maps/physmap-core.c:521` rounds the window **down**:

```c
info->win_order = get_bitmask_order(resource_size(res)) - 1;
info->maps[i].size = BIT(info->win_order + ...);
```

A 12.375 MB partition becomes an 8 MB device. Everything upstream reports the
real span — the monitor prints `12672 KB`, physmap prints the full range — and
then `mtdblock0` is 8192 KB, `romfs_fill_super` refuses an image bigger than
its device, and the guest panics with `Unable to mount root fs on "mtd0"`.

So you cannot grow a filesystem by handing it the leftovers. More windows, not
a bigger one. A three-window layout (4 + 8 + 1 = 13 MB) was built and booted
and then removed once the thing that needed it (`ld`) turned out not to run.

### `parttool` writes by name, but a layout change means reflashing everything

`parttool.py write_partition -n rootfs` resolves the offset from the table on
the chip, so the names survive a layout change. The *contents* do not: if you
edit `partitions.csv`, `idf.py flash` (which rewrites the table) has to run
first and then **every** partition must be rewritten, because they have all
moved.

### Current layout

```
0x010000 + 0x060000 = 0x070000   factory   384 kB   app is 235,440   (61%)
         + 0x260000 = 0x2D0000   kernel   2.375 MB  Image is 2,247,408 (90%)
         + 0x010000 = 0x2E0000   dtb        64 kB
         + 0x800000 = 0xAE0000   rootfs      8 MB   image is ~6.0 MB  (72%)
                                 5.1 MB unallocated tail
```

Kernel headroom is the tightest number at 242,960 bytes. Crossing it shows up
as a `parttool` write that refuses the image, not as a boot failure.

---

## Build environment

### ESP-IDF activation

`export.ps1` is broken in this install. Dot-source the eim profile:

```powershell
. C:\Espressif\tools\Microsoft.v6.1-beta1.PowerShell_profile.ps1
```

### `sdkconfig` hand-edits need the `# default:` marker removed

IDF 6.1 silently reverts them otherwise.

### The buildroot board directory is a manual copy

Buildroot lives in WSL at `~/buildroot-2026.05.1`; `board/esp32p4/` there is a
**copy** of `buildroot/board-esp32p4/` in this repo, not a link. Every change
has to be synced before `make`, and shell scripts need their CRLF stripped:

```bash
cp -a buildroot/board-esp32p4/. ~/buildroot-2026.05.1/board/esp32p4/
sed -i 's/\r$//' ~/buildroot-2026.05.1/board/esp32p4/*.sh
chmod +x ~/buildroot-2026.05.1/board/esp32p4/*.sh
```

Forgetting this means building the previous version of whatever you just
edited, with no indication.

### `post-build.sh` edits `/etc/fstab` and `/etc/profile` in place, and is not idempotent

It runs against `output/target`, which persists between builds, so a `sed` that
matched last time will not match this time. If you change one of those edits,
restore the pristine file first:

```bash
cp package/skeleton-init-sysv/skeleton/etc/fstab  output/target/etc/fstab
cp package/busybox/inittab                        output/target/etc/inittab
cp system/skeleton/etc/profile                    output/target/etc/profile
```

`/etc/profile` from the skeleton contains a literal `@PATH@` that buildroot
substitutes during the skeleton install step, which will not re-run. Set it by
hand to `export PATH="/bin:/sbin:/usr/bin:/usr/sbin"` after restoring.

### USB DMA buffers must be 64-byte aligned

An unaligned buffer hangs the task rather than returning an error.

### Heredocs in this environment collapse `\\` to `\`

Verified:

```
$ cat <<'EOF'
test2: \\n
EOF
test2: \n
```

Quoted heredoc, still collapsed. Any file content with escapes written through
a heredoc is silently wrong — Python string literals where `"\\n"` became a
real newline, shell scripts whose `printf "%s\\n"` lost its format, `.ascii`
lines split across two lines, `\\`-continued shell lines joined into one.
Write such files with an editor tool, or build the backslash at runtime.

---

## Guest kernel

Linux **6.8-rc1**, `CONFIG_RISCV_M_MODE=y`, NOMMU, rv32imac.

Two source patches in `buildroot/board-esp32p4/patches/linux/`:

* **0001** sets `CONFIG_PAGE_OFFSET` to `0x48000000` for `!MMU`. Mainline
  hardcodes `0x80000000`, which is not where this board's PSRAM is. It has to
  be a source patch: `PAGE_OFFSET` is a promptless `hex` symbol, so a fragment
  or a hand-edited `.config` gets reverted.
* **0002** adds `ARCH_FORCE_MAX_ORDER` to `arch/riscv/Kconfig`, which riscv is
  one of the few architectures not to define. `linux.fragment` sets it to 11.
  Note `arch/riscv/Kconfig` indents with **spaces**, not tabs — a patch that
  uses tabs fails to apply.

### The per-exec allocation ceiling

`binfmt_flat` makes **one physically contiguous allocation per exec**, covering
`text + data + max(bss + stack, relocs * 4)`, page-rounded. There is no demand
paging: a binary is resident in full for the life of the process.

`MAX_PAGE_ORDER` defaults to 10, a 4 MB cap; 0002 raises it to 11 for 8 MB, and
`/proc/buddyinfo` grows from 11 columns to 12 to prove it.

**8 MB is what the allocator can express, not what it can deliver.** Measured
after boot:

```
Normal: ... 3*1024kB 2*2048kB 3*4096kB 0*8192kB = 25836kB
```

25 MB free, largest block 4 MB. An 8 MB block on a 29 MB machine only exists
while nothing is churning. **Treat 4 MB as the number you can rely on.** That
is what killed GNU `ld` (5,283,840 bytes) — it segfaults on exec, every time.

An earlier note here claimed the ceiling was 512 kB because the free lists
never held anything bigger. That was wrong, and `/proc/buddyinfo` disproves it:
four free order-10 blocks at boot and still four after real work. Ordinary use
churns the low orders and leaves the top of the allocator alone.

`board/esp32p4/checkflat.sh` enforces the ceiling at build time and must be
changed together with `linux.fragment`.

---

## Guest userland gotchas

### This busybox `mount` passes the option string straight to the filesystem

The skeleton fstab's `defaults` is not a filesystem option, and the kernel
rejects the whole mount — which is why `/sys` was not mounted at all for a long
time. Measured:

```
mount -t sysfs sysfs /sys                        -> ok
mount -t sysfs -o rw sysfs /sys                  -> ok
mount -t sysfs -o nosuid,nodev,noexec sysfs /sys -> sysfs: Unknown parameter 'nosuid'
```

The VFS flags fail exactly like `defaults` does. The option field may contain
real filesystem options and nothing else.

The skeleton inittab also has `::sysinit:/bin/mount -o remount,rw /`, which can
never succeed on a read-only romfs root and produces
`romfs: Unknown parameter 'relatime'` as the first thing the guest ever prints.
`post-build.sh` deletes that line.

### busybox `ash` exits a `set -e` script when a `while` condition fails

```
+ set -e
+ '[' 1 -gt 0 ']'
+ src=hello-onboard.S
+ shift
+ '[' 0 -gt 0 ']'
<script ends, silently, status 1>
```

Nothing after the loop runs. dash does not do this. Use
`while :; do [ $# -gt 0 ] || break; ...` — the test in a `||` list is exempt
from errexit.

### `has_colors()` returns false — unexplained

`nctest` prints `colors : 0` in the guest. Terminfo is present and the terminal
is capable. Nobody has worked out why. **Open.**

---

## The serial tooling

`tools/hypmon.py` is a framed TUI over the console; `tools/hypvt.py` is the VT
emulator it renders through; both have test suites beside them.

Findings that only showed up against real board output:

* **REP (`ESC[<n>b`)** — ncurses draws horizontal rules as one character plus a
  repeat count. Without REP a 77-column box renders as three characters.
* **DEC special graphics (`ESC(0`, SO/SI)** — box frames arrive as
  `lqkxmjtuvwn`.
* **IRM (`CSI 4h/4l`)** — insert mode, needed or the unknown-sequence count is
  never zero.
* **`nano` enters the alternate screen buffer** under `TERM=xterm-256color` and
  sets its own scroll region 43 times, defeating any DECSTBM fence the monitor
  sets. What actually protects the status row is answering the cursor-position
  report with `rows - 1`.
* Under the alternate screen the scrollback holds only scrolled-off lines, so
  the still-visible shell output has to be read from the parked primary grid or
  the pane goes blank while nano runs.

The monitor prefix is `Ctrl+]` — two presses, not a control sequence you type
literally.

---

## Open questions

* `has_colors()` / `nctest colors : 0`.
* No persistent writable storage. `/home` is tmpfs; persistence would need the
  monitor to write back to a flash partition, which it currently only reads.
* `hypmon.py`'s interactive layer has never been driven by a human, only by
  rendering captured board output.
* Clean-tree reproducibility of the buildroot build is unverified.
* `native/` (the no-hypervisor port) is present but not the active line.
