/* Shadow mip, interrupt injection, and host device servicing
 *
 * Split out of main.c, which had grown to 2,300 lines. hyp.h carries the
 * contract between the modules and the map of which file does what.
 */
#include "hyp.h"

#include "hal/systimer_ll.h"
#include "hal/uart_ll.h"

/* Set by the trap handler on core 1 when Ctrl-^ arrives, acted on by core 0.
 * Printing from inside the trap handler would put a few hundred blocking UART
 * writes in the guest's path. */
volatile bool hyp_stats_wanted = false;

/* ===========================================================================
 * Shadow mip, and the single interrupt delivery point
 * ===========================================================================
 */

/* mip bits 7 and 11 are wires, not storage. Synthesise them on read; drop
 * writes to them. */
uint32_t shadow_mip_read(void)
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
void hyp_inject(RvExcFrame *frame, uint32_t cause, uint32_t tval)
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
void hyp_deliver_pending(RvExcFrame *frame)
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
void hyp_poll_host_devices(void)
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
            } else {
                uart_rx_push(c);
            }
        }
        uart_ll_clr_intsts_mask(hw, UART_INTR_RXFIFO_FULL | UART_INTR_RXFIFO_TOUT |
                                    UART_INTR_RXFIFO_OVF);
    }

    /* Turn any completion core 0 published into a guest interrupt. Cheap:
     * one volatile read when there is nothing to do. */
    hyp_virtio_blk_poll();

    v16550_reeval();
}


