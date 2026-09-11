/* Shared contract between the monitor's modules.
 *
 * main.c used to be all of it -- 2,300 lines covering instruction decode, three
 * emulated devices, CSR emulation, interrupt injection, trap dispatch, guest
 * loading and core bring-up. This header is what that split needed: the types
 * and state that genuinely cross module boundaries, and nothing else.
 *
 * The rule for what belongs here: a symbol earns a place only if more than one
 * .c file uses it. Everything module-private stays `static` in its own file --
 * the PLIC's registers, the 16550's ring buffer, the shadow PMP arrays, the
 * trace ring. If you find yourself adding a declaration here to make one file
 * compile, check first whether the code wanted to be in the other file.
 *
 *   hyp_decode.c   instruction fetch, RVC expansion, register access
 *   hyp_csr.c      shadow CSR bank, csr/mret/wfi emulation
 *   hyp_clint.c    emulated CLINT (guest timer)
 *   hyp_plic.c     emulated PLIC (guest interrupt controller)
 *   hyp_uart.c     emulated 16550 (guest console)
 *   hyp_intr.c     shadow mip, interrupt injection, host device servicing
 *   hyp_mmio.c     device table and the load/store decoder
 *   hyp_trace.c    trap trace ring, statistics, panic
 *   hyp_core.c     trap dispatch -- the entry point from hyp_vectors.S
 *   hyp_boot.c     guest loading, PMP, core-1 bring-up
 *   main.c         app_main: core 0's service loop
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "riscv/csr.h"
#include "riscv/rvruntime-frames.h"
#include "esp_rom_sys.h"

/* Kept from the original file rather than replaced with NULL: it is used
 * throughout and the diff is noisy enough already. */
#define null ((void*)0)


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
#define HYP_SYSTIMER_ALARM    1

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

#define HYP_VIRTIO_BLK_BASE     0x10001000u
#define HYP_VIRTIO_BLK_SIZE     0x00001000u

#define PLIC_UART_SOURCE    10      /* matches interrupts = <10> in the DTS */
#define PLIC_VIRTIO_BLK_SOURCE 11   /* matches interrupts = <11> in the DTS */
#define PLIC_MAX_SOURCE     31      /* matches riscv,ndev = <31> */

/* Ctrl-^ prints the trap histogram. Nothing binds it, and the numbers are
 * useless if you have to wait for a wedge to see them. */
#define HYP_KEY_STATS_DUMP 0x1eu


/* ===========================================================================
 * Guest memory layout
 * ===========================================================================
 */

extern uint32_t linux_base_address;
extern uint32_t linux_size;
extern uint32_t dtb_address;

/* Set once the guest has been mret-ed into. Read by core 0's liveness check,
 * which must not cry wedge before the guest has started. */
extern bool linux_running;


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
 * Trap accounting
 *
 * The trace ring answers "what just happened"; this answers "where does the
 * time go", which is the question every optimisation of this monitor actually
 * turns on.
 * ===========================================================================
 */

#define HYP_STATS_CAUSES 48u

/* Bounds hyp_stats.mmio[]. Defined here rather than beside the device table
 * because the statistics struct below indexes by it. Raise it when a device is
 * added; the dump iterates the table, so nothing else needs touching. */
#define HYP_MMIO_MAX_DEVICES 8u

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
    uint32_t mmio[HYP_MMIO_MAX_DEVICES];/* by index into mmio_devices[]         */
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
extern struct hyp_stats_s hyp_stats;


/* ===========================================================================
 * Shadow CSR bank
 *
 * The guest believes it is in M-mode and owns the machine CSRs. It does not:
 * it runs in U-mode, and its csr instructions trap as illegal instructions.
 * These slots are the CSRs it thinks it has.
 *
 * Shared because four modules need it: hyp_csr.c owns it, hyp_intr.c composes
 * mip and consults mie/mstatus to decide whether an interrupt can be delivered,
 * hyp_clint.c re-evaluates the timer against it, and hyp_core.c reads mtvec
 * when injecting.
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

typedef struct {
    uint32_t mstatus;
    uint32_t mie;
    uint32_t mtvec;
    uint32_t mscratch;
    uint32_t mepc;
    uint32_t mcause;
    uint32_t mtval;
    uint32_t mip;       /* bits 7 and 11 are NOT stored here -- see shadow_mip_read() */
} HypShadowCsr;

