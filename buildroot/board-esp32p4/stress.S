/* Exercise the relocation types mkflt.lua claims to handle:
 *   R_RISCV_PCREL_HI20 / LO12_I   lla
 *   R_RISCV_PCREL_LO12_S          a store through an lla'd address
 *   R_RISCV_CALL                  call to a local function
 *   R_RISCV_BRANCH / R_RISCV_JAL  a loop
 * plus .data and .bss sections, which must be laid out contiguously with the
 * text for the PC-relative addressing above to reach them.
 *
 * A `.word msg` in .data was here too and is deliberately gone: an absolute
 * pointer needs a load-time relocation, and a single-segment flat binary has
 * no relocation table. mkflt.lua refuses it by name. See mkflt.lua's header.
 */
	.section .rodata
msg:
	.ascii	"stress ok\n"
	.equ	msglen, . - msg

	.section .data
	.balign	4
counter:
	.word	0

	.section .bss
	.balign	4
scratch:
	.space	64

	.section .text
	.globl _start

	.equ	SYS_write, 64
	.equ	SYS_exit,  93

_start:
	/* store something into .bss through an lla'd address (LO12_S) */
	lla	t0, scratch
	li	t1, 0x5a
	sw	t1, 0(t0)

	/* loop three times, calling write each time */
	li	s0, 3
1:
	beqz	s0, 2f
	call	do_write
	addi	s0, s0, -1
	j	1b
2:
	/* one more write, this time addressing the string directly */
	lla	a1, msg
	li	a0, 1
	li	a2, msglen
	li	a7, SYS_write
	ecall

	li	a0, 0
	li	a7, SYS_exit
	ecall

do_write:
	addi	sp, sp, -16
	sw	ra, 12(sp)
	li	a0, 1
	lla	a1, msg
	li	a2, msglen
	li	a7, SYS_write
	ecall
	lw	ra, 12(sp)
	addi	sp, sp, 16
	ret
