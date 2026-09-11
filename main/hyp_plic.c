/* Emulated PLIC: the guest's interrupt controller
 *
 * Split out of main.c, which had grown to 2,300 lines. hyp.h carries the
 * contract between the modules and the map of which file does what.
 */
#include "hyp.h"

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

bool meip_pending(void)
{
    return plic_peek() != 0u;
}

void plic_set_pending(uint32_t source, bool asserted)
{
    if (asserted) {
        plic.pending |= (1u << source);
    } else {
        plic.pending &= ~(1u << source);
    }
}

uint32_t plic_read(uint32_t offset, uint32_t width)
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

void v16550_reeval(void);

void plic_write(uint32_t offset, uint32_t value, uint32_t width)
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


