/*
 * hello.S -- rv32 assembly on the ESP32-P4 hypervisor guest, raw Linux
 * syscalls, no libc calls.
 *
 * Runs as a U-mode process inside a NOMMU Linux guest, which is itself a
 * U-mode guest of a hand-written M-mode monitor. So an `ecall` here takes a
 * route no ecall was designed for:
 *
 *     U-mode program   -- ecall -->  traps to M-mode
 *     M-mode monitor   reflects it back into the guest kernel's handler
 *     guest kernel     runs sys_write, which writes to the emulated 16550
 *     that MMIO store  traps to M-mode again
 *     monitor          puts the byte on the real UART
 *
 * Two of those steps are traps the monitor emulates, which is why the guest
 * console costs about 1.56 traps per character.
 *
 * Build:
 *     riscv32-linux-gcc -Os -fPIC -static -Wl,--no-relax \
 *         -Wl,-elf2flt=-r hello.S -o hello
 *
 * ---------------------------------------------------------------------------
 * WHY -Wl,--no-relax IS NOT OPTIONAL FOR A SMALL FLAT BINARY
 *
 * Without it this builds, converts and installs cleanly, and then dies:
 *
 *   hello[35]: unhandled signal 11 code 0x2 at 0x48dfa2dc
 *   status: 00000080 badaddr: 000007a4 cause: 00000007
 *
 * cause 7 is a store access fault and badaddr 0x7a4 is a LINK-TIME offset --
 * an address that was never relocated to where the program actually loaded.
 * Disassembling uClibc's __uClibc_main in the output shows why:
 *
 *   2f8:  76f02823   sw  a5,1904(zero)   # 770 <__uclibc_progname>
 *
 * A store to absolute address 0x770 through x0. A flat binary is linked at
 * address 0, so every address in a small one falls inside the +/-2048 range
 * an x0-relative instruction can reach -- and the linker's relaxation pass
 * duly rewrites GOT-relative accesses into absolute ones. bFLT has no
 * relocation type that can fix those up afterwards, so they keep pointing at
 * offset-from-zero forever.
 *
 * This is why every other program on the board is fine: lua is 300 kB and
 * busybox 474 kB, so their data sits far beyond 2048 and the relaxation is
 * never possible. The bug only appears in binaries small enough to fit
 * entirely in the low 2 kB -- which is exactly what a hand-written assembly
 * example is.
 *
 * Note it is the LINK-time pass that matters: -mno-relax on the compiler
 * changes which relocations survive but does not prevent this.
 * ---------------------------------------------------------------------------
 *
 * ---------------------------------------------------------------------------
 * WHY THIS DEFINES main AND NOT _start
 *
 * The obvious version -- `_start`, `-nostdlib -nostartfiles`, exit via
 * syscall 93 -- assembles fine and then fails to convert:
 *
 *     ERROR: reloc type R_RISCV_PCREL_HI20 unsupported in this context
 *     ERROR: reloc type R_RISCV_PCREL_LO12_I unsupported in this context
 *     ERROR: reloc type R_RISCV_JAL unsupported in this context
 *
 * The cause is not the assembly. ld-elf2flt links with `-q` (--emit-relocs),
 * so EVERY relocation survives into the executable -- including the
 * PC-relative ones the linker already resolved -- and elf2flt then has to
 * classify each one. Its RISC-V case list (elf2flt.c, ~line 828) covers
 * R_RISCV_32/64 and the ADD/SUB/SET family and nothing else; PCREL_HI20,
 * PCREL_LO12_I, GPREL_I and JAL all fall through to `bad_resolved_reloc`.
 *
 * A normal link does not leave those behind, which is why every package on
 * this board builds and this file did not. Verified by writing the same
 * program in C: with -nostdlib it fails identically, so it was never an
 * assembly-versus-C question -- it was -nostdlib.
 *
 * So: link as an ordinary program. crt1 provides _start, calls main, and
 * turns the return value into exit(). The cost is about 1.9 kB total, and
 * the syscalls below are still made directly.
 *
 * (-mno-relax changes which relocations are left over -- GPREL_I becomes
 * PCREL_* -- but not whether elf2flt accepts them. It is not the fix.)
 * ---------------------------------------------------------------------------
 */

	.section .text
	.globl main

/* The message comes first, and that is not a style choice: `li a2, msglen`
 * needs msglen to be a constant the assembler already knows, and
 * `.equ msglen, . - msg` cannot be a forward reference. Putting the string
 * after main gives "Error: illegal operands `li a2,msglen'".
 *
 * Keeping it in .text rather than .rodata also means `lla` resolves it
 * PC-relative within the same section. */
msg:
	.ascii	"Hello from rv32 assembly.\n"
	.ascii	"U-mode process, in a NOMMU Linux guest, in U-mode,\n"
	.ascii	"under a hand-written M-mode hypervisor, on an ESP32-P4.\n"
	.equ	msglen, . - msg

/* REQUIRED. The string above is an odd number of bytes, so without this the
 * location counter is odd when main: starts and the first instruction is
 * misaligned. It assembles, links and converts to bFLT without a word of
 * complaint, and then the guest takes a fault the moment it is run:
 *
 *   HYP trace (injecting access fault), ... traps
 *
 * Putting data in .text is fine; forgetting to realign after it is not.
 */
	.balign	4

/* Linux/riscv syscall ABI: a7 = number, a0-a5 = arguments, ecall.
 * The return value comes back in a0. */
	.equ	SYS_write, 64

main:
	/* write(1, msg, msglen) -- a direct ecall, not a libc call. */
	li	a0, 1			/* fd: stdout */
	lla	a1, msg			/* buf: PC-relative, no GOT */
	li	a2, msglen		/* count */
	li	a7, SYS_write
	ecall

	/* Return 0. crt1 passes this to exit(), so there is no need for
	 * syscall 93 here -- and a0 currently holds the byte count, which
	 * would otherwise become a non-zero exit status. */
	li	a0, 0
	ret
