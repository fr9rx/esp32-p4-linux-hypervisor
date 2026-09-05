#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "riscv/csr.h"
#include "riscv/rvruntime-frames.h"
#include "riscv/interrupt.h"
#include "riscv/rv_utils.h"
#include "esp_partition.h"
#include "esp_cpu.h"
#include "esp_rom_sys.h"
#include "esp_rom_serial_output.h"

#include "soc/soc.h"
#include "soc/interrupts.h"
#include "soc/clic_reg.h"
#include "esp_private/interrupt_clic.h"
#include "hal/systimer_ll.h"
#include "hal/uart_ll.h"
#include "hal/cache_ll.h"

#include "hyp_kbd.h"

#define pmp_entery 5

/* Whether the guest waits for the physical UART. It does not.
 *
 * The trap histogram said store access faults -- almost all of them UART
 * register writes -- cost 12,000+ cycles each and were most of the time spent
 * in the monitor. Those cycles were the guest waiting for a byte to clear the
 * FIFO of what was then a 115200-baud wire -- worth paying while that wire was
 * the terminal, and not now that input comes from a USB keyboard and the wire
 * is only a log.
 *
 * Getting this to actually work took two attempts, and the first one is worth
 * recording because it looked right and measured as nothing. The original code
 * was
 *
 *     while (esp_rom_output_tx_one_char(value) != 0) { }
 *
 * and dropping the loop changed the histogram by 0.4% -- because the ROM
 * function spins on the FIFO level internally and then always reports success.
 * The loop around it never iterated. There is no "try once" mode to ask for;
 * the only way not to wait is to not call it.
 *
 * So the FIFO is written directly and the byte is dropped when there is no
 * room. What that costs in principle is a console log with holes in it during
 * any burst. In practice it costs nothing measurable any more, because the
 * console now runs at 4 Mbps rather than 115200 and the guest no longer outruns
 * the wire: `tx dropped` was 3392 per boot at 115200, 61 at 2 Mbps and 0 at 3
 * Mbps and above. Drop the baud back down and the holes come back -- the
 * counter in the histogram is how you find out.
 *
 * The monitor's own output is unaffected either way: esp_rom_printf() from core
 * 0 still blocks, so the trap histogram, the keyboard self-test and every
 * I:/W:/E: line stay complete regardless. Set this back to 1 for a guaranteed
 * faithful capture of the guest's own output. */
#define HYP_UART_LOG_BLOCKING 0

/* Ctrl-^ prints the trap histogram. Nothing binds it, and the numbers are
 * useless if you have to wait for a wedge to see them. */
#define HYP_KEY_STATS_DUMP 0x1eu

/* Ctrl-_ types "kbd-ok" through the keyboard decoder, so the input path can be
 * checked without a human at the keyboard. */
#define HYP_KEY_KBD_TEST   0x1fu

/* Trap accounting, defined next to the trace ring further down. Declared here
 * because the handlers that increment it are above that point. */
#define HYP_STATS_CAUSES 48u

/* What an interrupt's mcause exception code means on this CLIC.
 *
 * Measured, not read: routing inum 20 and 21 and printing the raw code gave 36,
 * i.e. inum + 16. That is the standard CLIC layout -- codes 0..15 are the
 * architectural local interrupts (MSI 3, MTI 7, MEI 11) and platform interrupts
 * start at 16 -- but the P4 TRM does not say so, and assuming the code WAS the
 * inum put every host interrupt in the clamped last histogram bucket labelled
 * "host-other" while looking perfectly plausible. */
#define HYP_CLIC_MCAUSE_BASE 16u
struct hyp_stats_s {
    uint32_t exc[HYP_STATS_CAUSES];     /* traps by mcause, interrupt bit clear */
    uint32_t irq[HYP_STATS_CAUSES];     /* traps by mcause, interrupt bit set   */
    uint64_t exc_cycles[HYP_STATS_CAUSES];
    uint64_t irq_cycles[HYP_STATS_CAUSES];
    uint32_t unhandled;
    uint32_t mmio[4];                   /* by index into mmio_devices[]         */
    uint32_t csr_read_only;             /* csrr with no write-back              */
    uint32_t csr_write;
    uint32_t mrets;
    uint32_t wfi_spins;                 /* wfi that could not block             */
    uint32_t wfi_blocks;                /* wfi that did block                   */
    uint32_t injections;
    uint64_t icache_sync_cycles;
    uint32_t icache_syncs;
    uint64_t idle_cycles;               /* time asleep in a blocking wfi */
    uint32_t irq_code_seen;             /* last raw mcause code for an interrupt */
};
static struct hyp_stats_s hyp_stats;

#define null ((void*)0)
uint32_t linux_base_address = 0x48000000;
uint32_t linux_size = 0x02000000;      /* 32 MB: all populated PSRAM, 0x48000000-0x4A000000 */
uint32_t dtb_address = 0x49F00000;     /* top 1 MB: PMP-granted, outside the memory{} node */

/* The guest's root filesystem -- executed in place, out of flash.
 *
 * It used to be memcpy'd out of the "rootfs" partition into a 2 MB PSRAM window
 * at 0x49D00000, which cost 2 MB of the guest's RAM forever and capped the
 * image at the size of that window. It is now left where it is: the partition
 * stays esp_partition_mmap()'d for the lifetime of the board, and the DTB's
 * mtd-rom node is patched to point at the mapping instead.
 *
 * Three things make that work, and all three are properties of this chip rather
 * than choices:
 *
 *   - the mapping lands in the flash cache window, 0x40000000-0x44000000, and
 *     PMP entry 6 covers exactly that window with R+X and the L bit
 *     (cpu_region_protect.c, the rev < v3 path). PMP permissions apply to
 *     U-mode, so the guest can already read and execute there. No new entry --
 *     which matters, because only entries 5 and 7-10/12-14 are free and 6
 *     outranks all of the latter.
 *   - physmap-core's ioremap is the identity on a NOMMU kernel, so an mtd-rom
 *     node whose reg names the mapping resolves to plain loads.
 *   - romfs_get_unmapped_area -> mtd_get_unmapped_area gives bFLT text pages
 *     straight out of that window, so program text is now XIP from flash and
 *     never occupies RAM at all.
 *
 * What it costs: flash is slower than PSRAM, so exec and demand-read of program
 * text is slower. What it buys: 2 MB of guest RAM back, no size cap below the
 * partition size, and the window at 0x49D00000 freed for the framebuffer.
 *
 * This is a table rather than a single mapping because there can be more than
 * one window, and the reason is a constraint rather than an ambition:
 * physmap-core.c:521 rounds an mtd-rom window DOWN to a power of two, so one
 * guest filesystem can be 4 MB or 8 MB and nothing between, and 16 MB does not
 * fit beside the app and kernel on a 16 MB chip. Several small windows are the
 * only way past that ceiling.
 *
 * A three-window layout (rootfs 4 + tools 8 + extra 1 = 13 MB) was built and
 * booted here, to carry a native ld. ld does not run on this board -- see
 * buildroot/README.md -- and what replaced it fits in 8 MB, so there is one
 * entry again. The table stays because the constraint has not gone anywhere.
 *
 * Each window needs its own placeholder reg pair in the DTS, and each pair must
 * appear exactly once in the blob. */
struct guest_window {
    const char *label;              /* partition label, and linux,mtd-name */
    const char *mountpoint;         /* where the guest's fstab puts it; doc only */
    uint8_t     placeholder[8];     /* the DTS reg pair, big-endian <addr size> */
    const void *mapping;            /* filled in by window_map_from_flash() */
    uint32_t    span;
};

/* ORDER MATTERS, and it must match dts/p4-phase6.dts. of_platform_populate
 * walks the tree in source order, so the guest sees these as mtdblock0/1/2 in
 * this order, and the kernel command line says root=mtd0. This array does not
 * control that -- the DTS does -- but keeping the two in the same order is the
 * only way the comments stay honest. */
static struct guest_window guest_windows[] = {
    { "rootfs", "/", { 0x49, 0xD0, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00 }, null, 0u },
};
#define GUEST_WINDOW_COUNT (sizeof(guest_windows) / sizeof(guest_windows[0]))

bool linux_running = false;

extern int hyp_vector_table;
static uint32_t hyp_stack[2048] __attribute__((aligned(16)));

/* Core 1's own stack, for the short stretch between being diverted out of
 * FreeRTOS and mret-ing into the guest.
 *
 * Separate from hyp_stack (which hyp_trap_entry swaps in from mscratch) and
 * separate from the IDF startup stack it would otherwise still be using, and
 * the second point is the one that matters: core 0's main_task calls
 * heap_caps_enable_nonos_stack_heaps(), which hands both cores' startup stacks
 * to the heap. Core 1 has to be off its startup stack before it says it is
 * done, or an allocation on core 0 can land inside core 1's live frame. */
static uint32_t hyp_boot_stack[512] __attribute__((aligned(16)));

/* Core 0 copies the kernel; core 1 must not mret into a half-copied image. */
static volatile bool guest_image_ready = false;

/* Set by the trap handler on core 1 when Ctrl-^ arrives, acted on by core 0.
 * Printing from inside the trap handler would put a few hundred blocking UART
 * writes in the guest's path. */
static volatile bool hyp_stats_wanted = false;


/* ===========================================================================
 * Host resources owned by the hypervisor
 *
 * Two real interrupts exist, and they exist for one reason: the hypervisor owns
 * mtvec, so nothing else can force control back out of a guest loop that takes
 * no traps. They do not perform injection -- they only manufacture a trap.
 *
 * inum 20 and 21 are free: soc.h reserves 24-28 (T1_WDT, CACHEERR, MEMPROT_ERR,
 * ASSIST_DEBUG, IPC_ISR) and 0 is ETS_INVALID_INUM.
 * ===========================================================================
 */

#define HYP_INUM_TIMER      20
#define HYP_INUM_UART       21
#define HYP_INT_PRIO        4       /* NOT 0: see hyp_setup_interrupts() */

/* Escape hatch. Blocking in a real wfi is correct and much cheaper than letting
 * the guest spin, but it relies on the P4's CLIC waking the core from wfi while
 * mstatus.MIE is clear -- true per the RISC-V spec (the wakeup condition ignores
 * MIE), unverified on this silicon. If the guest hangs the moment it first idles,
 * set this to 0: the guest then spins through wfi traps as it did before, which
 * is wasteful but cannot hang. */
#define HYP_WFI_BLOCKS      1

#define HYP_SYSTIMER_COUNTER  0     /* shared with esp_timer, read-only for us */
/* Alarm 1, not 0.
 *
 * Alarm 0 is SYSTIMER_ALARM_OS_TICK_CORE0. It was free while the monitor kept
 * FreeRTOS from ever starting; core 0 now boots normally and the port layer
 * reprograms alarm 0 on every tick, which would retarget the guest's clock 100
 * times a second. Alarm 1 is SYSTIMER_ALARM_OS_TICK_CORE1 and is permanently
 * free instead, because core 1 is the one core that never runs FreeRTOS.
 * (Alarm 2 belongs to esp_timer.) */
#define HYP_SYSTIMER_ALARM    1     /* SYSTIMER_ALARM_OS_TICK_CORE1: core 1 has no RTOS */

/* SYSTIMER is a 52-bit counter (32 lo + 20 hi) at a fixed 16 MHz
 * (XTAL 40 MHz / 2.5), which is exactly the timebase-frequency the guest DTS
 * declares. Nothing rescales anywhere. */
#define HYP_MTIME_MAX       (1ULL << 52)

/* Guest physical device windows. */
#define HYP_CLINT_BASE          0x02000000u
#define HYP_CLINT_SIZE          0x00010000u
#define HYP_PLIC_BASE           0x0C000000u
#define HYP_PLIC_SIZE           0x00400000u
#define HYP_UART_BASE           0x10000000u
#define HYP_UART_SIZE           0x00000100u

#define PLIC_UART_SOURCE    10      /* matches interrupts = <10> in the DTS */
#define PLIC_MAX_SOURCE     31      /* matches riscv,ndev = <31> */


/* ===========================================================================
 * Trap context
 *
 * Everything a handler needs about one trap, built once per trap by
 * hypervisor(). Before this existed, `instruction` and `length` were threaded
 * through every handler as loose parameters and the dispatcher had to know how
 * instructions are encoded. Now the dispatcher only routes.
 * ===========================================================================
 */

typedef struct {
    RvExcFrame *frame;
    uint32_t    cause;      /* mcause, interrupt bit already stripped by the dispatcher */
    uint32_t    tval;       /* mtval: an instruction on cause 2, an address on 5/7 */
    uint32_t    encoding;   /* the faulting instruction -- valid only after trap_decode() */
    uint32_t    length;     /* its size in bytes, 2 or 4 -- valid only after trap_decode() */
} GuestTrap;

