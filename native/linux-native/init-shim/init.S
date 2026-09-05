/* Tiny /init for ESP32-P4 Linux.
 *
 * Works around busybox sh's failure to exec via BINFMT_SCRIPT on this
 * RV32 M-mode NOMMU port. Pure asm, no libc, no heap-allocated strings.
 * Every string is assembled on the stack with byte stores so elf2flt
 * sees no absolute data relocs (it only accepts R_RISCV_32 on .data,
 * same constraint as probe.S).
 *
 * Sequence:
 *   mount("devtmpfs", "/dev", "devtmpfs", 0, NULL)
 *   fd = open("/dev/console", O_RDWR)
 *   dup3(fd, 0, 0); dup3(fd, 1, 0); dup3(fd, 2, 0)
 *   execve("/sbin/init", argv, envp)
 *     argv = { "/sbin/init", NULL }
 *     envp = { "HOME=/", "TERM=linux", "PATH=/sbin:/bin:/usr/sbin:/usr/bin", NULL }
 *   if execve fails: write("I:execve failed\n") then exit(1)
 *
 * Syscalls (RV generic):
 *   execve  221
 *   mount    40
 *   openat   56  (open is legacy; use openat(AT_FDCWD, ...))
 *   dup3     24
 *   close    57
 *   write    64
 *   exit_group 94
 */
    .option nopic
    .option norelax
    .text
    .globl _start
