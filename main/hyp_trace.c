/* Trap history, statistics and panic
 *
 * Split out of main.c, which had grown to 2,300 lines. hyp.h carries the
 * contract between the modules and the map of which file does what.
 */
#include "hyp.h"

#include "riscv/rv_utils.h"

/* The counters. Defined here because this is the file that reports them. */
struct hyp_stats_s hyp_stats;


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
uint32_t hyp_trace_total;
uint32_t hyp_trace_dumps;

void hyp_trace_record(uint32_t cause, uint32_t epc, uint32_t tval)
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
void hyp_trace_dump(const char *why)
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

    /* Walked rather than spelled out. The old line named clint, uart and plic
     * as literals, which was right for exactly three devices and silently
     * wrong the moment a fourth was added. */
    esp_rom_printf("  mmio");
    for (uint32_t i = 0u; i < mmio_device_count; i++) {
        esp_rom_printf(" %s=%u", mmio_devices[i].name,
                       (unsigned)hyp_stats.mmio[i]);
    }
    esp_rom_printf(" | csr rd=%u wr=%u | mret=%u inject=%u\n",
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
    esp_rom_printf("  uart rx overruns: %u, tx dropped: %u\n",
                   (unsigned)hyp_uart_overruns(), (unsigned)hyp_uart_tx_dropped());
    hyp_virtio_blk_stats();
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

