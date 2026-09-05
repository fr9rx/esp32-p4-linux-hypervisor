/*
 * Hypervisor trap vector.
 *
 * Replaces IDF's _vector_table. IDF's _panic_handler builds its frame on the
 * interrupted stack (addi sp, sp, -RV_STK_FRMSZ), which is fatal for a U-mode
 * guest: the guest owns sp, and Linux's reset_regs sets it to 0, so the frame
 * save faults inside the trap prologue and nests until the watchdog fires.
 *
 * This stub swaps in a private stack from mscratch before touching memory. The
 * frame layout is identical to RvExcFrame, so hypervisor() is unchanged.
 */

#include "riscv/rvruntime-frames.h"

    .section .iram1, "ax"

    /* CLIC takes mtvec[31:6] << 6, so this must be 64-byte aligned. */
    .balign 0x40
    .global hyp_vector_table
    .type hyp_vector_table, @function
hyp_vector_table:
    j     hyp_trap_entry
    .size hyp_vector_table, .-hyp_vector_table

    .balign 4
    .type hyp_trap_entry, @function
hyp_trap_entry:
    /* mscratch holds the hypervisor stack top; the guest's sp is parked there. */
    csrrw sp, mscratch, sp
    addi  sp, sp, -RV_STK_FRMSZ

    sw    ra,  RV_STK_RA(sp)
    sw    gp,  RV_STK_GP(sp)
    sw    tp,  RV_STK_TP(sp)
    sw    t0,  RV_STK_T0(sp)
    sw    t1,  RV_STK_T1(sp)
    sw    t2,  RV_STK_T2(sp)
    sw    s0,  RV_STK_S0(sp)
    sw    s1,  RV_STK_S1(sp)
    sw    a0,  RV_STK_A0(sp)
    sw    a1,  RV_STK_A1(sp)
    sw    a2,  RV_STK_A2(sp)
    sw    a3,  RV_STK_A3(sp)
    sw    a4,  RV_STK_A4(sp)
    sw    a5,  RV_STK_A5(sp)
    sw    a6,  RV_STK_A6(sp)
    sw    a7,  RV_STK_A7(sp)
    sw    s2,  RV_STK_S2(sp)
    sw    s3,  RV_STK_S3(sp)
    sw    s4,  RV_STK_S4(sp)
    sw    s5,  RV_STK_S5(sp)
    sw    s6,  RV_STK_S6(sp)
    sw    s7,  RV_STK_S7(sp)
    sw    s8,  RV_STK_S8(sp)
    sw    s9,  RV_STK_S9(sp)
    sw    s10, RV_STK_S10(sp)
    sw    s11, RV_STK_S11(sp)
    sw    t3,  RV_STK_T3(sp)
    sw    t4,  RV_STK_T4(sp)
    sw    t5,  RV_STK_T5(sp)
    sw    t6,  RV_STK_T6(sp)

    /* The guest's sp went into mscratch during the swap above. */
    csrr  t0, mscratch
    sw    t0, RV_STK_SP(sp)

    csrr  t0, mepc
    sw    t0, RV_STK_MEPC(sp)
    csrr  t0, mstatus
    sw    t0, RV_STK_MSTATUS(sp)
    csrr  t0, mcause
    sw    t0, RV_STK_MCAUSE(sp)
    csrr  t0, mtval
    sw    t0, RV_STK_MTVAL(sp)
    csrr  t0, mhartid
    sw    t0, RV_STK_MHARTID(sp)

    /* The guest owns gp. Linux points it at its own __global_pointer$, and the
     * compiler reaches small globals through it, so C code entered with the
     * guest's gp silently reads Linux's memory instead of the monitor's.
     * norelax is required: without it the linker rewrites this as an addi
     * relative to the current (guest) gp, which is the bug it is fixing. */
    .option push
    .option norelax
    la    gp, __global_pointer$
    .option pop

    mv    a0, sp
    call  hypervisor
    beqz  a0, hyp_unhandled

    /* Emulated. mepc may have been advanced and a destination register written. */
    lw    t0, RV_STK_MEPC(sp)
    csrw  mepc, t0

    lw    ra,  RV_STK_RA(sp)
    lw    gp,  RV_STK_GP(sp)
    lw    tp,  RV_STK_TP(sp)
    lw    t1,  RV_STK_T1(sp)
    lw    t2,  RV_STK_T2(sp)
    lw    s0,  RV_STK_S0(sp)
    lw    s1,  RV_STK_S1(sp)
    lw    a0,  RV_STK_A0(sp)
    lw    a1,  RV_STK_A1(sp)
    lw    a2,  RV_STK_A2(sp)
    lw    a3,  RV_STK_A3(sp)
    lw    a4,  RV_STK_A4(sp)
    lw    a5,  RV_STK_A5(sp)
    lw    a6,  RV_STK_A6(sp)
    lw    a7,  RV_STK_A7(sp)
    lw    s2,  RV_STK_S2(sp)
    lw    s3,  RV_STK_S3(sp)
    lw    s4,  RV_STK_S4(sp)
    lw    s5,  RV_STK_S5(sp)
    lw    s6,  RV_STK_S6(sp)
    lw    s7,  RV_STK_S7(sp)
    lw    s8,  RV_STK_S8(sp)
    lw    s9,  RV_STK_S9(sp)
    lw    s10, RV_STK_S10(sp)
    lw    s11, RV_STK_S11(sp)
    lw    t3,  RV_STK_T3(sp)
    lw    t4,  RV_STK_T4(sp)
    lw    t5,  RV_STK_T5(sp)
    lw    t6,  RV_STK_T6(sp)

    /* Park the hypervisor stack top back in mscratch, then take the guest's sp.
     * t0 and sp must be restored last: t0 carries the value, sp is the base. */
    addi  t0, sp, RV_STK_FRMSZ
    csrw  mscratch, t0
    lw    t0, RV_STK_T0(sp)
    lw    sp, RV_STK_SP(sp)
    mret

hyp_unhandled:
    /* Not the guest's. IDF's _panic_handler is unusable here: it re-arms the RTC
     * watchdog and expects a task stack and a running scheduler. Report it
     * ourselves instead. Does not return. */
    mv    a0, sp
    call  hyp_panic
    .size hyp_trap_entry, .-hyp_trap_entry
