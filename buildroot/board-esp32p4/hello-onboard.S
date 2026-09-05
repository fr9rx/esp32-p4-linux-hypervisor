/*
 * hello-onboard.S -- rv32 assembly you assemble and package ON THE BOARD, with
 * no compiler, no libc and no linker.
 *
 *     cp /usr/share/hello-onboard.S /home && cd /home
 *     mkflt hello-onboard.S
 *     ./hello-onboard
 *
 * hello.S is the same program built by the cross compiler. This one is built
 * on the P4 itself, by `as` and `objcopy` with the relocations applied by
 * /usr/lib/mkflt.lua.
 *
 * ---------------------------------------------------------------------------
 * WHY THERE IS NO LINKER HERE
 *
 * ld was on the board and could not run. binfmt_flat makes ONE contiguous
 * allocation per exec, ld is 5,283,840 bytes, and this guest's allocator has
 * no block that big once it has booted:
 *
 *     nommu: Allocation of length 5283840 from process 36 (ld) failed
 *     Normal: ... 3*1024kB 2*2048kB 3*4096kB 0*8192kB = 25400kB
 *
 * 25 MB free and not one block big enough -- every time, not intermittently.
 * Raising MAX_PAGE_ORDER to 11 gave the allocator an 8 MB order, but an 8 MB
 * block on a 29 MB machine only exists while nothing else is churning.
 * elf2flt consumes ld's output, so it went with it.
 *
 * WHAT REPLACED IT
 *
 * mkflt.lua, which lays this file's sections out end to end, applies the
 * relocations `as` left behind, and writes a bFLT header. That is all a linker
 * does for a single freestanding object. lua is 300 kB and was on the board
 * already for other reasons.
 *
 * The practical consequence for you: WRITE ORDINARY ASSEMBLY. `lla` works.
 * `call` works. Branches work. Multiple sections work. The one thing that does
 * not is `la` under `.option pic`, which wants a global offset table that a
 * flat binary built this way has not got -- mkflt.lua names it if you try.
 *
 * (The first version of this file went the other way and tried to avoid
 * relocations entirely, computing addresses with `auipc a1, 0` and a label
 * difference. rv32 gas will not fold a label difference into an instruction
 * immediate under any spelling -- not inline, not via .equ, not with %lo(),
 * not with relaxation off:
 *
 *     Error: illegal operands `addi a1,a1,msg-1b'
 *
 * Only literal constants are accepted. So relocation-free hand-written
 * assembly is not a thing on this architecture, and something has to apply
 * them. Hence mkflt.lua.)
 * ---------------------------------------------------------------------------
 *
 * / is read-only romfs, so work somewhere writable:
 *
 *     cp /usr/share/hello-onboard.S /home && cd /home
 */
/* The data comes first, and that is not a style choice: `li a2, msglen` needs
 * msglen to be a constant the assembler already knows, and `.equ msglen, . -
 * msg` cannot be a forward reference. Put this after the code and you get
 * "Error: illegal operands `li a2,msglen'".
 *
 * .rodata rather than .text: mkflt.lua lays the allocatable sections out end
 * to end, so a second section costs nothing and keeps the data out of the
 * instruction stream. It also means _start is not at offset 0 -- mkflt.lua
 * finds it in the symbol table and sets the bFLT entry point accordingly. */
	.section .rodata
msg:
	.ascii	"Hello from rv32 assembly, assembled and linked\n"
	.ascii	"on the ESP32-P4 itself -- as, objcopy and a Lua\n"
	.ascii	"linker ran as U-mode processes in a NOMMU Linux\n"
	.ascii	"guest, under a hand-written M-mode hypervisor.\n"
	.equ	msglen, . - msg

	.section .text
	.globl _start

/* Linux/riscv syscall ABI: a7 = number, a0-a5 = arguments, ecall.
 * The return value comes back in a0. */
	.equ	SYS_write, 64
	.equ	SYS_exit,  93

_start:
	/* write(1, msg, msglen) */
	li	a0, 1
	lla	a1, msg			/* PCREL_HI20 + PCREL_LO12_I, applied
					 * by mkflt.lua */
	li	a2, msglen
	li	a7, SYS_write
	ecall

	/* exit(0). There is no crt1 here to do it, and falling off the end of
	 * _start runs whatever happens to follow in memory. */
	li	a0, 0
	li	a7, SYS_exit
	ecall
