/* Shadow CSR bank, and the instructions that trap as illegal
 *
 * Split out of main.c, which had grown to 2,300 lines. hyp.h carries the
 * contract between the modules and the map of which file does what.
 */
#include "hyp.h"

#include "riscv/rv_utils.h"
#include "hal/cache_ll.h"

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




/* The type lives in hyp.h: four modules read this bank. */
HypShadowCsr shadow = { .mstatus = (PRIV_M << MSTATUS_MPP_S) };  /* born believing it is M-mode */

/* The guest's software privilege level. Real hardware would distinguish an ecall
 * from the kernel (cause 11) from one out of userspace (cause 8), but here the
 * whole guest runs in U-mode so the hardware always reports 8. This variable is
 * what recovers the distinction, and it moves on exactly two events: a trap
 * injected into the guest sets it to M, and mret restores it from mstatus.MPP.
 *
 * Note this kernel dispatches syscalls from excp_vect_table[8] and requires
 * MPP == 0; excp_vect_table[11] is "Oops - environment call from M-mode". So
 * ecall always injects cause 8, and guest_priv only feeds MPP. */
uint32_t guest_priv = PRIV_M;

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
void hyp_sync_icache(void)
{
    uint32_t t0 = rv_utils_get_cycle_count();
    cache_ll_l1_writeback_dcache_all(CACHE_LL_ID_ALL);
    cache_ll_l1_invalidate_icache_all(CACHE_LL_ID_ALL);
    hyp_stats.icache_syncs++;
    hyp_stats.icache_sync_cycles += (uint32_t)(rv_utils_get_cycle_count() - t0);
}

TrapResult hypervisor_mret(GuestTrap *trap)
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
/* Cycles spent asleep in a blocking wfi. hyp_core.c subtracts these from
 * the trap's measured cost, so an idle guest does not read as a slow one.
 */
uint32_t hyp_idle_cycles;

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
TrapResult hypervisor_wfi(void)
{
#if !HYP_WFI_BLOCKS
    return TRAP_ADVANCE;
#else
    bool timer_armed = ((shadow.mie & MIP_MTIP) != 0u) &&
                       (hyp_mtimecmp() < HYP_MTIME_MAX);
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

TrapResult hypervisor_csr(GuestTrap *trap)
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


