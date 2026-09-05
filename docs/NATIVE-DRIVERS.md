# Native drivers: findings and plan

Replacing emulated devices with real ESP32-P4 hardware, one at a time.

**Nothing here is built yet.** This is the reconnaissance, written down so the
next session starts from facts rather than from scratch. Every claim below is
either measured on the board or read out of the source file named.

---

## Why

Every emulated device access is an access fault into M-mode, decoded, serviced
and returned. The console costs about **1.56 traps per character** and tops out
at **80 kB/s** against ~390 kB/s on the wire. A native driver in the guest
touches the hardware register directly and costs nothing.

The end state the emulated devices exist to reach is a guest that talks to real
peripherals, at which point most of `mmio_devices[]` can go away.

---

## The enabling fact: the guest can already reach peripheral registers

This is the thing that makes the whole plan cheap, and it was not obvious.

IDF's `esp_cpu_configure_region_protection()`
(`components/esp_hw_support/port/esp32p4/cpu_region_protect.c`) ends with:

```c
// 7. Peripheral addresses
const uint32_t pmpaddr15 = PMPADDR_NAPOT(SOC_PERIPHERAL_LOW, SOC_PERIPHERAL_HIGH);
PMP_ENTRY_SET(15, pmpaddr15, PMP_NAPOT | RW);
```

(entry 15 on chip rev 1, entry 27 on rev 3), where

```c
const unsigned RW = PMP_L | PMP_R | PMP_W;
```

and `SOC_PERIPHERAL_LOW/HIGH` are `0x50000000` / `0x50100000`.

Two things follow:

1. **PMP permissions apply to U-mode regardless of the L bit.** The L bit
   *additionally* applies them to M-mode. So this entry grants the guest, in
   U-mode, read/write over the entire peripheral window.
2. **No lower-numbered entry shadows it.** Entries resolve by lowest matching
   index; 0–14 cover IRAM, DRAM, IROM, DROM, EXTRAM and RTC ranges, the
   monitor's own entry 5 covers `0x48000000` + 32 MB, and entry 6 covers the
   flash cache at `0x40000000`. None of them match `0x500xxxxx`.

**So a native driver needs no monitor change to get at its registers.** It is a
Linux platform driver plus a device tree node with the real address. That is
all.

### Verify this first

One command, and it was blocked only because the board was unplugged:

```
# devmem 0x500CA01C 32      # UART0 STATUS: RX count in [9:0], TX count in [25:16]
# devmem 0x500CA020 32      # UART0 CONF0
```

If those return plausible values the plan is open. If they fault, re-read the
PMP table on the running chip before doing anything else — the reasoning above
is static analysis of IDF's source, not a measurement.

Requires `CONFIG_DEVMEM` and busybox `devmem`; neither has been checked.

---

## What is *not* free

### 1. Interrupts

MMIO is direct; interrupts are not. The guest's interrupt controller is the
emulated PLIC, and real peripheral interrupts arrive at core 1's CLIC in
M-mode, where the monitor handles them. The monitor already does this for the
emulated UART (`HYP_INUM_UART`) by asserting a PLIC line.

So: **polled drivers work with no monitor change at all.** Interrupt-driven
ones need the monitor to route the real interrupt to a PLIC source, which is
the same mechanism that already exists — but it is monitor work, not guest
work, and it should be a second step.

### 2. Console ownership

This is the real obstacle for the UART specifically, and it is a design
problem, not a technical one.

UART0 is currently owned by the monitor. It carries:

* the monitor's own `esp_rom_printf` output,
* the guest console, forwarded byte by byte from the emulated 16550,
* the `Ctrl+]` monitor command prefix, handled on the RX side,
* USB keyboard bytes injected into the 16550 RX ring,
* the `hypmon.py` protocol.

If the guest drives UART0's FIFOs directly, TX interleaves (harmless — both are
just bytes) but RX **races**: the monitor and the guest both drain the same
hardware FIFO, and whoever reads a byte first keeps it. Monitor commands and
keyboard injection stop working.

**Recommended approach: bring the native UART up as an additional tty, not a
replacement.** A second `serial@500ca000` node gives the guest `ttyS1` on the
same hardware while `console=ttyS0` stays on the emulated 16550. Then

```
# time dd if=/dev/zero bs=1k count=256 | tr '\0' 'x' > /dev/ttyS1
```

benchmarks the native path against the emulated one with the console — and the
recovery path — untouched. Only once that number is known does it make sense to
move `console=` across, and even then the emulated 16550 should stay in the DTB
so a DTB reflash restores the old console.

Do **not** make the first experiment "switch the console over". If it does not
work there is no way to see why.

---