_start:
    /* Reserve a chunk of stack for string scratch. 256 bytes covers
     * everything we need and keeps sp 16-aligned. */
    addi    sp, sp, -256

    /*
     * Lay out all strings on the stack. Use sp-relative offsets.
     * Each is NUL-terminated. No relocations are required because
     * everything is built byte-by-byte via li/sb.
     *
     * Layout (offsets from sp):
     *    0: "devtmpfs\0"                  (source + fstype)
     *   16: "/dev\0"
     *   24: "/dev/console\0"
     *   40: "/sbin/init\0"
     *   56: "HOME=/\0"
     *   64: "TERM=linux\0"
     *   80: "PATH=/sbin:/bin:/usr/sbin:/usr/bin\0"
     *  128: argv[0] = &"/sbin/init"   (word)
     *  132: argv[1] = 0                (word)
     *  136: envp[0] = &"HOME=/"        (word)
     *  140: envp[1] = &"TERM=linux"    (word)
     *  144: envp[2] = &"PATH=..."      (word)
     *  148: envp[3] = 0                (word)
     *  160: "I:execve failed\n"        (16 bytes + NUL)
     */

    /* "devtmpfs\0" at sp+0 */
    li t0, 'd'; sb t0, 0(sp)
    li t0, 'e'; sb t0, 1(sp)
    li t0, 'v'; sb t0, 2(sp)
    li t0, 't'; sb t0, 3(sp)
    li t0, 'm'; sb t0, 4(sp)
    li t0, 'p'; sb t0, 5(sp)
    li t0, 'f'; sb t0, 6(sp)
    li t0, 's'; sb t0, 7(sp)
    sb zero,   8(sp)

    /* "/dev\0" at sp+16 */
    li t0, '/'; sb t0, 16(sp)
    li t0, 'd'; sb t0, 17(sp)
    li t0, 'e'; sb t0, 18(sp)
    li t0, 'v'; sb t0, 19(sp)
    sb zero,   20(sp)

    /* "/dev/console\0" at sp+24 */
    li t0, '/'; sb t0, 24(sp)
    li t0, 'd'; sb t0, 25(sp)
    li t0, 'e'; sb t0, 26(sp)
    li t0, 'v'; sb t0, 27(sp)
    li t0, '/'; sb t0, 28(sp)
    li t0, 'c'; sb t0, 29(sp)
    li t0, 'o'; sb t0, 30(sp)
    li t0, 'n'; sb t0, 31(sp)
    li t0, 's'; sb t0, 32(sp)
    li t0, 'o'; sb t0, 33(sp)
    li t0, 'l'; sb t0, 34(sp)
    li t0, 'e'; sb t0, 35(sp)
    sb zero,   36(sp)

    /* "/sbin/init\0" at sp+40 */
    li t0, '/'; sb t0, 40(sp)
    li t0, 's'; sb t0, 41(sp)
    li t0, 'b'; sb t0, 42(sp)
    li t0, 'i'; sb t0, 43(sp)
    li t0, 'n'; sb t0, 44(sp)
    li t0, '/'; sb t0, 45(sp)
    li t0, 'i'; sb t0, 46(sp)
    li t0, 'n'; sb t0, 47(sp)
    li t0, 'i'; sb t0, 48(sp)
    li t0, 't'; sb t0, 49(sp)
    sb zero,   50(sp)

    /* "HOME=/\0" at sp+56 */
    li t0, 'H'; sb t0, 56(sp)
    li t0, 'O'; sb t0, 57(sp)
    li t0, 'M'; sb t0, 58(sp)
    li t0, 'E'; sb t0, 59(sp)
    li t0, '='; sb t0, 60(sp)
    li t0, '/'; sb t0, 61(sp)
    sb zero,   62(sp)

    /* "TERM=linux\0" at sp+64 */
    li t0, 'T'; sb t0, 64(sp)
    li t0, 'E'; sb t0, 65(sp)
    li t0, 'R'; sb t0, 66(sp)
    li t0, 'M'; sb t0, 67(sp)
    li t0, '='; sb t0, 68(sp)
    li t0, 'l'; sb t0, 69(sp)
    li t0, 'i'; sb t0, 70(sp)
    li t0, 'n'; sb t0, 71(sp)
    li t0, 'u'; sb t0, 72(sp)
    li t0, 'x'; sb t0, 73(sp)
    sb zero,   74(sp)

    /* "PATH=/sbin:/bin:/usr/sbin:/usr/bin\0" at sp+80 (34 chars + NUL) */
    li t0, 'P'; sb t0, 80(sp)
    li t0, 'A'; sb t0, 81(sp)
    li t0, 'T'; sb t0, 82(sp)
    li t0, 'H'; sb t0, 83(sp)
    li t0, '='; sb t0, 84(sp)
    li t0, '/'; sb t0, 85(sp)
    li t0, 's'; sb t0, 86(sp)
    li t0, 'b'; sb t0, 87(sp)
    li t0, 'i'; sb t0, 88(sp)
    li t0, 'n'; sb t0, 89(sp)
    li t0, ':'; sb t0, 90(sp)
    li t0, '/'; sb t0, 91(sp)
    li t0, 'b'; sb t0, 92(sp)
    li t0, 'i'; sb t0, 93(sp)
    li t0, 'n'; sb t0, 94(sp)
    li t0, ':'; sb t0, 95(sp)
    li t0, '/'; sb t0, 96(sp)
    li t0, 'u'; sb t0, 97(sp)
    li t0, 's'; sb t0, 98(sp)
    li t0, 'r'; sb t0, 99(sp)
    li t0, '/'; sb t0, 100(sp)
    li t0, 's'; sb t0, 101(sp)
    li t0, 'b'; sb t0, 102(sp)
    li t0, 'i'; sb t0, 103(sp)
    li t0, 'n'; sb t0, 104(sp)
    li t0, ':'; sb t0, 105(sp)
    li t0, '/'; sb t0, 106(sp)
    li t0, 'u'; sb t0, 107(sp)
    li t0, 's'; sb t0, 108(sp)
    li t0, 'r'; sb t0, 109(sp)
    li t0, '/'; sb t0, 110(sp)
    li t0, 'b'; sb t0, 111(sp)
    li t0, 'i'; sb t0, 112(sp)
    li t0, 'n'; sb t0, 113(sp)
    sb zero,   114(sp)

    /* Build argv/envp pointer arrays at sp+128 / sp+136. */
    addi t0, sp, 40      /* &"/sbin/init" */
    sw t0, 128(sp)
    sw zero, 132(sp)

    addi t0, sp, 56      /* &"HOME=/" */
    sw t0, 136(sp)
    addi t0, sp, 64      /* &"TERM=linux" */
    sw t0, 140(sp)
    addi t0, sp, 80      /* &"PATH=..." */
    sw t0, 144(sp)
    sw zero, 148(sp)

    /*
     * mount("devtmpfs", "/dev", "devtmpfs", 0, NULL)
     *   a0 = sp+0  (source)
     *   a1 = sp+16 (target)
     *   a2 = sp+0  (fstype)
     *   a3 = 0     (flags)
     *   a4 = 0     (data)
     *   a7 = 40
     * Ignore return value — /dev may already be mounted on some boots.
     */
    mv      a0, sp
    addi    a1, sp, 16
    mv      a2, sp
    li      a3, 0
    li      a4, 0
    li      a7, 40
    ecall

    /*
     * fd = openat(AT_FDCWD=-100, "/dev/console", O_RDWR=2, 0)
     *   a0 = -100
     *   a1 = sp+24
     *   a2 = 2
     *   a3 = 0
     *   a7 = 56
     */
    li      a0, -100
    addi    a1, sp, 24
    li      a2, 2
    li      a3, 0
    li      a7, 56
    ecall
    /* Save fd in s0; skip dup3 if open failed (a0 < 0). */
    mv      s0, a0
    bltz    s0, .Lskip_dup

    /* dup3(fd, 0, 0) — syscall 24 */
    mv      a0, s0
    li      a1, 0
    li      a2, 0
    li      a7, 24
    ecall

    /* dup3(fd, 1, 0) */
    mv      a0, s0
    li      a1, 1
    li      a2, 0
    li      a7, 24
    ecall

    /* dup3(fd, 2, 0) */
    mv      a0, s0
    li      a1, 2
    li      a2, 0
    li      a7, 24
    ecall

    /* close original fd if >2 */
    li      t0, 2
    ble     s0, t0, .Lskip_dup
    mv      a0, s0
    li      a7, 57
    ecall