/* How the trap ends. This exists so that frame->mepc is advanced in exactly one
 * place. Every handler used to do its own `mepc += 4`, which is wrong for any
 * handler that jumps (mret, trap injection): those set mepc themselves and must
 * not have it advanced on top. */
typedef enum {
    TRAP_UNHANDLED = 0,   /* not ours -> panic */
    TRAP_ADVANCE,         /* emulated -> skip past the instruction */
    TRAP_RESUME,          /* mepc already set by the handler -> leave it alone */
} TrapResult;


/* ===========================================================================
 * Guest register access
 *
 * RvExcFrame starts with mepc, then x1..x31, so ((uint32_t *)frame)[n] is
 * exactly x[n] for n in 1..31. Index 0 lands on mepc, which is why x0 must be
 * special-cased in both directions -- a raw write to index 0 would corrupt the
 * return address instead of being discarded.
 * ===========================================================================
 */

static uint32_t reg_read(RvExcFrame *frame, uint32_t index)
{
    return index ? (uint32_t)((uint32_t *)frame)[index] : 0u;
}

static void reg_write(RvExcFrame *frame, uint32_t index, uint32_t value)
{
    if (index) {
        ((uint32_t *)frame)[index] = value;
    }
}


/* ===========================================================================
 * Instruction fetch and decode
 * ===========================================================================
 */

/* Read the instruction at mepc out of guest memory.
 *
 * Two halfword reads, never one word read: mepc is only 2-byte aligned whenever
 * the previous instruction was compressed, and a misaligned lw here would fault
 * inside the trap handler itself.
 *
 * volatile is load-bearing, not decoration -- without it the compiler is free to
 * fuse these two adjacent halfword loads back into the single word load this is
 * avoiding.
 *
 * Length lives in the low bits of the first halfword:
 *   xxxxxxxxxxxxxxaa,  aa != 11   -> 16-bit (compressed)
 *   xxxxxxxxxxxbbb11, bbb != 111  -> 32-bit
 *   xxxxxxxxxx011111              -> 48-bit or wider, does not exist on rv32imac
 */
static bool guest_fetch(uint32_t mepc, uint32_t *encoding, uint32_t *length)
{
    uint32_t low = *(volatile uint16_t *)mepc;

    if ((low & 0x3u) != 0x3u) {
        *encoding = low;
        *length = 2u;
        return true;
    }
    if ((low & 0x1fu) == 0x1fu) {
        return false;
    }

    *encoding = low | ((uint32_t)*(volatile uint16_t *)(mepc + 2u) << 16);
    *length = 4u;
    return true;
}

/* Rewrite a compressed instruction as the 32-bit instruction it stands for, so
 * that every handler downstream only ever sees 32-bit encodings.
 *
 * Only c.lw and c.sw are handled, and that is not a shortcut. Disassembling the
 * kernel Image shows which compressed memory ops it actually contains:
 *   c.lw 34328, c.sw 16640      -> can name any pointer, so can hit a device
 *   c.lwsp 59377, c.swsp 58614  -> sp-relative, and sp never points at a device
 *   c.lbu / c.sb / c.lhu / c.sh -> 0 occurrences; Zcb is absent from rv32imac,
 *                                  so byte and halfword MMIO is always 4-byte
 *
 * Verified 90/90 against riscv32-esp-elf-as across 3 dest regs x 3 base regs x
 * 5 offsets x both directions.
 *
 * Returns 0 for anything else, which the caller treats as undecodable.
 */
static uint32_t rvc_expand(uint32_t c)
{
    if ((c & 0x3u) != 0x0u) {
        return 0u;                              /* quadrant 0 only */
    }

    uint32_t rs1 = 8u + ((c >> 7) & 0x7u);      /* 3-bit fields name x8..x15 */
    uint32_t rx  = 8u + ((c >> 2) & 0x7u);      /* rd' for c.lw, rs2' for c.sw */
    uint32_t off = (((c >> 10) & 0x7u) << 3)    /* uimm[5:3] */
                 | (((c >>  6) & 0x1u) << 2)    /* uimm[2]   */
                 | (((c >>  5) & 0x1u) << 6);   /* uimm[6]   */

    switch ((c >> 13) & 0x7u) {
    case 0x2u:  /* c.lw -> lw rx, off(rs1)   I-type */
        return (off << 20) | (rs1 << 15) | (0x2u << 12) | (rx << 7) | 0x03u;
    case 0x6u:  /* c.sw -> sw rx, off(rs1)   S-type */
        return ((off >> 5) << 25) | (rx << 20) | (rs1 << 15)
             | (0x2u << 12) | ((off & 0x1fu) << 7) | 0x23u;
    default:
        return 0u;
    }
}

/* Fill in trap->encoding and trap->length.
 *
 * mtval is a union whose tag is mcause: on an illegal-instruction trap it holds
 * the instruction itself, so no fetch is needed. On an access fault it holds the
 * faulting address, so the instruction has to be read from mepc.
 *
 * Handlers call this themselves rather than the dispatcher calling it for
 * everyone, so that handlers which never look at the encoding (trap injection,
 * mret) do not pay for a fetch. That matters: ecall is the hottest trap.
 *
 * Note that expansion rewrites `encoding` but never touches `length`. They
 * answer different questions -- encoding is what gets decoded, length is how far
 * mepc moves, and a c.lw is still two bytes after it has been expanded. Fusing
 * them would make the guest skip an instruction on every compressed MMIO access.
 */
static bool trap_decode(GuestTrap *trap)
{
    if (trap->cause == 2u) {
        trap->encoding = trap->tval;
        trap->length = ((trap->tval & 0x3u) == 0x3u) ? 4u : 2u;
        return true;
    }

    uint32_t encoding;
    uint32_t length;

    if (!guest_fetch((uint32_t)trap->frame->mepc, &encoding, &length)) {
        return false;
    }

    if (length == 2u) {
        uint32_t expanded = rvc_expand(encoding);
        if (expanded == 0u) {
            return false;
        }
        encoding = expanded;
    }

    trap->encoding = encoding;
    trap->length = length;
    return true;
}


/* ===========================================================================
 * Shadow CSR bank
 *
 * The guest believes it is in M-mode and owns the machine CSRs. It does not:
 * it runs in U-mode, and its csr instructions trap here as illegal instructions.
 * These slots are the CSRs it thinks it has.
 *
 * This has to exist before any trap can be handed back to the guest. Linux's
 * own trap entry (handle_exception, kernel offset 0x1775a8) opens with
 *
 *     csrrw tp, mscratch, tp
 *     bnez  tp, ...
 *     csrr  tp, mscratch        <- read-back of the value just parked
 *     sw    sp, 8(tp)
 *
 * so mscratch is Linux's kernel-vs-user discriminator. Returning 0 for every
 * CSR read would make that csrr yield 0 and the store go to address 0x8 -- and
 * the panic would point at address 8, not at the CSR emulation that caused it.
 * ===========================================================================
 */

#define CSR_MSTATUS  0x300
#define CSR_MIE      0x304
#define CSR_MTVEC    0x305
#define CSR_MSCRATCH 0x340
#define CSR_MEPC     0x341
#define CSR_MCAUSE   0x342
#define CSR_MTVAL    0x343
#define CSR_MIP      0x344

/* MSTATUS_MIE/MPIE/MPP and MIP_MSIP/MTIP/MEIP come from riscv/encoding.h. */
#define MSTATUS_MPP_S 11

#define PRIV_U 0u
#define PRIV_M 3u

static struct {
    uint32_t mstatus;
    uint32_t mie;
    uint32_t mtvec;
    uint32_t mscratch;
    uint32_t mepc;
    uint32_t mcause;
    uint32_t mtval;
    uint32_t mip;       /* bits 7 and 11 are NOT stored here -- see shadow_mip_read() */
} shadow = { .mstatus = (PRIV_M << MSTATUS_MPP_S) };   /* born believing it is M-mode */

/* The guest's software privilege level. Real hardware would distinguish an ecall
 * from the kernel (cause 11) from one out of userspace (cause 8), but here the
 * whole guest runs in U-mode so the hardware always reports 8. This variable is
 * what recovers the distinction, and it moves on exactly two events: a trap
 * injected into the guest sets it to M, and mret restores it from mstatus.MPP.
 *
 * Note this kernel dispatches syscalls from excp_vect_table[8] and requires
 * MPP == 0; excp_vect_table[11] is "Oops - environment call from M-mode". So
 * ecall always injects cause 8, and guest_priv only feeds MPP. */
static uint32_t guest_priv = PRIV_M;

/* PMP. _start_kernel's "allow access to all memory" block writes pmpaddr0 = -1
 * then pmpcfg0 = A_NAPOT|R|W|X. Executing either for real would be fatal: PMP
 * resolves by lowest matching entry, so a guest entry 0 outranks the monitor's
 * entry 5 and would hand the guest the monitor's own memory. These are pure
 * storage -- what the guest writes reads back, and nothing consults them.
 * (Real hardware masks unimplemented pmpaddr bits on read-back; this kernel
 * never reads them, so the unmasked value is close enough to say so here.) */
/* CSR_PMPCFG0 (0x3a0) and CSR_PMPADDR0 (0x3b0) come from riscv/csr.h. */
#define CSR_PMPCFG3   (CSR_PMPCFG0  + 3)
#define CSR_PMPADDR15 (CSR_PMPADDR0 + 15)

static uint32_t shadow_pmpcfg[(CSR_PMPCFG3 - CSR_PMPCFG0) + 1];
static uint32_t shadow_pmpaddr[(CSR_PMPADDR15 - CSR_PMPADDR0) + 1];

static uint32_t *shadow_slot(uint32_t csr)
{
    switch (csr) {
    case CSR_MSTATUS:  return &shadow.mstatus;
    case CSR_MIE:      return &shadow.mie;
    case CSR_MTVEC:    return &shadow.mtvec;
    case CSR_MSCRATCH: return &shadow.mscratch;
    case CSR_MEPC:     return &shadow.mepc;
    case CSR_MCAUSE:   return &shadow.mcause;
    case CSR_MTVAL:    return &shadow.mtval;
    default:           break;
    }

    if (csr >= CSR_PMPCFG0 && csr <= CSR_PMPCFG3) {
        return &shadow_pmpcfg[csr - CSR_PMPCFG0];
    }
    if (csr >= CSR_PMPADDR0 && csr <= CSR_PMPADDR15) {
        return &shadow_pmpaddr[csr - CSR_PMPADDR0];
    }
    return null;
}

/* CSRs the guest may touch but whose value nothing depends on. Reads give 0 and
 * writes are dropped, rather than faulting. fflags/frm/fcsr are listed because
 * this kernel is CONFIG_FPU=y and reset_regs writes fcsr. */
static bool csr_read_only_zero(uint32_t csr)
{
    switch (csr) {
    case 0x001u:   /* fflags    */
    case 0x002u:   /* frm       */
    case 0x003u:   /* fcsr      */
    case 0x301u:   /* misa      */
    case 0xf11u:   /* mvendorid */
    case 0xf12u:   /* marchid   */
    case 0xf13u:   /* mimpid    */
    case 0xf14u:   /* mhartid   */
        return true;
    default:
        return false;
    }
}


/* ===========================================================================
 * Emulated CLINT -- timer
 * ===========================================================================
 */

static uint64_t shadow_mtimecmp = UINT64_MAX;   /* ~0 == timer disabled */

/* A 52-bit snapshot, matching what systimer_hal_get_counter_value() does. */
static uint64_t hyp_mtime(void)
{
    systimer_ll_counter_snapshot(&SYSTIMER, HYP_SYSTIMER_COUNTER);
    while (!systimer_ll_is_counter_value_valid(&SYSTIMER, HYP_SYSTIMER_COUNTER)) { }

    uint32_t lo = systimer_ll_get_counter_value_low(&SYSTIMER, HYP_SYSTIMER_COUNTER);
    uint32_t hi = systimer_ll_get_counter_value_high(&SYSTIMER, HYP_SYSTIMER_COUNTER);
    return ((uint64_t)hi << 32) | lo;
}

/* Point the host alarm at exactly mtimecmp, one-shot.
 *
 * Re-armed on EVERY mtimecmp word write, not just the final one. Linux's RV32
 * path writes lo = 0xFFFFFFFF, then hi, then lo, and the intermediate state can
 * momentarily name a target in the past. That is harmless: a spurious host
 * interrupt just re-evaluates MTIP and finds it false. Being clever about
 * "only arm on the last write" is how ticks get lost.
 *
 * SYSTIMER_LL_ALARM_MISS_COMPENSATE == 1 on this chip, so a target already in
 * the past fires immediately rather than never. */