## Hardware facts gathered

### UART0

Base address `0x500CA000`:

```c
/* components/soc/esp32p4/register/hw_ver3/soc/reg_base.h */
#define DR_REG_HPPERIPH1_BASE  0x500C0000
#define DR_REG_UART0_BASE      (DR_REG_HPPERIPH1_BASE + 0xA000)
```

Register offsets from `soc/uart_struct.h` (`uart_dev_t`), which is the
authority — the layout is close to the ESP32-S3's but not identical:

| offset | register |
|---|---|
| `0x00` | `fifo` |
| `0x04` | `int_raw` |
| `0x08` | `int_st` |
| `0x0c` | `int_ena` |
| `0x10` | `int_clr` |
| `0x14` | `clkdiv_sync` |
| `0x18` | `rx_filt` |
| `0x1c` | `status` — RX count `[9:0]`, TX count `[25:16]` |
| `0x20` | `conf0_sync` |
| `0x24` | `conf1` |

Note the `_sync` suffixes: several P4 UART registers are synchronised into the
UART clock domain and need an update/sync bit poked after a write. A driver
that only reads `status` and reads/writes `fifo` never touches them.

**The clock configuration is not in the UART on the P4.** On the S3 it is
`UART_CLK_CONF_REG` at `0x78`; on the P4 it moved to `HP_SYS_CLKRST`. This
matters less than it sounds: the ROM and IDF have already configured UART0 for
115200, so a guest driver that **does not set the baud rate at all** and
inherits the existing configuration is both simpler and safer. Do that first.

### There is already an ESP32 UART driver in the kernel

`drivers/tty/serial/esp32_uart.c` exists in 6.8-rc1, with

```c
{ .compatible = "esp,esp32-uart" },
{ .compatible = "esp,esp32s3-uart" },
```

and

```
config SERIAL_ESP32
	depends on XTENSA_PLATFORM_ESP32 || (COMPILE_TEST && OF)
```

Two problems: the `depends on` excludes riscv (a one-line Kconfig patch, in the
same style as `patches/linux/0002-*`), and the driver programs the S3's clock
registers, which the P4 does not have at those offsets. Options, in increasing
order of work:

1. Add a `esp,esp32p4-uart` variant to the existing driver that skips clock
   setup entirely and inherits the bootloader's configuration.
2. Write a small standalone driver. FIFO in, FIFO out, `status` for counts —
   under 200 lines for a polled console.

Option 1 is less code but couples to a driver whose variant structure assumes
it owns the clock. Option 2 is more honest about what is actually being done
and easier to make polled-only. **Recommend 2 for the first cut**, and upstream
shape later if it is worth it.

### Other candidates, easiest first

| device | why it is easy | why it might not be |
|---|---|---|
| **hwrng** | one read-only register, no shared state, no console risk, instantly testable through `/dev/hwrng` | none — this is the right thing to prove the pipeline with |
| **GPIO / LED** | write-only, visible result | pin muxing is shared with core 0 |
| **systimer** | would remove the CLINT emulation | interrupts, and the guest's whole timekeeping depends on it |
| **SPI / SD** | would give persistent storage | large driver, DMA, and DMA-capable memory on this board is core 0's |

If the goal is to prove the peripheral-access pipeline end to end with zero
risk, **do the hwrng first and the UART second**, even though the UART is the
one that matters for speed.

---

## Suggested order

1. **Confirm peripheral access from the guest** — `devmem` on `0x500CA01C`.
   Blocked only on the board being plugged in.
2. **A trivial read-only driver** (hwrng) to prove DTS node → platform driver →
   real register, with no way to lose the console.
3. **Native UART as `ttyS1`**, polled, no baud programming. Benchmark against
   the emulated console.
4. **Monitor-side interrupt routing** for a real peripheral IRQ into the
   emulated PLIC.
5. **Move `console=` across**, keeping the emulated 16550 in the DTB.
6. Then decide whether the CLINT and PLIC emulation are worth replacing, which
   is a much bigger change — those are not devices Linux talks to occasionally,
   they are the timebase and the interrupt controller.

Each step is separately testable, and steps 1–3 need no monitor change at all.

---

## Where the code should go

Not decided. Two options:

* **Kernel patches**, alongside `patches/linux/0001-*` and `0002-*`, applied by
  buildroot. Consistent with what is already there, and the Kconfig change has
  to be a patch anyway.
* **An out-of-tree module directory** built by a buildroot package. Cleaner to
  iterate on, but NOMMU module loading has not been tested here, and the guest
  kernel currently has no module support configured at all — check
  `linux.fragment` before assuming it does.

Given the driver has to be built in for a console anyway, patches are probably
the answer.