.Lskip_dup:
    /*
     * execve("/sbin/init", argv, envp)
     *   a0 = sp+40
     *   a1 = sp+128
     *   a2 = sp+136
     *   a7 = 221
     */
    addi    a0, sp, 40
    addi    a1, sp, 128
    addi    a2, sp, 136
    li      a7, 221
    ecall

    /* execve returned — it failed. Write a diagnostic, then exit. */
    /* "I:execve failed\n" at sp+160, 16 bytes */
    li t0, 'I'; sb t0, 160(sp)
    li t0, ':'; sb t0, 161(sp)
    li t0, 'e'; sb t0, 162(sp)
    li t0, 'x'; sb t0, 163(sp)
    li t0, 'e'; sb t0, 164(sp)
    li t0, 'c'; sb t0, 165(sp)
    li t0, 'v'; sb t0, 166(sp)
    li t0, 'e'; sb t0, 167(sp)
    li t0, ' '; sb t0, 168(sp)
    li t0, 'f'; sb t0, 169(sp)
    li t0, 'a'; sb t0, 170(sp)
    li t0, 'i'; sb t0, 171(sp)
    li t0, 'l'; sb t0, 172(sp)
    li t0, 'e'; sb t0, 173(sp)
    li t0, 'd'; sb t0, 174(sp)
    li t0, 0x0a; sb t0, 175(sp)

    /* write(1, sp+160, 16) */
    li      a0, 1
    addi    a1, sp, 160
    li      a2, 16
    li      a7, 64
    ecall

    /* exit_group(1) */
    li      a0, 1
    li      a7, 94
    ecall

    /* unreachable */
1:  j 1b

    /* Dummy reloc so elf2flt sees at least one R_RISCV_32 entry. */
    .data
dummy_reloc:
    .word _start