extern HypShadowCsr shadow;

/* The guest's software privilege level. Real hardware would distinguish an ecall
 * from the kernel (cause 11) from one out of userspace (cause 8), but here the
 * whole guest runs in U-mode so the hardware always reports 8. This variable is
 * what recovers the distinction, and it moves on exactly two events: a trap
 * injected into the guest sets it to M, and mret restores it from mstatus.MPP. */
extern uint32_t guest_priv;


/* ===========================================================================
 * hyp_decode.c -- instruction fetch, decode and guest register access
 * ===========================================================================
 */

uint32_t reg_read(RvExcFrame *frame, uint32_t index);
void     reg_write(RvExcFrame *frame, uint32_t index, uint32_t value);

/* Read the instruction at `mepc` out of guest memory. False if the address is
 * outside the guest window -- a fetch that faulted would re-enter the trap
 * handler and never come back. */
bool guest_fetch(uint32_t mepc, uint32_t *encoding, uint32_t *length);

/* Fill in trap->encoding and trap->length. Idempotent: handlers call it
 * without caring whether the dispatcher already did. */
bool trap_decode(GuestTrap *trap);


/* ===========================================================================
 * hyp_csr.c -- shadow CSRs, and the instructions that trap as illegal
 * ===========================================================================
 */

TrapResult hypervisor_csr(GuestTrap *trap);
TrapResult hypervisor_mret(GuestTrap *trap);
TrapResult hypervisor_wfi(void);

/* Make guest-written code visible to the instruction fetch. Called on every
 * return to the guest; see the comment at the definition for why a `fence.i`
 * from the guest is not enough on this core. */
void hyp_sync_icache(void);

/* True while the guest is parked in a real wfi. Core 0's liveness check reads
 * it so that an idle guest is not mistaken for a wedged one. */
extern volatile bool hyp_in_wfi;

/* Cycles spent asleep in a blocking wfi. hyp_core.c subtracts these from the
 * trap's measured cost, so an idle guest does not read as a slow one. */
extern uint32_t hyp_idle_cycles;


/* ===========================================================================
 * hyp_clint.c -- the guest's timer
 * ===========================================================================
 */

uint64_t hyp_mtime(void);
void     hyp_arm_timer(void);
bool     mtip_pending(void);
/* The guest's programmed timer deadline. hyp_csr.c consults it to decide
 * whether a wfi may block: nothing armed and nothing pending means there is
 * nothing to wake for. */
uint64_t hyp_mtimecmp(void);

uint32_t clint_read(uint32_t offset, uint32_t width);
void     clint_write(uint32_t offset, uint32_t value, uint32_t width);


/* ===========================================================================
 * hyp_plic.c -- the guest's interrupt controller
 * ===========================================================================
 */

bool     meip_pending(void);
void     plic_set_pending(uint32_t source, bool asserted);
uint32_t plic_read(uint32_t offset, uint32_t width);
void     plic_write(uint32_t offset, uint32_t value, uint32_t width);


/* ===========================================================================
 * hyp_uart.c -- the guest's console
 * ===========================================================================
 */

/* Push a received byte into the guest's receive ring. Core 1 is the only
 * writer, which is why host input is drained here rather than pushed straight
 * in by whoever received it. */
void     uart_rx_push(uint8_t c);

/* Recompute the 16550's interrupt state and drive the PLIC line. */
void     v16550_reeval(void);

uint32_t uart_read(uint32_t offset, uint32_t width);
void     uart_write(uint32_t offset, uint32_t value, uint32_t width);

/* The two counters the statistics dump reports. Accessors rather than an
 * exported struct: the rest of the 16550's state -- IER, LCR, the receive ring,
 * the DLAB latch -- has no business outside hyp_uart.c, and exporting the whole
 * thing to reach two fields is how a module stops being one. */
uint32_t hyp_uart_overruns(void);
uint32_t hyp_uart_tx_dropped(void);


/* ===========================================================================
 * hyp_intr.c -- shadow mip, injection, and host device servicing
 * ===========================================================================
 */

/* mip as the guest should see it: the stored soft bits, plus MTIP and MEIP
 * recomputed from the emulated timer and PLIC. */
uint32_t shadow_mip_read(void);

/* Enter the guest's trap handler: park mepc/mcause/mtval in the shadow bank,
 * drop mstatus.MIE, and set frame->mepc to the guest's mtvec. */
