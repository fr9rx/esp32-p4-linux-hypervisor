# Handoff

Everything known about this project, written for someone — or something —
arriving with no context. Facts here were measured on the board or read out of
the source file named. Where something is inference rather than measurement it
says so.

Read this first, then `docs/NOTES.md` (same material, organised by subsystem),
`buildroot/README.md` (guest userland in depth) and
`docs/NATIVE-DRIVERS.md` (the next piece of work).

---

# 1. What this is

A hand-written **M-mode hypervisor** for the ESP32-P4 that boots **NOMMU Linux
6.8-rc1 as a U-mode guest** by trap-and-emulate.

The P4 is a dual-core RISC-V **rv32imac** with:

* **no MMU**
* **no S-mode** — M-mode and U-mode only
* **no hypervisor extension**

There is no virtualisation hardware. The monitor runs in M-mode on core 1, puts
Linux in U-mode, and emulates the devices Linux insists on by catching the
access faults its loads and stores take. The guest believes it is running in
M-mode on a machine with a CLINT, a PLIC and a 16550; every one of those is a
fiction maintained by the trap handler.

Core 0 keeps running FreeRTOS and ESP-IDF normally, so the board still has USB
host, a panel driver and the rest of IDF.

```
core 0                            core 1
  FreeRTOS + ESP-IDF                M-mode monitor (main/main.c)
  USB keyboard -> 16550 RX ring       |  trap-and-emulate
  owns the physical UART0             v
  prints the trap histogram         U-mode: Linux 6.8-rc1, NOMMU, rv32imac
                                            romfs root, XIP from flash
                                            29 MB usable RAM
```

**The user wrote the original baseline.** Work described here continued from
commit `dae3ffb`.

---

# 2. Ground rules for working in this environment

These are not preferences, they are things that will waste your time if you do
not know them.

### 2.1 Host environment

| | |
|---|---|
| OS | Windows 11, PowerShell primary; a Bash tool is also available (Git Bash) |
| IDF | v6.1-beta1 at `C:\esp\v6.1-beta1\esp-idf`, tools at `C:\Espressif\tools` |
| buildroot | **2026.05.1, inside WSL Ubuntu** at `~/buildroot-2026.05.1` |
| board | ESP32-P4, CP210x USB-serial, normally `COM17` |
| flash baud | 921600 for `esptool`/`parttool` |
| **console baud** | **4 Mbps** (4000000), not 115200 |

### 2.2 ESP-IDF activation is broken; use the eim profile

`export.ps1` does not work in this install. Every PowerShell command that needs
IDF must start with:

```powershell
. C:\Espressif\tools\Microsoft.v6.1-beta1.PowerShell_profile.ps1
```

### 2.3 `sdkconfig` hand-edits need the `# default:` marker removed

IDF 6.1 (esp-idf-kconfig 3.12) reads `sdkconfig` as a *defaults* file and marks
every line it generated with a preceding `# default:` comment
(`esp_kconfiglib/core.py:1385`). A hand-edit that leaves the marker in place is
treated as a default, loses to the Kconfig default, and is **silently
rewritten**. Drop the marker to make the line user-set.
`.claude/skills/run-esp32-p4-emulator/setbaud.py` does this correctly for the
baud symbols and is the model to copy.

Always re-grep `sdkconfig` after a build to confirm a value took.

### 2.4 Heredocs in this environment collapse `\\` to `\`

**This one has cost more time than any other single thing.** Verified:

```
$ cat <<'EOF'
test1: \n
test2: \\n
EOF
test1: \n
test2: \n
```

A *quoted* heredoc, still collapsed. Consequences seen repeatedly:

* Python string literals where `"\\n"` became a real newline, so
  `str.replace()` silently matched nothing and reported success.
* Shell scripts whose `printf "%s\\n"` lost its format string and emitted a
  literal newline in the middle of a variable.
* `.ascii "text\\n"` assembly lines split across two source lines.
* `\\`-continued shell lines joined into one.

**Workaround:** write any file containing backslashes with an editor/Write
tool, not a heredoc. If you must go through a shell, write a helper script to
a file first and then run it, or build the backslash at runtime — e.g. in
Python `BS = chr(92)` with an `@NL@` placeholder replaced by `BS + "n"`.

Failures are quiet. Assume nothing worked until you `cat -A` the result.

### 2.5 WSL memory and parallelism

`%USERPROFILE%\.wslconfig` limits WSL memory; the GCC cross-toolchain build
needs it. **Build with `-j3`, not `-j8`** — higher parallelism OOMs.

### 2.6 The buildroot board directory is a manual copy

`~/buildroot-2026.05.1/board/esp32p4/` is a **copy** of
`buildroot/board-esp32p4/` in this repo, not a symlink. Every change must be
synced, and CRLF must be stripped from shell scripts:

```bash
cp -a buildroot/board-esp32p4/. ~/buildroot-2026.05.1/board/esp32p4/
sed -i 's/\r$//' ~/buildroot-2026.05.1/board/esp32p4/*.sh
chmod +x ~/buildroot-2026.05.1/board/esp32p4/*.sh
```

Forgetting this builds the *previous* version of whatever you just edited, with
no indication whatsoever.

### 2.7 `post-build.sh` is not idempotent

It edits `output/target/etc/{fstab,inittab,profile}` in place, and
`output/target` persists between builds. A `sed` that matched last time will
not match this time. If you change one of those edits, restore the pristine
file first:

```bash
cd ~/buildroot-2026.05.1
cp package/skeleton-init-sysv/skeleton/etc/fstab output/target/etc/fstab
cp package/busybox/inittab                       output/target/etc/inittab
cp system/skeleton/etc/profile                   output/target/etc/profile
sed -i 's|^export PATH=@PATH@$|export PATH="/bin:/sbin:/usr/bin:/usr/sbin"|' \
    output/target/etc/profile
```

