/*
 * stub.S — minimal bare-metal RV32 test "kernel"
 *
 * Linked at 0x48000000 (start of PSRAM).
 * Assumes UART0 is already configured at 115200 by the ESP-IDF boot shim.
 * Prints banner + registers, then loops in WFI.
 *
 * ESP32-P4 UART0 (hw_ver1):
 *   Base:          0x500CA000
 *   FIFO (+0x00):  write byte to TX FIFO
 *   STATUS (+0x1C): bits [23:16] = TXFIFO_CNT; FIFO size = 128
 */

#define UART0_FIFO      0x500CA000
#define UART0_STATUS    0x500CA01C
#define TXFIFO_FULL     127

    .section .text.start, "ax", @progbits
    .global _start
    .type   _start, @function
_start:
    la      sp, _stack_top

    mv      s0, a0          /* save hartid */
    mv      s1, a1          /* save dtb_pa */

    la      a0, msg_start
    call    uart_puts

    la      a0, msg_hartid
    call    uart_puts
    mv      a0, s0
    call    uart_hex32
    la      a0, msg_crlf
    call    uart_puts

    la      a0, msg_dtb
    call    uart_puts
    mv      a0, s1
    call    uart_hex32
    la      a0, msg_crlf
    call    uart_puts

    la      a0, msg_pc
    call    uart_puts
    auipc   a0, 0
    call    uart_hex32
    la      a0, msg_crlf
    call    uart_puts

    la      a0, msg_done
    call    uart_puts

1:  wfi
    j       1b


/* ------------------------------------------------------------------ */
/*
 * uart_puts(a0 = ptr to null-terminated string)
 *
 * putc is inlined — no inner `call`, so `ra` is never clobbered and
 * `ret` correctly returns to the caller.
 */
uart_puts:
    mv      t3, a0
1:  lb      a0, 0(t3)
    beqz    a0, 2f
    /* --- inline uart_putc --- */
    li      t0, UART0_STATUS
3:  lw      t1, 0(t0)
    srli    t1, t1, 16
    andi    t1, t1, 0xff
    li      t2, TXFIFO_FULL
    bge     t1, t2, 3b
    li      t0, UART0_FIFO
    sw      a0, 0(t0)
    /* --- end inline --- */
    addi    t3, t3, 1
    j       1b
2:  ret

/* ------------------------------------------------------------------ */
/*
 * uart_putc(a0 = byte)
 */
uart_putc:
    li      t0, UART0_STATUS
1:  lw      t1, 0(t0)
    srli    t1, t1, 16
    andi    t1, t1, 0xff
    li      t2, TXFIFO_FULL
    bge     t1, t2, 1b
    li      t0, UART0_FIFO
    sw      a0, 0(t0)
    ret

/* ------------------------------------------------------------------ */
/*
 * uart_hex32(a0 = uint32) — print "0x" + 8 hex digits
 *
 * Uses a proper stack frame so `call uart_puts` / `call uart_putc`
 * don't clobber our saved state.
 */
uart_hex32:
    addi    sp, sp, -16
    sw      ra,  12(sp)     /* save return address */
    sw      s2,   8(sp)     /* save s2 (callee-saved) */
    mv      s2, a0          /* keep the value to print in s2 */

    la      a0, msg_0x
    call    uart_puts

    li      t6, 28          /* start from most-significant nibble */
1:  mv      a0, s2
    srl     a0, a0, t6
    andi    a0, a0, 0xf
    li      t0, 10
    blt     a0, t0, 2f
    addi    a0, a0, 'a' - 10
    j       3f
2:  addi    a0, a0, '0'
3:  call    uart_putc
    addi    t6, t6, -4
    bgez    t6, 1b

    lw      ra,  12(sp)
    lw      s2,   8(sp)
    addi    sp, sp, 16
    ret


    .section .rodata, "a", @progbits
msg_start:
    .asciz  "\r\n\n*** Native RV32 stub running on ESP32-P4 ***\r\n"
msg_hartid:
    .asciz  "  hartid = "
msg_dtb:
    .asciz  "  dtb_pa = "
msg_pc:
    .asciz  "  pc     = "
msg_done:
    .asciz  "Boot chain OK -- looping in WFI\r\n"
msg_crlf:
    .asciz  "\r\n"
msg_0x:
    .asciz  "0x"


    .section .bss, "aw", @nobits
    .balign 16
    .space  4096
_stack_top:
