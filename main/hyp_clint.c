/* Emulated CLINT: the guest's timer
 *
 * Split out of main.c, which had grown to 2,300 lines. hyp.h carries the
 * contract between the modules and the map of which file does what.
 */
#include "hyp.h"

#include "hal/systimer_ll.h"

/* ===========================================================================
 * Emulated CLINT -- timer
 * ===========================================================================
 */

static uint64_t shadow_mtimecmp = UINT64_MAX;   /* ~0 == timer disabled */

/* A 52-bit snapshot, matching what systimer_hal_get_counter_value() does. */
uint64_t hyp_mtime(void)
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
void hyp_arm_timer(void)
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
bool mtip_pending(void)
{
    return hyp_mtime() >= shadow_mtimecmp;
}

uint32_t clint_read(uint32_t offset, uint32_t width)
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

void clint_write(uint32_t offset, uint32_t value, uint32_t width)
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



/* The guest's programmed deadline.
 *
 * hyp_csr.c needs it to decide whether a wfi may block: with no timer
 * armed and nothing pending there is nothing to wake for. An accessor
 * rather than an exported variable, because only this module may write
 * it -- clint_write() keeps the systimer alarm in step with it. */
uint64_t hyp_mtimecmp(void) { return shadow_mtimecmp; }