That last line matters: the skeleton `/etc/profile` contains a literal
`@PATH@` that buildroot substitutes during the skeleton *install* step, which
will not re-run on an incremental build.

### 2.8 Close the serial port before flashing

`idf.py flash` fails with `Could not open COM17, the port is busy`, **the
`parttool` steps afterwards reconnect happily**, and the board then runs the
*previous* app with no sign anything went wrong. That cost an hour of testing a
stale binary once.

```powershell
Get-Process python | Where-Object { $_.Id -ne $PID } | Stop-Process -Force
```

### 2.9 Working agreements the user has set

* **Announce exactly what will change in which file before editing.**
* **Do not call the Agent/subagent tool.**
* **Do not use workflows or deep-research unless asked.**
* **Commit only when asked.**

---

# 3. Repo map

| path | what |
|---|---|
| `main/main.c` | the monitor — 2,306 lines, heavily commented, the primary source of truth |
| `main/hyp_vectors.S` | trap entry/exit (140 lines) |
| `main/hyp_kbd.c/.h` | USB keyboard, decoded on core 0 into the 16550 RX ring (463 lines) |
| `main/linker.lf` | puts the whole monitor in IRAM — see §4.9 |
| `dts/p4-phase6.dts/.dtb` | the guest device tree, **the live one** |
| `dts/p4-phase{1,3,4,5}.dts` | earlier phases, kept for reference |
| `partitions.csv` | flash layout; read the comments before touching |
| `sdkconfig`, `sdkconfig.default` | IDF config |
| `buildroot/board-esp32p4/` | everything that builds the guest userland |
| `buildroot/README.md` | **guest userland notes, 810 lines** |
| `guest/` | prebuilt kernel + rootfs images, tracked (nothing in this repo can rebuild them) |
| `tools/hypmon.py` | framed serial monitor TUI (1,267 lines) |
| `tools/hypvt.py` | the VT emulator it renders through (794 lines) |
| `tools/test_hypmon.py`, `test_hypvt.py` | their test suites |
| `components/usb/` | vendored IDF USB pieces CherryUSB needs |
| `native/` | a **separate** no-hypervisor Linux port; not the active line |
| `.claude/skills/run-esp32-p4-emulator/` | verified build/flash/drive procedures + `driver.py` |
| `docs/NOTES.md` | findings by subsystem |
| `docs/NATIVE-DRIVERS.md` | the next piece of work |

Not tracked: `native/linux-native/build/` (248 MB), `native/images/` (11 MB),
`build/`, `managed_components/`, `.cache/`, `*.log`.

---

# 4. The monitor

`main/main.c` is the authority and is written to be read. This section is a map
of it plus the findings that are not obvious from the code.

### 4.1 How core 1 is diverted into the guest

**Via `__wrap_xPortStartScheduler()`. Not via the weak hook.**

The obvious hook is `esp_startup_start_app_other_cores()` in `startup.c`. It
**cannot** be used: `freertos/app_startup.c` also defines it **strongly**, so
overriding collides at link time as soon as that object is in the link — and it
is, because core 0 boots into FreeRTOS normally. `--wrap` does not help either,
because `--wrap` only redirects *undefined* references, and `startup.c` both
defines the symbol weakly and calls it in the same object.

The failure mode is not a link error. The board boots, core 0 comes all the way
up, the panel initialises, **and core 1 quietly runs FreeRTOS instead of the
guest with nothing printed to say so.**

`xPortStartScheduler()` is wrappable: it lives in the FreeRTOS port and is
called from `app_startup.c` and `tasks.c`, never from its own object. Both
cores pass through the wrap; it dispatches on core id.

### 4.2 Core 1 must claim `port_uxCoreStartupDone` itself

`main_task()` blocks in `wait_for_all_cores_ready()` until every core sets its
flag, and the real `xPortStartScheduler()` is what normally sets it — which
core 1 never reaches. Without claiming it by hand the cores deadlock: core 0 in
`wait_for_all_cores_ready()`, core 1 on `guest_image_ready`. The only symptom
is a boot that stops after `main_task: Started on CPU0`.

Safe to claim only *after* `__wrap_xPortStartScheduler()` has moved the core
onto `hyp_boot_stack`.

### 4.3 PMP

Set by IDF's `esp_cpu_configure_region_protection()`
(`components/esp_hw_support/port/esp32p4/cpu_region_protect.c`), then by the
monitor. **Entries resolve by lowest matching index.**

| entry | region | by | perms |
|---|---|---|---|
| 0–4 | IRAM/DRAM boundaries | IDF, locked | various |
| **5** | guest window `0x48000000` + 32 MB | **monitor**, `pmp_set_rwx_region()` | NAPOT RWX |
| 6 | flash cache `0x40000000`–`0x44000000` | IDF, locked | R+X |
| **15** (rev 1) / **27** (rev 3) | peripherals `0x50000000`–`0x50100000` | IDF, locked | **RW** |

Free entries on rev 1.0: 5 and 7–10/12–14. The guest window **must** be entry 5
and not one of the others, because entry 6 covers the flash window and a
higher-numbered entry can never override a lower one.

**PMP permissions apply to U-mode whether or not the L bit is set; L only
extends them to M-mode.** So the locked peripheral entry grants the *guest*
read/write over every ESP32-P4 peripheral register. This is the enabling fact
for native drivers (§10). It is static analysis of IDF's source, **not yet
measured on the chip.**

The rootfs mapping lands in the flash cache window, which entry 6 already
covers R+X — that is why romfs XIP works with no new PMP entry, and it matters
because entry 6 outranks every free entry.

### 4.4 Shadow CSRs

The guest believes it owns the machine CSRs. It does not. `main.c` keeps a
shadow bank for `mstatus`, `mie`, `mip`, `mtvec`, `mepc`, `mcause`, `mtval`,
`mscratch`, `pmpcfg0..3`, `pmpaddr0..15` and friends.