void hyp_inject(RvExcFrame *frame, uint32_t cause, uint32_t tval);

/* Inject the highest-priority pending interrupt, if the guest can take one. */
void hyp_deliver_pending(RvExcFrame *frame);

/* Service the host side: the systimer alarm, the physical UART's receive
 * FIFO, and any device whose completion has to be noticed from core 1. */
void hyp_poll_host_devices(void);

/* Set by core 1 when Ctrl-^ arrives, acted on by core 0. Printing from inside
 * the trap handler would put a few hundred blocking UART writes in the guest's
 * path. */
extern volatile bool hyp_stats_wanted;


/* ===========================================================================
 * hyp_mmio.c -- the device table and the load/store decoder
 *
 * A device is one row in mmio_devices[] and the decoder never changes. That is
 * the whole reason this is a table: adding the block device below meant adding
 * a row, not editing the middle of a load/store decoder.
 * ===========================================================================
 */

typedef struct {
    uint32_t    base;
    uint32_t    size;
    uint32_t  (*read)(uint32_t offset, uint32_t width);
    void      (*write)(uint32_t offset, uint32_t value, uint32_t width);
    const char *name;
} MmioDevice;

extern const MmioDevice mmio_devices[];
extern const uint32_t   mmio_device_count;

/* Which device owns an address, or null. hyp_core.c uses it to tell a real
 * bad access from one that merely landed in a device window. */
const MmioDevice *mmio_find(uint32_t address);

TrapResult hypervisor_mmio(GuestTrap *trap);


/* ===========================================================================
 * hyp_trace.c -- trap history, statistics and panic
 * ===========================================================================
 */

void hyp_trace_record(uint32_t cause, uint32_t epc, uint32_t tval);
void hyp_trace_dump(const char *why);
void hyp_stats_dump(void);

__attribute__((noreturn)) void hyp_panic(RvExcFrame *frame);

/* Total traps seen, and the dump budget. Core 0's liveness check compares the
 * total against its previous sample, and resets the budget before asking for a
 * dump it actually wants to see. */
extern uint32_t hyp_trace_total;
extern uint32_t hyp_trace_dumps;


/* ===========================================================================
 * hyp_core.c -- trap dispatch
 * ===========================================================================
 */

/* The entry point from hyp_vectors.S. Returns true if the trap was handled and
 * the guest should be resumed. */
bool hypervisor(RvExcFrame *frame);


/* ===========================================================================
 * hyp_sd.c -- the microSD card, as raw sectors
 *
 * Backing store for the guest's block device. Every function here BLOCKS and
 * must only be called from core 0: ESP-IDF's SD driver waits on semaphores and
 * ISR dispatch, neither of which exists in core 1's trap handler.
 * ===========================================================================
 */

bool     hyp_sd_init(void);
bool     hyp_sd_ready(void);
uint32_t hyp_sd_sector_count(void);
bool     hyp_sd_read(uint32_t sector, uint32_t count, void *dst);
bool     hyp_sd_write(uint32_t sector, uint32_t count, const void *src);


/* ===========================================================================
 * hyp_virtio_blk.c -- a virtio-mmio block device backed by the SD card
 *
 * The guest's kernel already has CONFIG_VIRTIO_MMIO and CONFIG_VIRTIO_BLK,
 * so this gets a working /dev/vda with no guest-side changes at all.
 *
 * Split across the cores: the register accessors run on core 1 in the trap
 * handler and may not block, so a queue notify only records that the guest
 * kicked. Core 0's service task does the SD I/O and publishes the used ring,
 * and hyp_virtio_blk_poll() -- called from core 1 -- turns that into a guest
 * interrupt.
 * ===========================================================================
 */

bool     hyp_virtio_blk_init(void);        /* core 0, after the card is up */
uint32_t hyp_virtio_blk_read(uint32_t offset, uint32_t width);
void     hyp_virtio_blk_write(uint32_t offset, uint32_t value, uint32_t width);
void     hyp_virtio_blk_poll(void);        /* core 1, from the device poll */
void     hyp_virtio_blk_stats(void);


/* ===========================================================================
 * hyp_boot.c -- guest loading and core bring-up
 * ===========================================================================
 */

/* Core 0 copies the kernel; core 1 must not mret into a half-copied image. */
extern volatile bool guest_image_ready;