static void hyp_arm_timer(void)
{
    systimer_ll_enable_alarm(&SYSTIMER, HYP_SYSTIMER_ALARM, false);
    systimer_ll_clear_alarm_int(&SYSTIMER, HYP_SYSTIMER_ALARM);

    if (shadow_mtimecmp >= HYP_MTIME_MAX) {
        return;                                  /* disabled; leave the alarm off */
    }

    systimer_ll_connect_alarm_counter(&SYSTIMER, HYP_SYSTIMER_ALARM, HYP_SYSTIMER_COUNTER);
    systimer_ll_enable_alarm_oneshot(&SYSTIMER, HYP_SYSTIMER_ALARM);
    systimer_ll_set_alarm_target(&SYSTIMER, HYP_SYSTIMER_ALARM, shadow_mtimecmp);
    systimer_ll_apply_alarm_value(&SYSTIMER, HYP_SYSTIMER_ALARM);
    systimer_ll_enable_alarm_int(&SYSTIMER, HYP_SYSTIMER_ALARM, true);
    systimer_ll_enable_alarm(&SYSTIMER, HYP_SYSTIMER_ALARM, true);
}

/* MTIP is a wire, not a latch: on a real CLINT mip.MTIP is combinationally
 * (mtime >= mtimecmp), read-only in mip, and the ONLY way software clears it is
 * by writing a larger mtimecmp. That is why timer-clint.c stops the clockevent
 * by writing mtimecmp = ULLONG_MAX rather than clearing a bit.
 *
 * Compute it, never latch it. A latched shadow.mip drops a tick in exactly the
 * case that matters: after the one-shot alarm has fired and been disarmed, MTIP
 * is still true until the guest acks. */
static bool mtip_pending(void)
{
    return hyp_mtime() >= shadow_mtimecmp;
}

static uint32_t clint_read(uint32_t offset, uint32_t width)
{
    (void)width;
    switch (offset) {
    case 0x0000u: return 0u;                                  /* msip, single hart */
    case 0x4000u: return (uint32_t)shadow_mtimecmp;
    case 0x4004u: return (uint32_t)(shadow_mtimecmp >> 32);
    /* Deliberately re-snapshot on each half. Linux's RV32 clint_get_cycles64
     * does the hi/lo/hi retry loop, so a torn read is self-correcting -- the
     * same contract it has with real hardware. */
    case 0xBFF8u: return (uint32_t)hyp_mtime();
    case 0xBFFCu: return (uint32_t)(hyp_mtime() >> 32);
    default:      return 0u;
    }
}

static void clint_write(uint32_t offset, uint32_t value, uint32_t width)
{
    (void)width;
    switch (offset) {
    case 0x0000u:
        break;                                                /* msip, single hart */
    case 0x4000u:
        shadow_mtimecmp = (shadow_mtimecmp & 0xFFFFFFFF00000000ULL) | value;
        hyp_arm_timer();
        break;
    case 0x4004u:
        shadow_mtimecmp = (shadow_mtimecmp & 0x00000000FFFFFFFFULL) | ((uint64_t)value << 32);
        hyp_arm_timer();
        break;
    default:
        break;
    }
}


/* ===========================================================================
 * Emulated PLIC
 *
 * Modelled against drivers/irqchip/irq-sifive-plic.c, one context (hart 0,
 * M-mode, MEIP) because the DTS has a single interrupts-extended entry.
 *
 *   0x000000 + 4*n              priority[n]
 *   0x001000 + 4*(n/32)         pending bitmap (read-only)
 *   0x002000 + 0x80*ctx + ...   per-context enable bitmap
 *   0x200000 + 0x1000*ctx + 0   threshold
 *   0x200000 + 0x1000*ctx + 4   claim (read) / complete (write)
 * ===========================================================================
 */

static struct {
    uint32_t priority[PLIC_MAX_SOURCE + 1];
    uint32_t enable;        /* context 0, sources 0..31 */
    uint32_t pending;
    uint32_t claimed;       /* in-service: not re-claimable until completed */
    uint32_t threshold;
} plic;

/* Highest-priority claimable source, or 0 if none.
 *
 * The `claimed` mask is what makes level sources safe. Without it a level
 * source whose condition is still true is immediately re-pending, gets
 * re-claimed inside plic_handle_irq's while loop, and spins forever. Real PLIC
 * gateways refuse to forward a new request between claim and complete. */
static uint32_t plic_peek(void)
{
    uint32_t best = 0u;
    uint32_t best_priority = plic.threshold;    /* strictly greater than threshold */

    for (uint32_t n = 1u; n <= PLIC_MAX_SOURCE; n++) {
        uint32_t bit = 1u << n;
        if ((plic.pending & bit) == 0u)  { continue; }
        if ((plic.enable  & bit) == 0u)  { continue; }
        if ((plic.claimed & bit) != 0u)  { continue; }
        if (plic.priority[n] <= best_priority) { continue; }   /* ties -> lowest id */
        best = n;
        best_priority = plic.priority[n];
    }
    return best;
}

static bool meip_pending(void)
{
    return plic_peek() != 0u;
}

static void plic_set_pending(uint32_t source, bool asserted)
{
    if (asserted) {
        plic.pending |= (1u << source);
    } else {
        plic.pending &= ~(1u << source);
    }
}

static uint32_t plic_read(uint32_t offset, uint32_t width)
{
    (void)width;

    if (offset < 0x1000u) {                                   /* priority array */
        uint32_t n = offset / 4u;
        return (n <= PLIC_MAX_SOURCE) ? plic.priority[n] : 0u;
    }
    if (offset >= 0x1000u && offset < 0x2000u) {              /* pending bitmap */
        return (offset == 0x1000u) ? plic.pending : 0u;
    }
    if (offset >= 0x2000u && offset < 0x200000u) {            /* enable bitmaps */
        return (offset == 0x2000u) ? plic.enable : 0u;        /* context 0 only */
    }

    /* Context block. Anything past context 0 reads 0 rather than aliasing onto
     * context 0 -- much easier to debug if a stray access shows up. */
    if (offset >= 0x200000u && offset < 0x201000u) {
        if (offset == 0x200000u) {
            return plic.threshold;
        }
        if (offset == 0x200004u) {                            /* claim */
            uint32_t source = plic_peek();
            if (source != 0u) {
                plic.pending &= ~(1u << source);
                plic.claimed |=  (1u << source);
            }
            return source;      /* 0 when nothing qualifies: plic_handle_irq
                                 * loops `while ((hwirq = readl(claim)))`, so a
                                 * bogus nonzero here hangs the guest silently */
        }
    }
    return 0u;
}

static void v16550_reeval(void);

static void plic_write(uint32_t offset, uint32_t value, uint32_t width)
{
    (void)width;

    if (offset < 0x1000u) {
        uint32_t n = offset / 4u;
        if (n <= PLIC_MAX_SOURCE) {
            plic.priority[n] = value;
        }
        return;
    }
    if (offset >= 0x2000u && offset < 0x200000u) {
        if (offset == 0x2000u) {
            plic.enable = value;      /* plic_toggle does readl|mask -> writel,
                                       * so this must read back what was written */
        }
        return;
    }
    if (offset >= 0x200000u && offset < 0x201000u) {
        if (offset == 0x200000u) {
            plic.threshold = value;
            return;
        }
        if (offset == 0x200004u) {                            /* complete */
            uint32_t source = value;
            if (source != 0u && source <= PLIC_MAX_SOURCE &&
                (plic.claimed & (1u << source)) != 0u) {
                plic.claimed &= ~(1u << source);
                /* A level source may re-assert immediately: recompute from the
                 * device rather than assuming the condition cleared. */
                if (source == PLIC_UART_SOURCE) {
                    v16550_reeval();
                }
            }
            return;
        }
    }
}


/* ===========================================================================
 * Emulated 16550 UART
 *
 * Linux reaches this first through earlycon=uart8250,mmio,0x10000000 (polled),
 * then through the real serial8250 driver once console=ttyS0 hands over. The
 * handover is where input, IIR, DLAB and the TX interrupt start to matter.
 * ===========================================================================
 */

#define UART_RX_RING_SIZE   512u    /* 512 keystrokes; the producers are a
                                     * 4 Mbps wire and a USB keyboard, and the
                                     * consumer is the guest's next MMIO read */

static struct {
    uint8_t  ier;       /* 1: bit0 ERBFI (rx avail), bit1 ETBEI (thr empty) */
    uint8_t  lcr;       /* 3: bit7 DLAB */
    uint8_t  mcr;       /* 4: bit4 loopback */
    uint8_t  scr;       /* 7: scratch */
    uint8_t  fcr;       /* 2 (write): bit0 FIFO enable */
    uint8_t  dll;       /* divisor latch low  (DLAB=1, offset 0) */
    uint8_t  dlm;       /* divisor latch high (DLAB=1, offset 1) */
    uint8_t  ring[UART_RX_RING_SIZE];
    uint32_t head;
    uint32_t tail;
    uint32_t overruns;
    uint32_t tx_dropped;    /* guest output the wire could not keep up with */
} uart16550;

static bool uart_rx_empty(void)
{
    return uart16550.head == uart16550.tail;
}

static void uart_rx_push(uint8_t c)
{
    uint32_t next = (uart16550.head + 1u) % UART_RX_RING_SIZE;
    if (next == uart16550.tail) {
        uart16550.overruns++;       /* drop; never block inside a trap handler */
        return;
    }
    uart16550.ring[uart16550.head] = c;
    uart16550.head = next;
}

static uint8_t uart_rx_pop(void)
{
    if (uart_rx_empty()) {
        return 0u;
    }
    uint8_t c = uart16550.ring[uart16550.tail];
    uart16550.tail = (uart16550.tail + 1u) % UART_RX_RING_SIZE;
    return c;
}

/* Recompute the UART's interrupt line into the PLIC.
 *
 * Two conditions raise it, and the second is easy to miss: once the driver
 * takes over from earlycon, serial8250_start_tx() sets IER.ETBEI and then waits
 * for a THRE interrupt to send the rest of its buffer. Our THR is always empty,
 * so ETBEI set means the line is asserted continuously -- exactly what real
 * hardware does. Omit it and output stops dead after the first chunk. */
static void v16550_reeval(void)
{
    bool rx_int  = ((uart16550.ier & 0x01u) != 0u) && !uart_rx_empty();
    bool tx_int  = ((uart16550.ier & 0x02u) != 0u);
    plic_set_pending(PLIC_UART_SOURCE, rx_int || tx_int);
}

static uint8_t uart_reg_read(uint32_t offset)
{
    if ((uart16550.lcr & 0x80u) != 0u) {        /* DLAB: divisor latches */
        if (offset == 0u) { return uart16550.dll; }
        if (offset == 1u) { return uart16550.dlm; }
    }

    switch (offset) {
    case 0u: {                                  /* RBR */
        uint8_t c = uart_rx_pop();
        v16550_reeval();
        return c;
    }
    case 1u:
        return uart16550.ier;
    case 2u: {                                  /* IIR */
        /* Returning 0 here means "modem status interrupt pending", which the
         * driver has no handler for. Enough of those and Linux prints
         * "irq N: nobody cared" and disables the line permanently. */
        uint8_t iir;
        if (((uart16550.ier & 0x01u) != 0u) && !uart_rx_empty()) {
            iir = 0x04u;                        /* received data available */
        } else if ((uart16550.ier & 0x02u) != 0u) {
            iir = 0x02u;                        /* transmitter holding empty */
        } else {
            iir = 0x01u;                        /* no interrupt pending */
        }
        return iir | (((uart16550.fcr & 0x01u) != 0u) ? 0xC0u : 0x00u);
    }
    case 3u:
        return uart16550.lcr;
    case 4u:
        return uart16550.mcr;
    case 5u:
        /* LSR. serial8250 spins until (lsr & 0x60) == 0x60: THR empty AND
         * transmitter idle. Bit 0 is DR, driven by the ring. */
        return 0x60u | (uart_rx_empty() ? 0x00u : 0x01u);
    case 6u:
        /* MSR: DCD|DSR|CTS asserted so flow control never blocks. In loopback
         * the driver expects MCR's low bits reflected back. */
        if ((uart16550.mcr & 0x10u) != 0u) {
            return (uint8_t)(((uart16550.mcr & 0x0Cu) << 4) |
                             ((uart16550.mcr & 0x02u) << 3) |
                             ((uart16550.mcr & 0x01u) << 5));
        }
        return 0xB0u;
    case 7u:
        return uart16550.scr;
    default:
        return 0u;
    }
}

static void uart_reg_write(uint32_t offset, uint8_t value)
{
    if ((uart16550.lcr & 0x80u) != 0u) {        /* DLAB: divisor latches */
        if (offset == 0u) { uart16550.dll = value; return; }
        if (offset == 1u) { uart16550.dlm = value; return; }
    }

    switch (offset) {
    case 0u:                                    /* THR */
        if ((uart16550.mcr & 0x10u) != 0u) {
            uart_rx_push(value);                /* loopback: back to our own RX */
        } else {
#if HYP_UART_LOG_BLOCKING
            while (esp_rom_output_tx_one_char(value) != 0) { }
#else
            /* Straight at the FIFO, and dropped if it is full. Deliberately not
             * esp_rom_output_tx_one_char(): that waits for room. */
            uart_dev_t *tx = UART_LL_GET_HW(0);
            if (uart_ll_get_txfifo_len(tx) > 0u) {
                uint8_t byte = (uint8_t)value;
                uart_ll_write_txfifo(tx, &byte, 1u);
            } else {
                uart16550.tx_dropped++;
            }
#endif
        }
        v16550_reeval();
        break;
    case 1u:
        uart16550.ier = value;
        v16550_reeval();    /* enabling ETBEI asserts the line immediately: there
                             * is no external event to trigger it later */
        break;
    case 2u:
        uart16550.fcr = value;                  /* FCR (write side of IIR) */
        break;
    case 3u:
        uart16550.lcr = value;
        break;
    case 4u:
        uart16550.mcr = value;
        break;
    case 7u:
        uart16550.scr = value;
        break;
    default:
        break;
    }
}

static uint32_t uart_read(uint32_t offset, uint32_t width)
{
    uint32_t value = 0u;
    for (uint32_t i = 0u; i < width; i++) {
        value |= (uint32_t)uart_reg_read(offset + i) << (8u * i);
    }
    return value;
}

static void uart_write(uint32_t offset, uint32_t value, uint32_t width)
{
    for (uint32_t i = 0u; i < width; i++) {
        uart_reg_write(offset + i, (uint8_t)(value >> (8u * i)));
    }
}


/* ===========================================================================
 * Shadow mip, and the single interrupt delivery point
 * ===========================================================================
 */

/* mip bits 7 and 11 are wires, not storage. Synthesise them on read; drop
 * writes to them. */
static uint32_t shadow_mip_read(void)
{
    uint32_t mip = shadow.mip & ~(MIP_MTIP | MIP_MEIP);
    if (mtip_pending()) { mip |= MIP_MTIP; }
    if (meip_pending()) { mip |= MIP_MEIP; }
    return mip;
}

/* Highest-priority deliverable interrupt, or -1. RISC-V priority within a
 * privilege level is MEI(11) > MSI(3) > MTI(7). */
static int hyp_pending_cause(void)
{
    if ((shadow.mstatus & MSTATUS_MIE) == 0u) {
        return -1;                              /* guest global gate closed */
    }
    if ((shadow.mie & MIP_MEIP) != 0u && meip_pending()) { return 11; }
    if ((shadow.mie & MIP_MSIP) != 0u && (shadow.mip & MIP_MSIP) != 0u) { return 3; }
    if ((shadow.mie & MIP_MTIP) != 0u && mtip_pending()) { return 7; }
    return -1;
}

/* Synthesise a trap into the guest: what hardware would do, in software.
 *
 * `cause` already carries bit 31 for interrupts. mepc is NOT advanced -- for an
 * exception the guest's handler does that itself (the syscall path does
 * epc += 4), and for an interrupt the instruction has already retired.
 *
 * The real mstatus is untouched. The guest's "M-mode" is U-mode hardware; only
 * guest_priv and shadow.mstatus move. */
static void hyp_inject(RvExcFrame *frame, uint32_t cause, uint32_t tval)
{
    uint32_t status = shadow.mstatus;

    status = (status & ~MSTATUS_MPIE) | ((status & MSTATUS_MIE) ? MSTATUS_MPIE : 0u);
    status &= ~MSTATUS_MIE;
    status = (status & ~MSTATUS_MPP) | (guest_priv << MSTATUS_MPP_S);
    shadow.mstatus = status;

    shadow.mepc   = (uint32_t)frame->mepc;
    shadow.mcause = cause;
    shadow.mtval  = tval;

    guest_priv  = PRIV_M;
    frame->mepc = shadow.mtvec & ~0x3u;   /* Linux M-mode uses direct mode */
    hyp_stats.injections++;
}

/* The one place interrupts are delivered.
 *
 * Called at the tail of hypervisor(), AFTER mepc has been advanced -- injection
 * saves frame->mepc into shadow.mepc, so running it earlier makes the guest
 * re-execute the emulated instruction on mret.
 *
 * One site is sufficient because every transition that can open the gate is
 * itself a trap: csrw mstatus/mie and mret are mcause 2; mtimecmp and PLIC
 * writes are mcause 7; and a newly-pending condition during a trap-free guest
 * loop is what the host interrupts exist to manufacture. */
static void hyp_deliver_pending(RvExcFrame *frame)
{
    int cause = hyp_pending_cause();
    if (cause < 0 || shadow.mtvec == 0u) {
        return;     /* guest has no trap vector yet: nothing safe to jump to */
    }
    hyp_inject(frame, 0x80000000u | (uint32_t)cause, 0u);
}


/* ===========================================================================
 * Host device servicing
 * ===========================================================================
 */

/* Idempotent and storm-proof: always clears what it services. Shared by the
 * host-interrupt path and the wfi wait loop.
 *
 * Both sources are level-triggered. Failing to clear the device status means
 * the line re-asserts the instant we mret, and the board wedges in the trap
 * handler with no output. */
static void hyp_poll_host_devices(void)
{
    if (systimer_ll_is_alarm_int_fired(&SYSTIMER, HYP_SYSTIMER_ALARM)) {
        /* One-shot has fired; disarm and ack. Nothing else to do -- MTIP is
         * recomputed from mtime >= mtimecmp, which is still true. */
        systimer_ll_enable_alarm(&SYSTIMER, HYP_SYSTIMER_ALARM, false);
        systimer_ll_clear_alarm_int(&SYSTIMER, HYP_SYSTIMER_ALARM);
    }

    uart_dev_t *hw = UART_LL_GET_HW(0);
    uint32_t status = uart_ll_get_intsts_mask(hw);
    if ((status & (UART_INTR_RXFIFO_FULL | UART_INTR_RXFIFO_TOUT | UART_INTR_RXFIFO_OVF)) != 0u) {
        uint32_t n = uart_ll_get_rxfifo_len(hw);
        while (n-- > 0u) {
            uint8_t c;
            uart_ll_read_rxfifo(hw, &c, 1);
            if (c == HYP_KEY_STATS_DUMP) {
                hyp_stats_wanted = true;
            } else if (c == HYP_KEY_KBD_TEST) {
                hyp_kbd_selftest();
            } else {
                uart_rx_push(c);
            }
        }
        uart_ll_clr_intsts_mask(hw, UART_INTR_RXFIFO_FULL | UART_INTR_RXFIFO_TOUT |
                                    UART_INTR_RXFIFO_OVF);
    }

    /* The USB keyboard, decoded on core 0 and drained here.
     *
     * Done on this side rather than by having core 0 push straight into the
     * 16550 ring, so that core 1 stays the only writer of that ring and neither
     * side needs a lock. The cost is latency of at most one trap, and the guest
     * takes timer traps continuously. */
    uint8_t key;
    while (hyp_kbd_pop(&key)) {
        uart_rx_push(key);
    }

    v16550_reeval();
}


/* ===========================================================================
 * CSR emulation
 *
 * Every instruction here shares opcode 0x73 (SYSTEM). They are told apart by
 * funct3 alone:
 *
 *   funct3 | instruction | operation              | where the value comes from
 *   -------+-------------+------------------------+---------------------------
 *      0   | ecall / ebreak / mret / wfi -- no CSR field at all; bits [31:20]
 *          |               name the instruction, so these must be compared as
 *          |               whole 32-bit words
 *      1   | csrrw       | rd = csr; csr  =  src  | register rs1
 *      2   | csrrs       | rd = csr; csr |=  src  | register rs1
 *      3   | csrrc       | rd = csr; csr &= ~src  | register rs1
 *      4   |  -- unused --
 *      5   | csrrwi      | rd = csr; csr  =  src  | rs1 field as a 5-bit
 *      6   | csrrsi      | rd = csr; csr |=  src  |   unsigned immediate
 *      7   | csrrci      | rd = csr; csr &= ~src  |
 *
 * so funct3 factors into two independent fields:
 *   funct3 & 3  ->  1 swap, 2 set, 3 clear
 *   funct3 & 4  ->  set means the rs1 field is an immediate, not a register
 *
 * Instruction fields, all of them:
 *   [31:20] csr number      [19:15] rs1 (or the 5-bit immediate)
 *   [14:12] funct3          [11:7]  rd            [6:0] opcode 0x73
 *
 * csrw and csrr are NOT instructions. They are pseudo-instructions built on x0:
 *   csrr t0, mtvec  ->  0x305022f3  ->  csrrs t0, mtvec, x0   (funct3 2, rs1=x0)
 *   csrw mtvec, t1  ->  0x30531073  ->  csrrw x0, mtvec, t1   (funct3 1, rd =x0)
 * There is no encoding that means "csrr", so this code implements the six real
 * instructions and lets the x0 cases fall out.
 * ===========================================================================
 */

/* Make guest-written code visible to the instruction fetch.
 *
 * Linux writes code and then calls flush_icache_range(), which on riscv is
 * `fence.i`. That invalidates the I-cache but does NOT write back the D-cache,
 * and on this core the instruction fetch does not see dirty D-cache lines. So
 * the bytes are still sitting in L1 D while the fetch reads the stale line
 * underneath them.
 *
 * It cost a boot to find. Linux delivers a signal to init by writing a two
 * instruction sigreturn trampoline into the signal frame ON THE USER STACK and
 * mret-ing to it. The fetch got the stack slot's previous contents, which
 * decoded as a load, and the guest died on a load access fault at 0x7a whose
 * mepc pointed at `li a7, 139` -- an instruction that cannot fault that way.
 * That mismatch between the executed word and the word in memory is the whole
 * signature of the bug.
 *
 * `fence.i` is opcode 0x0f, a legal instruction, so it does not trap and cannot
 * be hooked. This runs on every mret instead: the writeback pushes L1 D into
 * L2, and invalidating L1 I forces the fetch to go get it. L2 itself is not
 * touched -- the fetch reaches it, so the data need not go all the way out to
 * PSRAM.
 *
 * This is a sledgehammer: a full-cache pair of operations on every return to
 * the guest, i.e. once per syscall. It is here to prove the diagnosis. The
 * replacement is a hypercall in the guest's local_flush_icache_all(), which
 * would do this once per actual flush instead of once per trap. */
static inline void hyp_sync_icache(void)
{
    uint32_t t0 = rv_utils_get_cycle_count();
    cache_ll_l1_writeback_dcache_all(CACHE_LL_ID_ALL);
    cache_ll_l1_invalidate_icache_all(CACHE_LL_ID_ALL);
    hyp_stats.icache_syncs++;
    hyp_stats.icache_sync_cycles += (uint32_t)(rv_utils_get_cycle_count() - t0);
}

static TrapResult hypervisor_mret(GuestTrap *trap)
{
    uint32_t status = shadow.mstatus;

    guest_priv = (status & MSTATUS_MPP) >> MSTATUS_MPP_S;
    status = (status & ~MSTATUS_MIE) | ((status & MSTATUS_MPIE) ? MSTATUS_MIE : 0u);
    status |= MSTATUS_MPIE;
    status &= ~MSTATUS_MPP;                      /* MPP <- U, per spec */
    shadow.mstatus = status;

    if (shadow.mepc == 0u) {
        /* Nothing was ever injected, so there is nothing to return from. Jumping
         * to 0 would be an unrecoverable mystery; name it instead. */
        return TRAP_UNHANDLED;
    }

    hyp_sync_icache();
    hyp_stats.mrets++;

    trap->frame->mepc = shadow.mepc;
    return TRAP_RESUME;   /* a jump, not a skip: mepc must not be advanced on top */
}

/* Whether a real wfi would wake, which is NOT the same question as whether an
 * interrupt can be delivered.
 *
 * The RISC-V spec is explicit that wfi's wake condition ignores mstatus.MIE: a
 * pending, mie-enabled interrupt resumes the hart whether or not the global gate
 * is open. hyp_pending_cause() gates on mstatus.MIE because *delivery* does.
 * Conflating the two is a bug that cost 16% of all traps -- see below. */