This must exist before any trap can be handed back to the guest. Concretely:
Linux's `_start_kernel` "allow access to all memory" block does

```
csrw pmpaddr0, -1
csrw pmpcfg0,  A_NAPOT|R|W|X
```

Executing either for real would be fatal — a guest `pmpcfg0` outranks the
monitor's entry 5 by index and would hand the guest the monitor's own memory.
Real hardware masks unimplemented `pmpaddr` bits on read-back and this kernel
checks that, so the shadow has to be self-consistent, not just present.

A missing CSR read makes the following `csrr` yield 0 and the dependent store
go to address 0x8 — which is why the shadow bank has to be complete rather than
best-effort.

Note this kernel dispatches syscalls from `excp_vect_table[8]` and **requires
`MPP == 0`**; `excp_vect_table[11]` is "Oops - environment call from M-mode".

### 4.5 Emulated devices

Dispatched by a linear scan of `mmio_devices[]` on every access fault.

| base | size | device | notes |
|---|---|---|---|
| `0x02000000` | `0x10000` | **CLINT** | `mtime`, `mtimecmp`, `msip` |
| `0x0C000000` | `0x400000` | **PLIC** | one context: hart 0, M-external. `riscv,ndev = 31`. UART is source 10 |
| `0x10000000` | `0x100` | **16550 UART** | the console. `reg-shift = 0`, `reg-io-width = 1` |

**CLINT/timer.** Backed by SYSTIMER, a 52-bit counter at a fixed 16 MHz
(XTAL 40 MHz / 2.5), which is exactly the `timebase-frequency` the DTS
declares — nothing rescales anywhere. Uses **counter 0, alarm 1**.

* Alarm **1**, not 0: alarm 0 is `SYSTIMER_ALARM_OS_TICK_CORE0`, and now that
  core 0 boots FreeRTOS normally the port layer reprograms it 100 times a
  second, which would retarget the guest's clock. Alarm 1 is
  `SYSTIMER_ALARM_OS_TICK_CORE1` and core 1 is the one core that never runs an
  RTOS. Alarm 2 belongs to `esp_timer`.
* Re-armed on **every** `mtimecmp` word write, not just the final one — Linux's
  RV32 sequence writes the two halves separately.
  `SYSTIMER_LL_ALARM_MISS_COMPENSATE == 1` on this chip, so a target already in
  the past still fires.
* `mtip_pending()` is **computed, never latched**. A latched `shadow.mip`
  drops a tick.

**PLIC.** Modelled against `drivers/irqchip/irq-sifive-plic.c`. The `claimed`
mask is what makes level-triggered sources safe; without it a level source
re-asserts immediately and the guest livelocks.

**16550.** Linux reaches it first through
`earlycon=uart8250,mmio,0x10000000` (polled), then via `serial8250`.
Full-duplex; TX interrupt raised when `IER.ETBEI` is set. RX comes from a ring
fed by core 0 (physical UART **and** USB keyboard).

### 4.6 Host interrupts owned by the monitor

Only two, and they exist for exactly one reason: the monitor owns `mtvec`, so
nothing else can force control out of a guest loop that takes no traps. **They
do not perform injection — they only manufacture a trap.**

```c
#define HYP_INUM_TIMER  20
#define HYP_INUM_UART   21
#define HYP_INT_PRIO    4      /* NOT 0 */
```

inum 20 and 21 are free: `soc.h` reserves 24–28 (T1_WDT, CACHEERR,
MEMPROT_ERR, ASSIST_DEBUG, IPC_ISR) and 0 is `ETS_INVALID_INUM`.

**CLIC `mcause` encoding was measured, not read:** routing inum 20 and 21 and
printing the raw code gave 36, i.e. `inum + 16`. Codes 0–15 are the
architectural local interrupts (MSI 3, MTI 7, MEI 11).

Both sources are **level-triggered**; failing to clear the device status
re-enters immediately.

### 4.7 `hyp_clic_sanitize()` is not optional hygiene

`esp_cache_err_int_init()` routes `ETS_CACHE_INTR_SOURCE` on core 1. With the
monitor owning `mtvec` and a U-mode guest owning `sp`, taking that interrupt
faults **inside the trap prologue** and nests until the core dies. The CLIC
control block is per-core behind a current/other aliasing scheme, so this has
to run on core 1 itself.

Core 1 also clears `mstatus.MIE` before its own bring-up, because IDF's
`call_start_cpu1()` leaves a live cache-error interrupt pointing into
`_mtvt_table` and `mtvec` is about to change.

### 4.8 The I-cache coherency sledgehammer

**This cost a boot to find and the signature is worth memorising.**

Linux writes code and calls `flush_icache_range()`, which on riscv is
`fence.i`. That invalidates L1 I but does **not** write back L1 D, and on this
core the instruction fetch does not see dirty D-cache lines. So freshly written
instructions sit in L1 D while the fetch reads the stale line underneath.

It showed up when Linux delivered a signal to init by writing a two-instruction
sigreturn trampoline **onto the user stack** and `mret`-ing to it. The fetch got
the stack slot's previous contents, which decoded as a load, and the guest died
on a load access fault at `0x7a` whose `mepc` pointed at `li a7, 139` — an
instruction that cannot fault that way.

> **The signature of this class of bug is a mismatch between the executed word
> and the word in memory.** If `mepc` points at an instruction that cannot
> produce the fault you are seeing, suspect cache coherency.

`fence.i` is opcode `0x0f`, a legal instruction, so it does not trap and cannot
be hooked. `hyp_sync_icache()` therefore runs
`cache_ll_l1_writeback_dcache_all()` + `cache_ll_l1_invalidate_icache_all()` on
**every** `mret`. L2 is not touched — the fetch reaches it, so data need not go
out to PSRAM.

