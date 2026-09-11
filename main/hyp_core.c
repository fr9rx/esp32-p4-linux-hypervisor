/* Trap dispatch: the entry point from hyp_vectors.S
 *
 * Split out of main.c, which had grown to 2,300 lines. hyp.h carries the
 * contract between the modules and the map of which file does what.
 */
#include "hyp.h"

#include "riscv/rv_utils.h"

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