static bool hyp_wfi_wake(void)
{
    if ((shadow.mie & MIP_MEIP) != 0u && meip_pending()) { return true; }
    if ((shadow.mie & MIP_MSIP) != 0u && (shadow.mip & MIP_MSIP) != 0u) { return true; }
    if ((shadow.mie & MIP_MTIP) != 0u && mtip_pending()) { return true; }
    return false;
}

/* True while the guest is parked in a real wfi. Read by core 0's liveness
 * check, which would otherwise report an idle guest as a wedged one: a guest
 * blocked in wfi takes no traps at all, which is exactly the signature the
 * check looks for. */
volatile bool hyp_in_wfi = false;

/* Cycles spent asleep inside the current trap, so they can be subtracted from
 * what that trap is charged.
 *
 * Without this the accounting is actively misleading: a blocking wfi is
 * measured as time in the handler, so the first histogram taken after wfi
 * started working reported 27,291 cycles per illegal-instruction trap -- which
 * is the guest sleeping, not the monitor working. A cost metric that goes up
 * when you make the system faster is worse than no metric. */
static uint32_t hyp_idle_cycles;

/* wfi: block in the hypervisor rather than spinning the guest.
 *
 * We are inside a trap, so mstatus.MIE == 0 and a real wfi wakes on a pending
 * CLIC line WITHOUT vectoring -- we resume at the next instruction, poll, and
 * decide. No re-entrancy, no nesting.
 *
 * The gate used to include (shadow.mstatus & MSTATUS_MIE), on the assumption
 * that Linux idles with interrupts enabled. The trap histogram says otherwise:
 * 1,903,285 wfi traps spun and ZERO blocked, i.e. this path never once did its
 * job and one trap in six was pure waste. It is wrong for the reason
 * hyp_wfi_wake() describes -- the guest reaches wfi with its own gate shut and
 * relies on the hardware waking it anyway.
 *
 * What is still required before blocking is that some host interrupt can
 * actually arrive. A timer the guest has disarmed (mtimecmp at ~0) plus a PLIC
 * context it has not enabled means nothing will ever fire, and a wfi there
 * would be an unrecoverable hang with no diagnostic. */
static TrapResult hypervisor_wfi(void)
{
#if !HYP_WFI_BLOCKS
    return TRAP_ADVANCE;
#else
    bool timer_armed = ((shadow.mie & MIP_MTIP) != 0u) &&
                       (shadow_mtimecmp < HYP_MTIME_MAX);
    bool ext_armed   = ((shadow.mie & MIP_MEIP) != 0u);

    if (!timer_armed && !ext_armed) {
        hyp_stats.wfi_spins++;
        return TRAP_ADVANCE;
    }

    hyp_stats.wfi_blocks++;
    hyp_in_wfi = true;
    uint32_t asleep = rv_utils_get_cycle_count();
    while (!hyp_wfi_wake()) {
        asm volatile ("wfi");
        hyp_poll_host_devices();
    }
    hyp_in_wfi = false;
    asleep = rv_utils_get_cycle_count() - asleep;
    hyp_idle_cycles += asleep;
    hyp_stats.idle_cycles += asleep;
    return TRAP_ADVANCE;
#endif
}

static TrapResult hypervisor_csr(GuestTrap *trap)
{
    /* cause 2, so this is free: mtval already holds the instruction. */
    if (!trap_decode(trap)) {
        return TRAP_UNHANDLED;
    }

    uint32_t instruction = trap->encoding;

    if ((instruction & 0x7fu) != 0x73u) {
        return TRAP_UNHANDLED;    /* an illegal instruction that is not a SYSTEM op */
    }

    uint32_t funct3 = (instruction >> 12) & 0x7u;

    /* No CSR field, no rd, no rs1 -- compare the whole word. */
    if (funct3 == 0u) {
        switch (instruction) {
        case 0x30200073u:  return hypervisor_mret(trap);
        case 0x10500073u:  return hypervisor_wfi();
        default:           return TRAP_UNHANDLED;
        }
    }
    if (funct3 == 4u) {
        return TRAP_UNHANDLED;    /* not a defined CSR encoding */
    }

    uint32_t csr = instruction >> 20;
    uint32_t rd  = (instruction >>  7) & 0x1fu;
    uint32_t rs1 = (instruction >> 15) & 0x1fu;

    uint32_t src = (funct3 & 0x4u) ? rs1 : reg_read(trap->frame, rs1);

    uint32_t old;
    uint32_t *slot = shadow_slot(csr);

    if (csr == CSR_MIP) {
        /* Bits 7 and 11 are computed; the rest is storage. Writes to the
         * computed bits are dropped, as on hardware. */
        old = shadow_mip_read();
        if (((funct3 & 0x3u) == 1u) || (rs1 != 0u)) {
            uint32_t updated;
            switch (funct3 & 0x3u) {
            case 1u:  updated = src;        break;
            case 2u:  updated = old |  src; break;
            case 3u:  updated = old & ~src; break;
            default:  updated = old;        break;
            }
            shadow.mip = updated & ~(MIP_MTIP | MIP_MEIP);
        }
    } else if (slot != null) {
        old = *slot;

        /* The suppression rules are asymmetric, and both halves matter:
         *   csrrs/csrrc with rs1 == x0 (or uimm == 0) must NOT write -- that is
         *     exactly what makes `csrr` a pure read;
         *   csrrw/csrrwi ALWAYS write, even when the value is zero, so
         *     `csrw mtvec, x0` must not be dropped.
         * Simplifying this to `rs1 != 0` would silently break the second case. */
        if (((funct3 & 0x3u) == 1u) || (rs1 != 0u)) {
            switch (funct3 & 0x3u) {
            case 1u: *slot = src;        break;   /* csrrw / csrrwi */
            case 2u: *slot = old |  src; break;   /* csrrs / csrrsi */
            case 3u: *slot = old & ~src; break;   /* csrrc / csrrci */
            default: break;
            }
        }
    } else if (csr_read_only_zero(csr)) {
        old = 0u;
    } else {
        /* Deliberately not "return 0 and hope". An unimplemented CSR that reads
         * as zero is indistinguishable from a correct one until something reads
         * back what it wrote, and by then the failure has moved somewhere else.
         * Panicking here names the CSR number on the console instead. */
        return TRAP_UNHANDLED;
    }

    if (((funct3 & 0x3u) == 1u) || (rs1 != 0u)) {
        hyp_stats.csr_write++;
    } else {
        hyp_stats.csr_read_only++;
    }

    /* Unconditional: reg_write discards index 0, which is what makes the
     * rd == x0 form of csrw come out right. */
    reg_write(trap->frame, rd, old);
    return TRAP_ADVANCE;
}


/* ===========================================================================
 * MMIO dispatch
 *
 * The device table is the reason this is split out. Before, hypervisor_mmio
 * bounds-checked the UART by name and called uart_read/uart_write directly, so
 * every new device meant editing the middle of the load/store decoder. Now a
 * device is one row here and the decoder never changes.
 * ===========================================================================
 */

typedef struct {
    uint32_t    base;
    uint32_t    size;
    uint32_t  (*read)(uint32_t offset, uint32_t width);
    void      (*write)(uint32_t offset, uint32_t value, uint32_t width);
    const char *name;
} MmioDevice;

static const MmioDevice mmio_devices[] = {
    { HYP_CLINT_BASE, HYP_CLINT_SIZE, clint_read, clint_write, "clint" },
    { HYP_UART_BASE,  HYP_UART_SIZE,  uart_read,  uart_write,  "uart"  },
    { HYP_PLIC_BASE,  HYP_PLIC_SIZE,  plic_read,  plic_write,  "plic"  },
};

static const MmioDevice *mmio_find(uint32_t address)
{
    for (uint32_t i = 0u; i < sizeof(mmio_devices) / sizeof(mmio_devices[0]); i++) {
        const MmioDevice *device = &mmio_devices[i];
        if (address >= device->base && address < device->base + device->size) {
            return device;
        }
    }
    return null;
}

static TrapResult hypervisor_mmio(GuestTrap *trap)
{
    /* mtval is the faulting address on causes 5 and 7. */
    const MmioDevice *device = mmio_find(trap->tval);
    if (device == null) {
        return TRAP_UNHANDLED;    /* a genuine bad access, not a device window */
    }
    if (!trap_decode(trap)) {
        return TRAP_UNHANDLED;
    }

    uint32_t instruction = trap->encoding;

    /* Loads are opcode 0x03 and stores 0x23: the same seven bits apart from
     * bit 5. Masking with 0x5f accepts both and rejects everything else. */
    if ((instruction & 0x5fu) != 0x03u) {
        return TRAP_UNHANDLED;
    }

    hyp_stats.mmio[(uint32_t)(device - mmio_devices) & 0x3u]++;

    uint32_t offset = trap->tval - device->base;
    uint32_t rd  = (instruction >>  7) & 0x1fu;   /* loads:  destination  */
    uint32_t rs2 = (instruction >> 20) & 0x1fu;   /* stores: value source */
    uint32_t src = reg_read(trap->frame, rs2);

    /* Dense key: funct3 in bits 0-2, opcode bit 5 (load vs store) in bit 3.
     * Verified against the assembler: lb/lh/lw/lbu/lhu -> 0,1,2,4,5 and
     * sb/sh/sw -> 8,9,a. */
    switch (((instruction >> 12) & 0x7u) | ((instruction >> 2) & 0x8u)) {
    /* Loads write rd. The sign-extending casts are the whole difference between
     * lb/lh and lbu/lhu -- note that readb() compiles to lb, not lbu, so the
     * signed forms are not theoretical. */
    case 0x0u: reg_write(trap->frame, rd, (uint32_t)(int32_t)(int8_t) device->read(offset, 1u)); break;  /* lb  */
    case 0x1u: reg_write(trap->frame, rd, (uint32_t)(int32_t)(int16_t)device->read(offset, 2u)); break;  /* lh  */
    case 0x2u: reg_write(trap->frame, rd,                             device->read(offset, 4u)); break;  /* lw  */
    case 0x4u: reg_write(trap->frame, rd,                             device->read(offset, 1u)); break;  /* lbu */
    case 0x5u: reg_write(trap->frame, rd,                             device->read(offset, 2u)); break;  /* lhu */

    /* Stores have no rd, so they simply do not write one. */
    case 0x8u: device->write(offset, src, 1u); break;   /* sb */
    case 0x9u: device->write(offset, src, 2u); break;   /* sh */
    case 0xau: device->write(offset, src, 4u); break;   /* sw */

    default:   return TRAP_UNHANDLED;
    }

    return TRAP_ADVANCE;
}


/* ===========================================================================
 * Trap dispatch
 * ===========================================================================
 */

/* Trap accounting.
 *
 * The trace ring below answers "what just happened"; this answers "where does
 * the time go", which is the question every optimisation of this monitor
 * actually turns on. Without it, every claim about cost is a guess: nobody can
 * say from the outside whether a boot is spending its cycles on UART MMIO, on
 * CSR emulation, or on the cache maintenance in hyp_sync_icache().
 *
 * Cost per trap is one cycle-counter read on entry, one on exit, and two adds.
 * rv_utils_get_cycle_count() is a csrr, so this is cheap enough to leave on
 * permanently -- and a measurement you have to rebuild to enable is a
 * measurement nobody takes.
 *
 * Read out by core 0 over the physical UART, which is no longer the guest's
 * terminal, so the dump does not scribble on the display.
 */
/* Trap history.
 *
 * An injected access fault arrived whose mtval and mepc disagreed: mtval said a
 * load from 0x7a, while the instruction at mepc was `li a7, 139`, which cannot
 * fault on a load. One trap line cannot say which of the two is lying, but the
 * traps immediately before it can -- a signal being delivered, an mret, or a
 * run of ecalls each imply a different story.
 *
 * Recording is four stores per trap. The dump only runs on an injected access
 * fault, which is rare, and is capped because it prints on the same UART the
 * guest console uses. */
#define HYP_TRACE_LEN   16u
#define HYP_TRACE_DUMPS 4u

static struct {
    uint32_t cause;
    uint32_t epc;
    uint32_t tval;
    uint32_t priv;
} hyp_trace[HYP_TRACE_LEN];
static uint32_t hyp_trace_next;
static uint32_t hyp_trace_total;
static uint32_t hyp_trace_dumps;

static void hyp_trace_record(uint32_t cause, uint32_t epc, uint32_t tval)
{
    uint32_t i = hyp_trace_next;
    hyp_trace[i].cause = cause;
    hyp_trace[i].epc   = epc;
    hyp_trace[i].tval  = tval;
    hyp_trace[i].priv  = guest_priv;
    hyp_trace_next = (i + 1u) % HYP_TRACE_LEN;
    hyp_trace_total++;
}

/* Oldest first, so the last line is the trap being reported. The instruction is
 * fetched per entry rather than recorded, because what matters is whether the
 * word at that pc is a load/store at all -- and by dump time the guest has not
 * had a chance to change it. */
static void hyp_trace_dump(const char *why)
{
    if (hyp_trace_dumps >= HYP_TRACE_DUMPS) {
        return;
    }
    hyp_trace_dumps++;

    uint32_t count = (hyp_trace_total < HYP_TRACE_LEN) ? hyp_trace_total : HYP_TRACE_LEN;
    uint32_t start = (hyp_trace_next + HYP_TRACE_LEN - count) % HYP_TRACE_LEN;

    esp_rom_printf("\nHYP trace (%s), %u traps, oldest first:\n", why, (unsigned)hyp_trace_total);
    for (uint32_t n = 0u; n < count; n++) {
        uint32_t i = (start + n) % HYP_TRACE_LEN;
        uint32_t epc = hyp_trace[i].epc;
        uint32_t encoding = 0u;
        uint32_t length = 0u;

        if (epc >= linux_base_address && epc < linux_base_address + linux_size) {
            (void)guest_fetch(epc, &encoding, &length);
        }

        /* Loads are opcode 0x03 and stores 0x23 -- the same seven bits apart
         * from bit 5, so 0x5f accepts both. Compressed forms are checked
         * separately: quadrant 0 funct3 2/6 is c.lw/c.sw. */
        bool mem = false;
        if (length == 4u) {
            mem = (encoding & 0x5fu) == 0x03u;
        } else if (length == 2u) {
            uint32_t q = encoding & 0x3u;
            uint32_t f = (encoding >> 13) & 0x7u;
            mem = (q == 0u) || ((q == 2u) && (f == 2u || f == 6u));
        }

        esp_rom_printf("  %u: cause=%u epc=0x%08x tval=0x%08x priv=%u insn=0x%08x/%u%s\n",
                       (unsigned)n, (unsigned)hyp_trace[i].cause, (unsigned)epc,
                       (unsigned)hyp_trace[i].tval, (unsigned)hyp_trace[i].priv,
                       (unsigned)encoding, (unsigned)length,
                       mem ? "" : ((hyp_trace[i].cause == 5u || hyp_trace[i].cause == 7u)
                                   ? "  <- access fault, but not a load/store" : ""));
    }
}

/* Where the time goes. Printed on the physical UART by core 0.
 *
 * Cycles are the CPU's own, at 360 MHz on this part, and the per-trap average
 * is the number that matters: it is what any change to a handler moves. The
 * icache line is broken out on purpose -- hyp_sync_icache() runs a full-cache
 * writeback plus an I-cache invalidate on every single mret, which is the one
 * cost in here that was always suspected and never measured. */
static const char *hyp_cause_name(uint32_t cause)
{
    switch (cause) {
    case 0u:  return "insn-align";
    case 1u:  return "insn-fault";
    case 2u:  return "illegal    ";
    case 3u:  return "breakpoint ";
    case 4u:  return "load-align ";
    case 5u:  return "load-fault ";
    case 6u:  return "store-align";
    case 7u:  return "store-fault";
    case 8u:  return "ecall-U    ";
    case 11u: return "ecall-M    ";
    default:  return "other      ";
    }
}

void hyp_stats_dump(void)
{
    uint64_t total = 0u;
    uint32_t traps = 0u;
    for (uint32_t i = 0u; i < HYP_STATS_CAUSES; i++) {
        total += hyp_stats.exc_cycles[i] + hyp_stats.irq_cycles[i];
        traps += hyp_stats.exc[i] + hyp_stats.irq[i];
    }
    if (traps == 0u) {
        return;
    }

    esp_rom_printf("\nHYP stats: %u traps, %u kcycles in the monitor "
                   "(%u cycles/trap avg)\n",
                   (unsigned)traps, (unsigned)(total / 1000u),
                   (unsigned)(total / traps));

    for (uint32_t i = 0u; i < HYP_STATS_CAUSES; i++) {
        if (hyp_stats.exc[i] != 0u) {
            esp_rom_printf("  exc %2u %s %8u  %6u cyc/ea\n", (unsigned)i,
                           hyp_cause_name(i), (unsigned)hyp_stats.exc[i],
                           (unsigned)(hyp_stats.exc_cycles[i] / hyp_stats.exc[i]));
        }
    }
    for (uint32_t i = 0u; i < HYP_STATS_CAUSES; i++) {
        if (hyp_stats.irq[i] != 0u) {
            /* An interrupt's mcause code is the CLIC inum on this part, so
             * these line up with HYP_INUM_TIMER and HYP_INUM_UART. */
            esp_rom_printf("  irq %2u %s %8u  %6u cyc/ea\n", (unsigned)i,
                           (i == HYP_INUM_TIMER + HYP_CLIC_MCAUSE_BASE) ? "host-timer " :
                           (i == HYP_INUM_UART  + HYP_CLIC_MCAUSE_BASE) ? "host-uart  "
                                                                        : "host-other ",
                           (unsigned)hyp_stats.irq[i],
                           (unsigned)(hyp_stats.irq_cycles[i] / hyp_stats.irq[i]));
        }
    }

    esp_rom_printf("  mmio clint=%u uart=%u plic=%u | csr rd=%u wr=%u | "
                   "mret=%u inject=%u\n",
                   (unsigned)hyp_stats.mmio[0], (unsigned)hyp_stats.mmio[1],
                   (unsigned)hyp_stats.mmio[2],
                   (unsigned)hyp_stats.csr_read_only, (unsigned)hyp_stats.csr_write,
                   (unsigned)hyp_stats.mrets, (unsigned)hyp_stats.injections);
    esp_rom_printf("  last raw interrupt mcause code: %u\n",
                   (unsigned)hyp_stats.irq_code_seen);
    esp_rom_printf("  wfi blocked=%u spun=%u | unhandled=%u | "
                   "guest idle %u kcycles (not charged above)\n",
                   (unsigned)hyp_stats.wfi_blocks, (unsigned)hyp_stats.wfi_spins,
                   (unsigned)hyp_stats.unhandled,
                   (unsigned)(hyp_stats.idle_cycles / 1000u));
    if (hyp_stats.icache_syncs != 0u) {
        uint64_t ic = hyp_stats.icache_sync_cycles;
        esp_rom_printf("  icache sync: %u calls, %u kcycles, %u cyc/ea, "
                       "%u%% of monitor time\n",
                       (unsigned)hyp_stats.icache_syncs, (unsigned)(ic / 1000u),
                       (unsigned)(ic / hyp_stats.icache_syncs),
                       (unsigned)((ic * 100u) / (total ? total : 1u)));
    }
    esp_rom_printf("  uart rx overruns: %u, tx dropped: %u | keyboards: %u, "
                   "hid reports: %u, dropped keys: %u\n",
                   (unsigned)uart16550.overruns, (unsigned)uart16550.tx_dropped,
                   (unsigned)hyp_kbd_devices(),
                   (unsigned)hyp_kbd_reports(), (unsigned)hyp_kbd_overruns());
}

void hyp_panic(RvExcFrame *frame)
{
    uint32_t mepc = (uint32_t)frame->mepc;
    uint32_t encoding = 0u;
    uint32_t length = 0u;

    /* Only fetch if mepc is inside the guest window: a fetch that faults would
     * re-enter the trap handler and never come back. */
    if (mepc >= linux_base_address && mepc < linux_base_address + linux_size) {
        (void)guest_fetch(mepc, &encoding, &length);
    }

    esp_rom_printf("\nHYP mcause=0x%08x mepc=0x%08x mtval=0x%08x insn=0x%08x/%u "
                   "guest_sp=0x%08x mstatus=0x%08x priv=%u core=%u\n",
                   (unsigned)frame->mcause, (unsigned)mepc,
                   (unsigned)frame->mtval, (unsigned)encoding, (unsigned)length,
                   (unsigned)frame->sp, (unsigned)frame->mstatus,
                   (unsigned)guest_priv, (unsigned)rv_utils_get_core_id());
    hyp_trace_dumps = 0u;               /* a panic ends the run: always worth the dump */
    hyp_trace_dump("panic");
    while (1) { }
}

static TrapResult hypervisor_trap(GuestTrap *trap)
{
    switch (trap->cause) {
    case 2u:            /* illegal instruction: CSR ops, mret, wfi */
        return hypervisor_csr(trap);
    case 3u:            /* breakpoint: ebreak / c.ebreak (Linux WARN) */
        if (shadow.mtvec == 0u) { return TRAP_UNHANDLED; }
        hyp_inject(trap->frame, 3u, (uint32_t)trap->frame->mepc);
        return TRAP_RESUME;
    case 5u:            /* load access fault  */
    case 7u:            /* store access fault */
        /* Two very different things arrive here. An access inside a device
         * window is ours to emulate. An access anywhere else is a real fault,
         * and the guest already knows what to do with it: for U-mode that is
         * SIGSEGV, for its own kernel an oops with a pc and an address -- both
         * far more informative than hyp_panic, which until now killed the
         * machine over an ordinary userspace null dereference.
         *
         * The device-window case is kept separate on purpose: if mmio_find hits
         * but the decoder bails (an addressing form not implemented yet), that
         * is a monitor bug, and injecting a fault the guest cannot explain
         * would bury it. Those still panic. */
        if (mmio_find(trap->tval) != null) {
            return hypervisor_mmio(trap);
        }
        if (shadow.mtvec == 0u) {
            return TRAP_UNHANDLED;    /* no guest vector yet: would jump to 0 */
        }
        hyp_trace_dump("injecting access fault");
        hyp_inject(trap->frame, trap->cause, trap->tval);
        return TRAP_RESUME;
    case 8u:
        /* Hardware always reports 8 because the whole guest runs in U-mode.
         * This kernel's excp_vect_table[8] IS the syscall dispatcher and it
         * requires MPP == 0; index 11 is "Oops - environment call from M-mode".
         * So always inject 8, and let guest_priv feed MPP. */
        if (shadow.mtvec == 0u) { return TRAP_UNHANDLED; }
        hyp_inject(trap->frame, 8u, 0u);
        return TRAP_RESUME;
    default:
        return TRAP_UNHANDLED;
    }
}

bool hypervisor(RvExcFrame *frame)
{
    if (!linux_running) {
        return false;
    }

    uint32_t mcause = (uint32_t)frame->mcause;
    uint32_t entry_cycles = rv_utils_get_cycle_count();
    hyp_idle_cycles = 0u;

    hyp_trace_record(mcause, (uint32_t)frame->mepc, (uint32_t)frame->mtval);

    if ((mcause & 0x80000000u) != 0u) {
        /* A host interrupt. Its only job is to have manufactured this trap;
         * servicing updates virtual device state, and delivery happens below
         * like it does for every other trap. */
        /* The interrupt's mcause code. Expected to be the CLIC inum (20 or 21
         * here), but the first histogram showed it landing in the clamped last
         * bucket, i.e. >= 32 -- so the P4's CLIC does not report what the plain
         * reading of the spec suggests. Kept as the raw value plus a clamp, and
         * the raw value is printed, because guessing at it once already
         * produced a mislabelled row. */
        uint32_t code = (mcause & 0x7fffffffu);
        uint32_t slot = (code < HYP_STATS_CAUSES) ? code : (HYP_STATS_CAUSES - 1u);
        hyp_stats.irq_code_seen = code;
        hyp_poll_host_devices();
        hyp_deliver_pending(frame);
        hyp_stats.irq[slot]++;
        hyp_stats.irq_cycles[slot] +=
            (uint32_t)(rv_utils_get_cycle_count() - entry_cycles) - hyp_idle_cycles;
        return true;
    }

    GuestTrap trap = {
        .frame = frame,
        .cause = mcause,
        .tval  = (uint32_t)frame->mtval,
        .encoding = 0u,
        .length = 0u,
    };

    TrapResult outcome = hypervisor_trap(&trap);

    {
        uint32_t slot = (mcause < HYP_STATS_CAUSES) ? mcause : HYP_STATS_CAUSES - 1u;
        hyp_stats.exc[slot]++;
        hyp_stats.exc_cycles[slot] +=
            (uint32_t)(rv_utils_get_cycle_count() - entry_cycles) - hyp_idle_cycles;
        if (outcome == TRAP_UNHANDLED) { hyp_stats.unhandled++; }
    }

    switch (outcome) {
    case TRAP_ADVANCE:
        if (trap.length == 0u) {
            /* A handler returned ADVANCE without decoding. Advancing by zero is
             * a silent infinite loop on the same instruction -- panic instead. */
            return false;
        }
        frame->mepc += trap.length;
        break;
    case TRAP_RESUME:
        break;
    default:
        return false;
    }

    hyp_deliver_pending(frame);
    return true;
}