This is a sledgehammer kept to prove the diagnosis. **The intended replacement
is a hypercall in the guest's `local_flush_icache_all()`**, doing it once per
actual flush instead of once per trap.

### 4.9 The whole monitor lives in IRAM (`main/linker.lf`)

`noflash` maps `libmain.a`'s `.text` to `iram0_text` and `.rodata` to
`dram0_data`.

**This is a correctness fix as much as a speed one.** Core 1's trap handler
used to live in flash. Any flash operation on core 0 disables the cache, and
IDF's answer is to IPC the other core into an IRAM spin
(`spi_flash_op_block_func`). Core 1 does not run FreeRTOS and cannot be parked
that way, so a trap taken during a flash write would have fetched its handler
through a disabled cache. Nothing writes flash at runtime *today*, which is the
only reason it never fired. **It is a landmine, not a design.**

The speed half: 345,906 traps in a 45-second session at 542 cycles each, every
one previously subject to flash-cache misses.

Corollary, and it is in the project memory: **keep flash access out of
FreeRTOS tasks on core 0.**

### 4.10 `wfi` — one trap in six was pure waste

The gate used to include `(shadow.mstatus & MSTATUS_MIE)`, assuming Linux idles
with interrupts enabled. The histogram said otherwise: **1,903,285 `wfi` traps
spun and ZERO blocked.** The guest reaches `wfi` with its own gate shut and
relies on the hardware waking it anyway — the RISC-V spec is explicit that
`wfi`'s wake condition **ignores `mstatus.MIE`**.

Now the monitor blocks in a real `wfi`. It is inside a trap so `mstatus.MIE`
is 0, and the CLIC wakes the core **without vectoring** — execution resumes at
the next instruction, polls, and decides. No re-entrancy, no nesting.

What is still required before blocking is that some host interrupt can actually
arrive: a timer the guest disarmed (`mtimecmp` at ~0) **plus** a PLIC context
it has not enabled means nothing will ever fire, and a `wfi` there would be an
unrecoverable hang with no diagnostic. Escape hatch: `#define HYP_WFI_BLOCKS 0`
returns to spinning.

**Accounting trap:** a blocking `wfi` is time in the handler, so the first
histogram after `wfi` started working reported 27,291 cycles per
illegal-instruction trap — the guest *sleeping*, not the monitor working.
`hyp_idle_cycles` now subtracts it. A cost metric that goes up when you make
the system faster is worse than no metric.

### 4.11 UART TX does not block, and the first attempt at that did nothing

Store access faults — almost all UART register writes — cost 12,000+ cycles
each and were most of the time spent in the monitor. Those cycles were the
guest waiting for a byte to clear the FIFO.

The first fix looked right and measured as nothing:

```c
while (esp_rom_output_tx_one_char(value) != 0) { }   /* loop never iterated */
```

**The ROM function spins on the FIFO level internally and then always reports
success.** There is no "try once" mode. The only way not to wait is not to call
it — so the FIFO is written directly and the byte is dropped when full.

What that costs is a console log with holes during a burst. Measured
`tx dropped` per boot: **3392 at 115200, 61 at 2 Mbps, 0 at 3 Mbps and above.**
The console runs at 4 Mbps, so it is currently 0. Drop the baud and the holes
come back; the counter in the histogram is how you find out.

`HYP_UART_LOG_BLOCKING 1` restores faithful capture. The monitor's own
`esp_rom_printf()` from core 0 still blocks either way, so `I:`/`W:`/`E:` lines
and the histogram are always complete.

### 4.12 Instruction fetch and decode

* **Two halfword reads, never one word read.** `mepc` is only 2-byte aligned
  whenever the previous instruction was compressed.
* Length lives in the low bits of the first halfword.
* `rvc_expand()` handles **only `c.lw` and `c.sw`**, and that is not a
  shortcut — disassembling the kernel showed those are the only compressed
  forms that reach MMIO. **Verified 90/90 against `riscv32-esp-elf-as`** across
  3 destination registers × 3 base registers × 10 offsets.
* Expansion rewrites `encoding` but never `length`.

### 4.13 Guest load and DTB patching

* Kernel and DTB are `memcpy`'d from flash into PSRAM
  (`partition_cp_to_psram`). The split of who does what is forced by
  `spi_flash`'s cross-core handshake.
* Guest RAM: `0x48000000`, 32 MB granted by PMP; `memory{}` node declares
  31 MB; the DTB sits at `0x49F00000` outside it; Linux ends up with ~29 MB
  usable.
* **The rootfs is not copied.** The partition stays `esp_partition_mmap`'d for
  the life of the board and the DTB's `mtd-rom` `reg` is patched to wherever
  the mapping landed. Three properties of the chip make this work:
  * the mapping lands in the flash cache window, which PMP entry 6 already
    grants R+X to U-mode;
  * physmap-core's `ioremap` is the identity on NOMMU;
  * `romfs_get_unmapped_area` → `mtd_get_unmapped_area` hands bFLT text pages
    straight out of that window, so **program text is XIP from flash and never
    occupies RAM**.

  Cost: flash is slower than PSRAM. Benefit: 2 MB of guest RAM back and no size
  cap below the partition size.
* `patch_dtb_window()` finds the placeholder by **byte pattern** — the
  big-endian `<address size>` pair — rather than walking the FDT. It is simpler
  and self-checking: a DTS edit that changes the placeholder makes it fail
  loudly instead of writing to the wrong offset. **The pattern must appear
  exactly once.** `guest_windows[]` is a table because more than one window is
  possible; give each its own distinct placeholder.
* `patch_guest_cmdline()` rewrites the kernel's built-in command line in place.
  `CONFIG_CMDLINE_FORCE=y` so the DTB's `bootargs` is ignored. The field is 73
  chars + 3 NUL, so a replacement of up to 75 fits. It is **searched, not
  hardcoded** — the offset is specific to a kernel build. Only the older
  initramfs kernel needs it; on the romfs kernel the needle is absent and the
  patch is a no-op.

### 4.14 Diagnostics

* **Trap histogram** — 48 causes, cycles per cause, MMIO counts per device,
  `tx dropped`, `wfi spins`, `icache syncs`. Printed by pressing **`Ctrl-^`**
  (0x1e) on the physical console. Cost per trap: two cycle-counter reads and
  two adds.
* **Trace ring** — four stores per trap; dumped only on an injected access
  fault. It exists because of a bug where `mtval` and `mepc` disagreed.
* **`Ctrl-_`** (0x1f) types `kbd-ok` through the keyboard decoder, so the input
  path can be checked without a human at the keyboard.

Do **not** print from inside the trap handler — it puts hundreds of blocking
UART cycles in the hottest path on the machine.

---

# 5. The guest kernel

**Linux 6.8-rc1**, `CONFIG_RISCV_M_MODE=y`, NOMMU, rv32imac.

Two source patches in `buildroot/board-esp32p4/patches/linux/`:

**0001 — `CONFIG_PAGE_OFFSET = 0x48000000` for `!MMU`.** Mainline hardcodes
`0x80000000`, which is not where this board's PSRAM is. **It has to be a source
patch**: `PAGE_OFFSET` is a promptless `hex` symbol, so a Kconfig fragment or a
hand-edited `.config` gets reverted.

**0002 — adds `ARCH_FORCE_MAX_ORDER` to `arch/riscv/Kconfig`.** riscv is one of
the few architectures that does not define it (arc/arm/arm64/powerpc do).
`linux.fragment` then sets `CONFIG_ARCH_FORCE_MAX_ORDER=11`.

> **`arch/riscv/Kconfig` indents with SPACES, not tabs.** A patch that uses
> tabs fails to apply at that hunk. Generate such patches programmatically to
> control exact whitespace.

### 5.1 The per-exec allocation ceiling — the single most important guest fact

`binfmt_flat` makes **one physically contiguous allocation per exec**, covering
`text + data + max(bss + stack, relocs * 4)`, page-rounded. **There is no
demand paging** — a binary is resident in full for the life of the process
whether it touches all of itself or not. NOMMU also cannot compact (compaction
needs MMU page migration).

`MAX_PAGE_ORDER` defaults to 10 → a 4 MB cap. Patch 0002 raises it to 11 → 8 MB
cap. Verified: `/proc/buddyinfo` grew from 11 columns to 12.

**8 MB is what the allocator can express, not what it can deliver.** Measured
after boot:

```
Normal: 19*4kB 8*8kB 12*16kB 11*32kB 11*64kB 11*128kB 8*256kB
        3*512kB 3*1024kB 2*2048kB 3*4096kB 0*8192kB = 25836kB
```

25 MB free, largest block 4 MB. An 8 MB block on a 29 MB machine only exists
while nothing is churning. **Treat 4 MB as the number you can rely on.**

> An earlier note in this project claimed the ceiling was 512 kB because the
> free lists never held anything bigger. **That was wrong.** `/proc/buddyinfo`
> shows four free order-10 blocks at boot and still four after real work
> (`ls -R /`, `dmesg`, `grep`, `tar`, a 50,000-element Lua table, an editor).
> Ordinary use churns the *low* orders and leaves the top of the allocator
> alone. The old figure needlessly excluded GNU gzip, bzip2recover and others.

Raising the order further is possible but not free: `pageblock_order` follows
`MAX_PAGE_ORDER` when huge pages are not in use, so on a 29 MB machine an 8 MB
pageblock is already a quarter of memory and anti-fragmentation grouping stops
meaning much.

`board/esp32p4/checkflat.sh` recomputes the allocation for every bFLT binary,
**fails the build** over 8,388,608, and prints a BIG note over 1,048,576. It
must be changed together with `linux.fragment`. It has already caught GNU
`gzip` (1,003,520) and `bzip2recover` (1,662,976). `file` asks for 7.3 MB in
one allocation regardless of database size and is excluded.

---

# 6. The guest userland

Full detail in `buildroot/README.md`. Highlights:

* romfs, read-only, XIP from flash. `/home` is tmpfs and does not survive a
  reboot. **There is no persistent writable storage.**
* busybox, plus a second "busybox-extra" build — the two exist because of the
  per-exec ceiling; more binaries is the answer, each gets its own block.
* lua 5.4 (`LUA_32BITS`, so 32-bit integers and a 32-bit float number type),
  nano, less, ncurses, bc/dc.
* `mkromfs.py` writes the image directly — buildroot 2026.05 removed romfs
  support and there is no `genromfs`. `checkromfs.py` re-implements the
  kernel's reader and diffs it against the tree; **it must print
  `VERDICT: OK` before you flash.**
* Login: `root` / `esp32p4` (the `native` variant is `root` with an empty
  password). Boot to login ≈ **25 s** (native ≈ 12 s).

### 6.1 The on-board toolchain

The guest can assemble, link and run its own programs:

```
# cp /usr/share/hello-onboard.S /home && cd /home
# mkflt hello-onboard.S && ./hello-onboard
```

`as` + `objcopy` + `/usr/lib/mkflt.lua` — a linker written in Lua (~500 lines).
It lays the allocatable sections out end to end, gives symbols addresses,
applies the rv32 relocations and writes the bFLT header, calling `objcopy -O
binary --only-section=NAME` for each section's bytes.

Handled relocations: `PCREL_HI20`/`PCREL_LO12_I`/`PCREL_LO12_S` (so `lla`
works), `HI20`/`LO12_I`/`LO12_S`, `BRANCH`, `JAL`, `CALL`/`CALL_PLT`,
`ADD32`/`SUB32`/`SET32`; `RELAX`/`NONE` ignored. **Anything else is refused by
name** — a hand-rolled linker must never emit a zero and let the program jump
to it. `/usr/share/stress.S` is the regression test.

### 6.2 Why `ld` and `elf2flt` are not on the board

**`ld` does not run.** It is 5,283,840 bytes, `binfmt_flat` needs that
contiguous, and the allocator never has it:

```
nommu: Allocation of length 5283840 from process 36 (ld) failed
Normal: ... 3*1024kB 2*2048kB 3*4096kB 0*8192kB = 25400kB
binfmt_flat: Unable to allocate RAM for process text/data, errno -12
Segmentation fault
```

Every time, not intermittently. `echo 3 > /proc/sys/vm/drop_caches` does not
help — the page cache is 1.7 MB and lives in low orders.

`elf2flt` converts `ld`'s output, so it went too. Both were built, shipped,
verified present, and removed. The three-window partition layout (4 + 8 + 1 =
13 MB) existed to carry them and was removed with them.

### 6.3 Relocation-free rv32 assembly is not possible

The first design tried to skip the linker with `objcopy -O binary` on an
unlinked object and self-computed addresses:

```
1:  auipc a1, 0
    addi  a1, a1, msg - 1b
```

rv32 gas rejects that and **every** spelling of it — inline, hoisted into
`.equ`, wrapped in `%lo()`, with `.balign` removed so no alignment intervenes,
and with relaxation disabled at both assembly and link time:

```
Error: illegal operands `addi a1,a1,msg-1b'
```

Only literal constants are accepted as an immediate. So every address-of leaves
a relocation and something must apply it.

### 6.4 A flat binary's data segment is not next to its text

From `fs/binfmt_flat.c`:

```c
realdatastart = textpos + ntohl(hdr->data_start);
datapos = ALIGN(realdatastart + DATA_START_OFFSET_WORDS * sizeof(u32),
                FLAT_DATA_ALIGN);          /* FLAT_DATA_ALIGN is 0x20 */
reloc = (__be32 __user *)(datapos + (ntohl(hdr->reloc_start) - text_len));
```

Data starts at a **32-byte boundary past** the end of text, bss follows data,
and the relocation table is addressed relative to `datapos`. **This is why
elf2flt-built binaries reach their data through a GOT and set
`FLAT_FLAG_GOTPIC`** — and a GOT is exactly what hand-written assembly has not
got.

Symptom of getting it wrong:

```
binfmt_flat: reloc outside program 0x84000000 (0 - 0xd0/0x8e), killing stress!
```

`0x84000000` is `ntohl()` of the little-endian word `0x00000084` — the
program's own data pointer. The loader read the relocation table from inside
the text.

`mkflt.lua`'s resolution: **one segment, no load-time relocations.** Code,
rodata, data *and* bss all in the text region; bss materialised as zeroes in
the file rather than declared; `data_start = data_end = bss_end = end of
image`; `reloc_count = 0`. Cost: `.word some_symbol` cannot work, and is
refused by name with advice to use `lla` at run time.

### 6.5 `-Wl,--no-relax` is mandatory for small cross-compiled flat binaries

A flat binary is linked at address 0, so in a *small* one every address sits
inside the ±2048 an `x0`-relative instruction can reach, and the linker's
relaxation rewrites GOT accesses into absolute stores that bFLT cannot
relocate:

```
hello[35]: unhandled signal 11 code 0x2 at 0x48dfa2dc
status: 00000080 badaddr: 000007a4 cause: 00000007
2f8:  76f02823   sw  a5,1904(zero)   # 770 <__uclibc_progname>
```

`badaddr` is a **link-time offset**. Nothing else on the board trips over this
because lua (300 kB) and busybox (474 kB) have data far beyond 2048.
**`-mno-relax` on the compiler is NOT the fix — the damage is done at link
time.**

### 6.6 `-nostdlib` does not work with `ld-elf2flt`

`ld-elf2flt` links with `-q` (`--emit-relocs`), so every relocation survives
including already-resolved PC-relative ones, and `elf2flt` must classify each.
Its RISC-V case list (`elf2flt.c` ~line 828) covers `R_RISCV_32/64` and the
`NONE`/`ADD`/`SUB`/`SET` family and nothing else:

```
ERROR: reloc type R_RISCV_PCREL_HI20 unsupported in this context
```

Proved it was not an assembly-vs-C question by writing the same program in C:
with `-nostdlib` it fails identically. The fix is to link as an ordinary
program (`main`, crt1, libc) — about 2 kB, syscalls still direct.

`ld-elf2flt` picks `elf2flt -a -p FILE FILE` (the PIC/GOT path) when it finds
`_GLOBAL_OFFSET_TABLE_` in the output, and `-r FILE` otherwise. The `-r` path
is the one that rejects PCREL. **It was never the compiler that made the
difference.**

### 6.7 `.balign` after data in `.text`

A string of odd length leaves the location counter odd, so the next instruction
is misaligned. It assembles, links and converts **without a word of complaint**
and then the guest faults on the first instruction. Putting data in `.text` is
fine; forgetting to realign after it is not.

### 6.8 busybox quirks

**`mount` passes the option string straight to the filesystem.** Measured:

```
mount -t sysfs sysfs /sys                        -> ok
mount -t sysfs -o rw sysfs /sys                  -> ok
mount -t sysfs -o nosuid,nodev,noexec sysfs /sys -> sysfs: Unknown parameter 'nosuid'
```

The VFS flags fail exactly like the skeleton's `defaults` does. **This is why
`/sys` was not mounted at all for a long time.** The option field may contain
real filesystem options and nothing else.

The skeleton inittab's `::sysinit:/bin/mount -o remount,rw /` can never succeed
on a read-only romfs root and produces `romfs: Unknown parameter 'relatime'` as
the first thing the guest ever prints. `post-build.sh` deletes that line.

**`ash` exits a `set -e` script when a `while` condition fails.**

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

**binutils installs its tool set twice**, once in `/usr/bin` and again in
`/usr/<triple>/bin`, as hardlinks. `du` counts them once; **romfs has no
hardlinks and stores each copy**. Leaving that directory made the image
34,544,032 bytes and read as "the toolchain is enormous" when it was there
twice. Same for `ld`/`ld.bfd` and the bzip2 triplet.

---

# 7. Flash layout

### 7.1 A guest filesystem partition must be a power of two

`drivers/mtd/maps/physmap-core.c:521` rounds the window **down**:

```c
info->win_order = get_bitmask_order(resource_size(res)) - 1;
info->maps[i].size = BIT(info->win_order + ...);
```

A 12.375 MB partition becomes an 8 MB device. Everything upstream reports the
real span — the monitor prints `12672 KB`, physmap prints the full range — and
then `mtdblock0` is 8192 KB, `romfs_fill_super` refuses an image bigger than
its device, and the guest panics with `Unable to mount root fs on "mtd0"`.

**You cannot grow a filesystem by handing it the leftovers.** More windows, not
a bigger one. A three-window layout (4 + 8 + 1 = 13 MB) was built and booted:

```
physmap-flash 40030000.rom: [mem 0x40030000-0x4042ffff]
physmap-flash 40430000.rom: [mem 0x40430000-0x40c2ffff]
physmap-flash 40c30000.rom: [mem 0x40c30000-0x40d2ffff]
mtd0: 00400000 "rootfs"   mtd1: 00800000 "tools"   mtd2: 00100000 "extra"
```

and then removed, because the only thing needing 13 MB was `ld`.

If you add one back: **node order in the DTS is the mtd device order**
(`of_platform_populate` walks in source order, and the command line says
`root=mtd0`), and **each node needs its own distinct placeholder `reg` pair**.

### 7.2 Current layout

```
0x010000 + 0x060000 = 0x070000   factory   384 kB   app 235,440       (61%)
         + 0x260000 = 0x2D0000   kernel   2.375 MB  Image 2,247,408   (90%)
         + 0x010000 = 0x2E0000   dtb        64 kB   blob 1,781         (3%)
         + 0x800000 = 0xAE0000   rootfs      8 MB   image ~6.0 MB     (72%)
                                 5.1 MB unallocated tail
