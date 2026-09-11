/* Guest instruction fetch, decode and register access
 *
 * Split out of main.c, which had grown to 2,300 lines. hyp.h carries the
 * contract between the modules and the map of which file does what.
 */
#include "hyp.h"

/* ===========================================================================
 * Guest register access
 *
 * RvExcFrame starts with mepc, then x1..x31, so ((uint32_t *)frame)[n] is
 * exactly x[n] for n in 1..31. Index 0 lands on mepc, which is why x0 must be
 * special-cased in both directions -- a raw write to index 0 would corrupt the
 * return address instead of being discarded.
 * ===========================================================================
 */

uint32_t reg_read(RvExcFrame *frame, uint32_t index)
{
    return index ? (uint32_t)((uint32_t *)frame)[index] : 0u;
}

void reg_write(RvExcFrame *frame, uint32_t index, uint32_t value)
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
bool guest_fetch(uint32_t mepc, uint32_t *encoding, uint32_t *length)
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
bool trap_decode(GuestTrap *trap)
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