/* ===========================================================================
 * Host bring-up (runs on core 1, where the guest lives)
 * ===========================================================================
 */

/* Rip out everything IDF armed on this core.
 *
 * Not optional hygiene. esp_cache_err_int_init() routed ETS_CACHE_INTR_SOURCE
 * to ETS_CACHEERR_INUM and enabled it, and IDF's _mtvt_table entry for it is
 * _panic_handler -- which builds its frame on the interrupted stack. With a
 * U-mode guest owning sp, that faults inside the trap prologue and nests until
 * the watchdog fires. Exactly the death hyp_vectors.S exists to avoid.
 *
 * The CLIC control block is per-core behind a current/other aliasing scheme
 * (soc/interrupt_reg.h), so this MUST run on the core it is meant to affect. */
static void hyp_clic_sanitize(uint32_t core_id)
{
    for (int src = 0; src < ETS_MAX_INTR_SOURCE; src++) {
        esp_rom_route_intr_matrix(core_id, src, ETS_INVALID_INUM);
    }
    for (int i = 0; i < 32; i++) {
        ESP_INTR_DISABLE(i);
        esprv_int_set_vectored(i, false);
    }
}

static void hyp_setup_interrupts(uint32_t core_id)
{
    hyp_clic_sanitize(core_id);

    /* Threshold, explicitly. On this silicon (rev < v3, INTTHRESH_STANDARD == 0)
     * it is the memory-mapped CLIC_INT_THRESH_REG rather than the mintthresh
     * CSR, which is what this helper picks for us. Only the FreeRTOS port ever
     * writes it, and FreeRTOS never starts, so it is sitting at its reset value
     * -- and the comparison is inclusive, so a non-zero reset value would mask
     * our lines and nothing would ever fire. */
    rv_utils_restore_intlevel_regval(RVHAL_INTR_ENABLE_THRESH_CLIC);

    /* SYSTIMER counter 0 is shared with esp_timer, which already started it.
     * Enable it anyway -- idempotent, and a stopped counter presents only as
     * "the guest's clock never advances". */
    systimer_ll_enable_clock(&SYSTIMER, true);
    systimer_ll_enable_counter(&SYSTIMER, HYP_SYSTIMER_COUNTER, true);
    systimer_ll_counter_can_stall_by_cpu(&SYSTIMER, HYP_SYSTIMER_COUNTER, 0, false);
    systimer_ll_counter_can_stall_by_cpu(&SYSTIMER, HYP_SYSTIMER_COUNTER, 1, false);
    systimer_ll_enable_alarm(&SYSTIMER, HYP_SYSTIMER_ALARM, false);
    systimer_ll_clear_alarm_int(&SYSTIMER, HYP_SYSTIMER_ALARM);

    /* Take ownership of UART0 from the ROM. The ROM's uart_rx_intr_handler is
     * already unrouted by IDF's core_intr_matrix_clear, but its enable bits,
     * its latched status, and any bytes typed during boot are all still live. */
    uart_dev_t *hw = UART_LL_GET_HW(0);
    uart_ll_disable_intr_mask(hw, UART_LL_INTR_MASK);
    uart_ll_clr_intsts_mask(hw, UART_LL_INTR_MASK);
    while (uart_ll_get_rxfifo_len(hw) > 0u) {
        uint8_t discard;
        uart_ll_read_rxfifo(hw, &discard, 1);
    }
    uart_ll_set_rxfifo_full_thr(hw, 1);     /* one keystroke -> one interrupt */
    uart_ll_set_rx_tout(hw, 10);            /* ~11 us at 921600 */

    /* Priority must be set explicitly. CLIC_INT_CTL resets to 0x1f and with
     * NLBITS == 3 that is level 0; the threshold comparison is inclusive, so
     * threshold 0 masks level 0 and a line left at its reset priority never
     * fires. esp_intr_alloc always sets priority, which is why IDF never trips
     * on this and we would. */
    esprv_int_set_type(HYP_INUM_TIMER, INTR_TYPE_LEVEL);
    esprv_int_set_priority(HYP_INUM_TIMER, HYP_INT_PRIO);
    esprv_int_set_vectored(HYP_INUM_TIMER, false);   /* shv=0 -> lands in mtvec */

    esprv_int_set_type(HYP_INUM_UART, INTR_TYPE_LEVEL);
    esprv_int_set_priority(HYP_INUM_UART, HYP_INT_PRIO);
    esprv_int_set_vectored(HYP_INUM_UART, false);

    esp_rom_route_intr_matrix(core_id, ETS_SYSTIMER_TARGET1_INTR_SOURCE, HYP_INUM_TIMER);
    esp_rom_route_intr_matrix(core_id, ETS_UART0_INTR_SOURCE, HYP_INUM_UART);

    ESP_INTR_ENABLE(HYP_INUM_TIMER);
    ESP_INTR_ENABLE(HYP_INUM_UART);

    uart_ll_ena_intr_mask(hw, UART_INTR_RXFIFO_FULL | UART_INTR_RXFIFO_TOUT |
                              UART_INTR_RXFIFO_OVF);

    /* mstatus.MIE is deliberately left clear. The guest runs in U-mode, and
     * M-mode interrupts are always globally enabled when the current privilege
     * is below M -- so this costs nothing and keeps our own setup code
     * uninterruptible. */
}

static inline void linux_start(void)
{
    asm volatile ("csrc mstatus, %0" :: "r"(3u << 11));
    asm volatile ("csrw mepc, %0" :: "r"(linux_base_address));
    asm volatile ("li a0, 0");
    asm volatile ("mv a1, %0" :: "r"(dtb_address) : "a1");
    asm volatile ("mret");
}

static esp_err_t pmp_set_rwx_region(uint32_t base, uint32_t size)
{
    if (size < 8u || (size & (size - 1u)) != 0u || (base & (size - 1u)) != 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    if (PMP_ENTRY_CFG_READ(pmp_entery) & PMP_L) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint32_t addr = base | ((size - 1u) >> 1);
    const uint8_t cfg = PMP_NAPOT | PMP_R | PMP_W | PMP_X;

    PMP_RESET_AND_ENTRY_SET(pmp_entery, addr, cfg);

    if (PMP_ENTRY_CFG_READ(pmp_entery) != cfg) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* Copy a partition into PSRAM. `limit` caps the copy: it exists from when the
 * rootfs was copied into a 2 MB window smaller than its partition, and without
 * the cap the memcpy ran straight through the DTB sitting above it. Nothing
 * passes a non-zero limit now -- the filesystems are mapped, not copied -- but
 * the kernel and DTB still come through here. Pass 0 for no cap. */
static esp_err_t partition_cp_to_psram(const char *label, uint32_t destination,
                                       uint32_t limit)
{
    esp_err_t err_code;
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                        ESP_PARTITION_SUBTYPE_DATA_UNDEFINED,
                                                        label);
    if (p == null) {
        return ESP_ERR_NOT_FOUND;
    }

    uint32_t span = p->size;
    if (limit != 0u && span > limit) {
        span = limit;
    }

    const void* src;
    esp_partition_mmap_handle_t h;
    err_code = esp_partition_mmap(p, 0, span, ESP_PARTITION_MMAP_DATA, &src, &h);
    if (err_code != ESP_OK) {
        return err_code;
    }
    memcpy((void *)destination, src, span);
    esp_partition_munmap(h);
    return ESP_OK;
}

/* Map one filesystem partition into the flash cache window and leave it mapped.
 *
 * No munmap: the guest reads this for its whole uptime. Dropping the handle
 * would unmap the pages and the guest's next read would fault somewhere far
 * away from here.
 *
 * Three of these are mapped at boot and they are not free of each other: they
 * share the flash MMU, so 13 MB of windows consumes 13 MB of it. That is well
 * inside SOC_DROM_LOW..SOC_DROM_HIGH on this part, and if it ever stops being
 * so, the bounds check below is what says so rather than the guest. */
static esp_err_t window_map_from_flash(struct guest_window *w)
{
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                       ESP_PARTITION_SUBTYPE_DATA_UNDEFINED,
                                                       w->label);
    if (p == null) {
        return ESP_ERR_NOT_FOUND;
    }

    esp_partition_mmap_handle_t h;
    esp_err_t err = esp_partition_mmap(p, 0, p->size, ESP_PARTITION_MMAP_DATA,
                                       &w->mapping, &h);
    if (err != ESP_OK) {
        return err;
    }
    w->span = p->size;

    /* Sanity: a mapping outside the flash cache window would not be covered by
     * PMP entry 6, and the guest's first read would take an access fault that
     * looks like a filesystem bug. Say so here instead. */
    uint32_t va = (uint32_t)w->mapping;
    if (va < SOC_DROM_LOW || va + w->span > SOC_DROM_HIGH) {
        esp_rom_printf("E: %s mapped at 0x%08x, outside the PMP-granted "
                       "flash window 0x%08x-0x%08x\n",
                       w->label, (unsigned)va,
                       (unsigned)SOC_DROM_LOW, (unsigned)SOC_DROM_HIGH);
        return ESP_ERR_INVALID_STATE;
    }

    /* A partition whose size is not a power of two is a silent loss of up to
     * half of it: physmap-core.c:521 computes win_order from the size and then
     * uses BIT(win_order), so a 12 MB window becomes an 8 MB device and an
     * image that fits the partition does not fit the device. The guest reports
     * that as "Unable to mount root fs", which points nowhere near
     * partitions.csv, so it is worth a line here. */
    if ((w->span & (w->span - 1u)) != 0u) {
        esp_rom_printf("W: %s is %u KB, not a power of two -- the guest sees "
                       "only %u KB of it (physmap rounds down)\n",
                       w->label, (unsigned)(w->span / 1024u),
                       (unsigned)((1u << (31 - __builtin_clz(w->span))) / 1024u));
    }
    return ESP_OK;
}

/* Point one mtd-rom node in the DTB at wherever its mapping landed.
 *
 * esp_partition_mmap picks a free MMU slot, so the address is not knowable at
 * build time and the DTS carries a placeholder. Patched by pattern rather than
 * by walking the FDT: reg is one big-endian <address size> pair, and each
 * node's placeholder pair appears exactly once in the blob, so finding those
 * eight bytes is both simpler and self-checking -- a DTS edit that changes a
 * placeholder makes this fail loudly instead of writing to the wrong offset.
 *
 * With three nodes the "exactly once" property is doing more work than it was
 * with one, because a duplicated placeholder would now silently patch the
 * FIRST node twice and leave the second pointing at a stale address. The three
 * pairs in guest_windows[] are deliberately different from each other; the
 * count is checked after compiling the DTS, not here.
 *
 * The scan restarts from the top of the blob for each window. Three passes
 * over 2 kB at boot is not worth optimising. */
static bool patch_dtb_window(const uint8_t needle[8], uint32_t address,
                             uint32_t span)
{
    uint8_t *dtb = (uint8_t *)dtb_address;

    /* The blob's own totalsize, big-endian at offset 4. Bounded by the 1 MB
     * window in case the partition was never written. */
    uint32_t total = ((uint32_t)dtb[4] << 24) | ((uint32_t)dtb[5] << 16) |
                     ((uint32_t)dtb[6] << 8)  |  (uint32_t)dtb[7];
    if (total < 8u || total > 0x00100000u) {
        return false;
    }

    for (uint32_t i = 0u; i + 8u <= total; i++) {
        if (memcmp(dtb + i, needle, 8) != 0) {
            continue;
        }
        dtb[i + 0] = (uint8_t)(address >> 24);
        dtb[i + 1] = (uint8_t)(address >> 16);
        dtb[i + 2] = (uint8_t)(address >> 8);
        dtb[i + 3] = (uint8_t)(address);
        dtb[i + 4] = (uint8_t)(span >> 24);
        dtb[i + 5] = (uint8_t)(span >> 16);
        dtb[i + 6] = (uint8_t)(span >> 8);
        dtb[i + 7] = (uint8_t)(span);
        return true;
    }
    return false;
}

/* Rewrite the kernel's built-in command line in place.
 *
 * CONFIG_CMDLINE_FORCE=y, so the DTB's bootargs is ignored -- but the built-in
 * string is plain data inside the image we just copied into RWX PSRAM. The
 * field is 73 chars plus 3 NUL, so a replacement of up to 75 fits.
 *
 * This buys three things: root=/dev/vda goes away (the embedded initramfs makes
 * it inert anyway, and this stops the kernel from trying), and keep_bootcon
 * holds earlycon open through the ttyS0 handover -- without it, any defect in
 * the 8250 emulation is indistinguishable from a hang.
 *
 * Searched rather than hardcoded at 0x48247128: that offset is specific to this
 * kernel build. */