```

16 MB chip (`esptool flash-id`: manufacturer c8, device 4018). App must be
64K-aligned, data partitions 4K-aligned.

Kernel headroom (242,960 bytes) is the tightest number. Crossing it shows up as
a `parttool` write that refuses the image, **not** as a boot failure.

### 7.3 Changing the layout means reflashing everything

`parttool.py write_partition -n rootfs` resolves the offset from the table on
the chip, so names survive a layout change. The contents do not: `idf.py flash`
(which rewrites the table) must run first, then **every** partition must be
rewritten, because they have all moved.

---

# 8. Tooling

### 8.1 `driver.py` — the scriptable path

`.claude/skills/run-esp32-p4-emulator/driver.py`

```powershell
python .claude\skills\run-esp32-p4-emulator\driver.py run --variant hypervisor "uname -a" "cat /proc/mtd"
python ... attach --variant hypervisor --seconds 20      # watch, no reset
python ... keys   --variant hypervisor --send "q"        # raw keystrokes
```

Flags: `--variant hypervisor|native` (required — different logins), `--port`
(default COM17), `--baud` (default 4000000), `--boot-timeout`, `--retries`,
`--log FILE`.

> **PowerShell eats `$` in the command strings you pass.** Avoid `$` in
> anything sent through `driver.py` from PowerShell, or the guest receives a
> mangled command. This wasted a debugging round.

### 8.2 `hypmon.py` — the interactive TUI

Framed monitor modelled on a reference Rust TUI. Sidebar of actions, console
pane, filter row, timestamps by stable transcript index. **No send box** — the
console pane has focus and you type straight into it; click to focus, wheel to
scroll. Prefix is **`Ctrl+]`, pressed twice** (`^]` in the old UI was unclear
and was fixed). `--raw` restores plain passthrough.

Renders through `tools/hypvt.py`, a pure VT emulator with no I/O. Findings that
only appeared against real board output:

* **REP (`ESC[<n>b`)** — ncurses draws horizontal rules as one character plus a
  repeat count. Without REP a 77-column box renders as **three characters**.
* **DEC special graphics (`ESC(0`, SO/SI)** — frames arrive as `lqkxmjtuvwn`.
* **IRM (`CSI 4h/4l`)** — insert mode; without it the unknown-sequence count
  never reaches zero.
* **`nano` enters the alternate screen buffer** (`ESC[?1049h`) under
  `TERM=xterm-256color` and sets its own scroll region **43 times**, defeating
  any DECSTBM fence. What actually protects the status row is answering the
  cursor-position report with `rows - 1`.
* Under the alternate screen, scrollback holds only scrolled-off lines, so the
  still-visible shell output must be read from the **parked primary grid** or
  the pane goes blank while nano runs.
* Timestamps must be stamped by **stable transcript index**, not by scrollback
  position, or the column is blank for everything currently on screen.
* `Line.add()` counting SGR bytes as printable truncates rows and drops the
  right border — width accounting needs an explicit `add_styled(text, width)`.

Test suites (`test_hypvt.py` ~70 cases, `test_hypmon.py` 548 lines) feed input
byte-by-byte and at every two-way split, and assert every rendered row is
exactly the terminal width across 7 sizes × 6 modes.

`sys.stdout.reconfigure(encoding="utf-8", errors="replace")` is required — the
Windows console is cp1252 and cannot print box characters.

---

# 9. Measured numbers

| | |
|---|---|
| traps in a 45 s session | 345,906, ~542 cycles each |
| store access faults (pre-fix) | 12,000+ cycles each, most of monitor time |
| `wfi` traps that spun before the fix | 1,903,285, zero blocked — **1 trap in 6** |
| console throughput | ~80 kB/s guest, ~390 kB/s wire (4 Mbps) |
| traps per console character | ~1.56 |
| `tx dropped` per boot | 3392 @115200, 61 @2 Mbps, 0 @≥3 Mbps |
| guest RAM | 31 MB declared, ~29,232 kB managed |
| boot to login | ~25 s hypervisor, ~12 s native |
| CPU | 360 MHz; SYSTIMER 16 MHz |
| largest reliably-allocatable block | **4 MB** |

---

# 10. Native drivers (the next piece of work)

Full plan in `docs/NATIVE-DRIVERS.md`. Summary:

**The enabling fact:** IDF grants the peripheral window
`0x50000000`–`0x50100000` with `PMP_NAPOT | RW` where
`RW = PMP_L | PMP_R | PMP_W`, on a locked entry (15 on rev 1, 27 on rev 3), and
no lower-numbered entry shadows it. PMP applies to U-mode regardless of L.
**So the guest can already read and write real peripheral registers, and a
native driver needs no monitor change to reach its hardware.**

**Verify this first** — it is static analysis, not a measurement:

```
# devmem 0x500CA01C 32    # UART0 STATUS: RX count [9:0], TX count [25:16]
```

(needs `CONFIG_DEVMEM` and busybox `devmem`; neither checked).

**What is not free:**

* **Interrupts.** Real peripheral IRQs arrive at core 1's CLIC in M-mode.
  Polled drivers need no monitor change; interrupt-driven ones need the monitor
  to route into the emulated PLIC, which is the mechanism that already exists
  for `HYP_INUM_UART`.
* **Console ownership.** UART0 carries the monitor's output, the guest console,
  the `Ctrl+]` prefix, keyboard injection and the hypmon protocol. TX
  interleaving is harmless; **RX races** — whoever reads a byte keeps it.
  **Bring the native UART up as an additional tty (`ttyS1`), not a
  replacement**, benchmark, and only then move `console=`. Do not make the
  first experiment "switch the console over"; if it fails there is no way to
  see why.

**Hardware facts gathered:** UART0 at `0x500CA000`
(`DR_REG_HPPERIPH1_BASE 0x500C0000 + 0xA000`); register map in
`soc/uart_struct.h`; several P4 UART registers are `_sync` (clock-domain
crossed) and need a sync bit poked, but a driver that only touches `fifo` and
`status` never sees them; **the P4's UART clock config is in `HP_SYS_CLKRST`,
not in the UART** as on the S3 — so inherit the ROM/IDF baud and do not program
it.

`drivers/tty/serial/esp32_uart.c` exists in 6.8-rc1 with `esp,esp32-uart` and
`esp,esp32s3-uart`, but `depends on XTENSA_PLATFORM_ESP32 || (COMPILE_TEST &&
OF)` excludes riscv, and it programs S3 clock registers the P4 does not have
there.

**Suggested order:** confirm peripheral access → a trivial read-only driver
(hwrng) to prove DTS node → platform driver → real register with no console
risk → native UART as `ttyS1`, polled → monitor-side IRQ routing → move
`console=`. Steps 1–3 need no monitor change.

---

# 11. Open problems

* **`has_colors()` returns false in the guest.** `nctest` prints `colors : 0`.
  Terminfo is present and the terminal is capable. Unexplained.
* **No persistent writable storage.** `/home` is tmpfs. Persistence needs the
  monitor to write back to a flash partition, which it currently only reads —
  and see §4.9 about flash writes and core 1's trap handler.
* **`hyp_sync_icache()` is a sledgehammer.** Replacement is a hypercall in the
  guest's `local_flush_icache_all()`.
* **`hypmon.py`'s interactive layer has never been driven by a human**, only by
  rendering captured board output.
* **Clean-tree reproducibility of the buildroot build is unverified.**
* **`native/`** (the no-hypervisor port) is present but not the active line;
  the user considered dropping it.
* **The board was physically disconnected** at the end of the last session — no
  serial ports present on the host at all. Reconnect before assuming a driver
  bug.

---

# 12. How to check you have not broken anything

```bash
# guest userland
cp -a buildroot/board-esp32p4/. ~/buildroot-2026.05.1/board/esp32p4/
sed -i 's/\r$//' ~/buildroot-2026.05.1/board/esp32p4/*.sh
cd ~/buildroot-2026.05.1 && make -j3            # checkflat must pass
python3 board/esp32p4/mkromfs.py output/target output/images/rootfs.romfs rootfs
python3 board/esp32p4/checkromfs.py output/images/rootfs.romfs output/target
#   -> VERDICT: OK
```

```powershell
# monitor
. C:\Espressif\tools\Microsoft.v6.1-beta1.PowerShell_profile.ps1
idf.py build

# tooling
python tools\test_hypvt.py
python tools\test_hypmon.py

# on hardware
python .claude\skills\run-esp32-p4-emulator\driver.py run --variant hypervisor `
  "uname -srm" "cat /proc/mtd" "free | head -2" `
  "cd /home && cp /usr/share/stress.S . && mkflt stress.S && ./stress"
```

`stress` must print `stress ok` four times. The boot log must show
`physmap-flash 40030000.rom` and must **not** show any `Unknown parameter`
mount errors.

---

# 13. Git

Two commits were made at the end of the last session:

* `caddf34` — on-board rv32 toolchain, single-window rootfs, fstab repairs
  (69 files; first commit of `buildroot/`, `tools/`, `components/`, `.claude/`)
* `a9f7306` — `README.md` and the notes

Branch is `master`; the repo's "main branch" for PRs is `main`.
`converstation.md` was deleted by the user and that deletion is committed.

**Commit only when asked.**