static bool patch_guest_cmdline(uint32_t span)
{
    static const char needle[] = "root=/dev/vda rw earlycon=";
    static const char replacement[] =
        "earlycon=uart8250,mmio,0x10000000 console=ttyS0 keep_bootcon";

    const size_t needle_len = sizeof(needle) - 1u;
    char *base = (char *)linux_base_address;

    for (uint32_t i = 0u; i + needle_len < span; i++) {
        if (memcmp(base + i, needle, needle_len) != 0) {
            continue;
        }
        size_t original = strlen(base + i);
        if (sizeof(replacement) - 1u > original) {
            return false;                     /* would overrun the field */
        }
        memset(base + i, 0, original);
        memcpy(base + i, replacement, sizeof(replacement) - 1u);
        return true;
    }
    return false;
}

/* Everything that touches flash, done before FreeRTOS starts.
 *
 * The split is forced by spi_flash's cross-core handshake, and finding that out
 * cost a boot: a flash operation on core 0 normally IPCs core 1 into
 * spi_flash_op_block_func() so that neither core is fetching from a disabled
 * cache. Core 1 does not run FreeRTOS here, so that IPC is never serviced and
 * core 0 spins forever inside
 * spi_flash_disable_interrupts_caches_and_other_cpu() -- which presents as the
 * task watchdog firing on IDLE0 about ten milliseconds into app_main(), with
 * the register dump pointing at cache_utils.c and nothing pointing at the
 * guest.
 *
 * cache_utils.c takes a different path when xTaskGetSchedulerState() is
 * taskSCHEDULER_NOT_STARTED: no IPC, on the assumption that the other core is
 * still spinning in startup code. That is exactly the state here, so all of the
 * loading happens in that window and nothing after it touches flash again.
 *
 * Returns false if the guest cannot be assembled; the caller still has to boot
 * core 0, because a board with an empty kernel partition should come up far
 * enough to say so. */
static bool hyp_load_guest(void)
{
    esp_err_t result = partition_cp_to_psram("kernel", linux_base_address, 0u);
    if (result != ESP_OK) {
        esp_rom_printf("E: kernel: %s\n", esp_err_to_name(result));
        return false;
    }
    esp_rom_printf("I: Copied Linux Image to PSRAM\n");

    result = partition_cp_to_psram("dtb", dtb_address, 0u);
    if (result != ESP_OK) {
        esp_rom_printf("E: dtb: %s\n", esp_err_to_name(result));
        return false;
    }
    esp_rom_printf("I: Copied DTB to PSRAM\n");

    /* The guest filesystems, left in flash.
     *
     * A MISSING partition is not fatal: a kernel with a built-in initramfs
     * needs no rootfs at all, the tools and extra windows are optional by
     * design, and the same firmware has to boot a board flashed with an older
     * partition table. Each absent one costs a warning and its mount.
     *
     * What IS fatal is mapping one and then failing to tell the guest where it
     * went. A guest that reads the stale placeholder address fails somewhere in
     * the filesystem layer, a long way from the cause, so that case stops the
     * boot here where the message is still readable. */
    for (unsigned i = 0u; i < GUEST_WINDOW_COUNT; i++) {
        struct guest_window *w = &guest_windows[i];

        result = window_map_from_flash(w);
        if (result != ESP_OK) {
            esp_rom_printf("W: %s: %s (no %s mount)\n",
                           w->label, esp_err_to_name(result), w->mountpoint);
            continue;
        }
        if (!patch_dtb_window(w->placeholder, (uint32_t)w->mapping, w->span)) {
            esp_rom_printf("E: %s mapped at 0x%08x but its DTB placeholder was "
                           "not found -- guest would read the wrong address\n",
                           w->label, (unsigned)(uint32_t)w->mapping);
            return false;
        }
        esp_rom_printf("I: %s XIP from flash at 0x%08x (%u KB) -> %s\n",
                       w->label, (unsigned)(uint32_t)w->mapping,
                       (unsigned)(w->span / 1024u), w->mountpoint);
    }

    /* Only the older initramfs kernel needs this -- the romfs kernel sets its
     * own CONFIG_CMDLINE, so the needle is absent and the patch is a no-op. */
    if (patch_guest_cmdline(0x00400000u)) {
        esp_rom_printf("I: Patched guest command line\n");
    } else {
        esp_rom_printf("I: Guest command line left as built\n");
    }
    return true;
}

static bool guest_loaded = false;

/* Load the guest, then hand over to IDF's own core-0 startup.
 *
 * The wrap exists only to get in before vTaskStartScheduler(); everything after
 * it is stock. __real_ is called rather than reimplemented, so main_task,
 * app_main and the task watchdog all behave exactly as they normally would. */
extern void __real_esp_startup_start_app(void);

void __wrap_esp_startup_start_app(void)
{
    guest_loaded = hyp_load_guest();
    __real_esp_startup_start_app();
}

/* Core 0, as a FreeRTOS task.
 *
 * Nothing in here touches flash. What is left is the USB keyboard -- which needs
 * the scheduler, because the host stack blocks on semaphores and waits on ISR
 * dispatch -- and then watching the guest. */
void app_main(void)
{
    if (!guest_loaded) {
        esp_rom_printf("E: guest was not loaded; core 1 stays parked\n");
        return;
    }

    /* The keyboard. Core 0, needs the scheduler, and not fatal: the serial
     * port is still an input path. */
    if (!hyp_kbd_init()) {
        esp_rom_printf("W: kbd: no USB host; serial input only\n");
    }

    guest_image_ready = true;
    esp_rom_printf("W: Core 0: guest released\n");

    /* ------------------------------------------------------------------
     * The service loop.
     *
     * 10 Hz is plenty now that there is no framebuffer to repaint -- all this
     * loop does is watch the guest. vTaskDelay yields, so the idle task still
     * runs and the task watchdog stays fed.
     *
     * The liveness check reports rather than resets. A hypervisor that does a
     * privilege-dropping mret on every single trap is maximally exposed to the
     * CLIC erratum on this silicon (mcause.mpil latching at 0xff, after which
     * no interrupt can ever be delivered because level > mil is 255 > 255), and
     * the failure mode is a guest that simply stops. Resetting would hide it;
     * printing the trap ring names it.
     * ------------------------------------------------------------------ */
    const TickType_t period = pdMS_TO_TICKS(100);
    uint32_t last_traps = 0u;
    uint32_t quiet_ticks = 0u;
    uint32_t ticks = 0u;
    bool reported = false;

    for (;;) {
        if (hyp_stats_wanted) {
            hyp_stats_wanted = false;
            hyp_stats_dump();
        }

        if (++ticks >= 10u) {                   /* ~1 s */
            ticks = 0u;
            if (hyp_trace_total == last_traps) {
                quiet_ticks++;
            } else {
                quiet_ticks = 0u;
                reported = false;
            }
            last_traps = hyp_trace_total;

            /* 5 s of no traps at all, and not parked in a wfi. The hyp_in_wfi
             * test is what stops this from crying wedge at an idle guest, now
             * that wfi actually blocks. */
            if (quiet_ticks >= 5u && !reported && linux_running && !hyp_in_wfi) {
                reported = true;
                esp_rom_printf("\nW: guest has taken no trap for %u s -- wedged?\n",
                               (unsigned)quiet_ticks);
                hyp_trace_dumps = 0u;
                hyp_trace_dump("liveness");
                hyp_stats_dump();
            }
        }

        vTaskDelay(period);
    }
}

/* Core 1. The guest's core: PMP, trap vector, host interrupts, then mret.
 *
 * PMP, mtvec and mscratch are all per-hart, so all three are set up here rather
 * than inherited from core 0. IDF's call_start_cpu1() has already run
 * bootloader_init_mem() on this core, so entries 0-4/6/11/15 arrive locked
 * exactly as they are on core 0. Free entries on rev 1.0 are 5 and 7-10/12-14;
 * 5 is the one used, and it has to be 5 rather than any of the others because
 * entry 6 covers the flash window and PMP resolves by lowest matching index.
 *
 * Entered from __wrap_xPortStartScheduler() rather than by overriding
 * esp_startup_start_app_other_cores(), and the difference cost a boot to find.
 *
 * The obvious hook is that weak symbol in startup.c. It cannot be used here:
 * freertos/app_startup.c ALSO defines it, strongly, so overriding it collides
 * at link time as soon as that object is in the link -- and it is, because core
 * 0 now boots into FreeRTOS normally. A --wrap on it does not help either,
 * because --wrap only redirects UNDEFINED references and startup.c both defines
 * the symbol weakly and calls it in the same object. The board boots, core 0
 * comes all the way up, the panel initialises -- and core 1 quietly runs
 * FreeRTOS instead of the guest, with nothing printed to say so.
 *
 * xPortStartScheduler() is wrappable: it lives in the FreeRTOS port and is
 * called from app_startup.c and tasks.c, never from its own object. Both cores
 * pass through the wrap, so it dispatches on the core id -- core 0 goes on into
 * the real scheduler, core 1 comes here and never returns. Everything IDF does
 * to bring core 1 up before that point (crosscore interrupt, the INT WDT tick
 * hook) is left in place and then undone by hyp_clic_sanitize(). */
static void hyp_guest_core_entry(void)
{
    /* Tell core 0 this core has finished starting up.
     *
     * main_task() blocks in wait_for_all_cores_ready() until every core has set
     * its flag, and the real xPortStartScheduler() is what normally sets it --
     * which this core no longer reaches. Without this the two cores deadlock
     * waiting for each other: core 0 inside wait_for_all_cores_ready(), core 1
     * on guest_image_ready just below, and the only symptom is a boot that
     * stops after "main_task: Started on CPU0".
     *
     * Safe to claim here only because __wrap_xPortStartScheduler() has already
     * moved this core onto hyp_boot_stack -- see the note there. */
    extern volatile unsigned port_uxCoreStartupDone[CONFIG_FREERTOS_NUMBER_OF_CORES];
    port_uxCoreStartupDone[rv_utils_get_core_id()] = 1u;

    while (!guest_image_ready) { }

    /* Keep our own bring-up uninterruptible. IDF's call_start_cpu1() left this
     * core with a live cache-error interrupt pointing into _mtvt_table, and
     * mtvec is about to become ours -- taking anything before hyp_clic_sanitize
     * has run would land in hypervisor() with linux_running still false. */
    RV_CLEAR_CSR(mstatus, MSTATUS_MIE);

    uint32_t core_id = rv_utils_get_core_id();

    esp_err_t result = pmp_set_rwx_region(linux_base_address, linux_size);
    if (result != ESP_OK) {
        esp_rom_printf("E: pmp: %s\n", esp_err_to_name(result));
        while (1) { }
    }
    esp_rom_printf("I: Core %u: PMP RWX on guest window\n", (unsigned)core_id);

    /* A stack the guest cannot reach. hyp_trap_entry swaps it in from mscratch
     * before touching memory, because the guest owns sp and Linux's reset_regs
     * sets it to 0. */
    RV_WRITE_CSR(mscratch, (uint32_t)&hyp_stack[sizeof(hyp_stack) / sizeof(hyp_stack[0])]);
    esp_cpu_intr_set_ivt_addr(&hyp_vector_table);

    hyp_setup_interrupts(core_id);
    esp_rom_printf("I: Core %u: interrupts armed (timer=%u uart=%u)\n",
                   (unsigned)core_id, HYP_INUM_TIMER, HYP_INUM_UART);

    linux_running = true;
    esp_rom_printf("W: Starting Linux on core %u\n", (unsigned)core_id);
    linux_start();

    while (1) { }
}

/* Divert core 1 into the guest.
 *
 * Called on both cores: core 0 from vTaskStartScheduler(), core 1 from
 * esp_startup_start_app_other_cores(). Only core 1 is taken. */
BaseType_t __real_xPortStartScheduler(void);

BaseType_t __wrap_xPortStartScheduler(void)
{
    if (rv_utils_get_core_id() != 0u) {
        /* Move onto our own stack before doing anything else, then jump. Two
         * instructions in one asm block because both operands have to be in
         * registers before sp changes, and because there is no returning from
         * this: the guest owns sp after the mret at the end. */
        asm volatile ("mv sp, %0\n	"
                      "jalr %1"
                      :: "r"(&hyp_boot_stack[sizeof(hyp_boot_stack) / sizeof(hyp_boot_stack[0])]),
                         "r"(&hyp_guest_core_entry)
                      : "memory");
        __builtin_unreachable();
    }
    return __real_xPortStartScheduler();
}
