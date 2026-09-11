/* Emulated 16550: the guest's console
 *
 * Split out of main.c, which had grown to 2,300 lines. hyp.h carries the
 * contract between the modules and the map of which file does what.
 */
#include "hyp.h"

#include "esp_rom_serial_output.h"
#include "hal/uart_ll.h"

/* Whether the guest waits for the physical UART. It does not.
 *
 * The trap histogram said store access faults -- almost all of them UART
 * register writes -- cost 12,000+ cycles each and were most of the time spent
 * in the monitor. Those cycles were the guest waiting for a byte to clear the
 * FIFO of what was then a 115200-baud wire -- worth paying while that wire was
 * slow, and not now that it runs at 4 Mbps.
 *
 * (This was originally justified by input having moved to a USB keyboard, so
 * the wire was "only a log". The keyboard is gone and the serial console is
 * the terminal again -- but the measurement below still holds, because it was
 * always about the baud rate rather than about where input came from.)
 *
 * Getting this to actually work took two attempts, and the first one is worth
 * recording because it looked right and measured as nothing. The original code
 * was
 *
 *     while (esp_rom_output_tx_one_char(value) != 0) { }
 *
 * and dropping the loop changed the histogram by 0.4% -- because the ROM
 * function spins on the FIFO level internally and then always reports success.
 * The loop around it never iterated. There is no "try once" mode to ask for;
 * the only way not to wait is to not call it.
 *
 * So the FIFO is written directly and the byte is dropped when there is no
 * room. What that costs in principle is a console log with holes in it during
 * any burst. In practice it costs nothing measurable any more, because the
 * console now runs at 4 Mbps rather than 115200 and the guest no longer outruns
 * the wire: `tx dropped` was 3392 per boot at 115200, 61 at 2 Mbps and 0 at 3
 * Mbps and above. Drop the baud back down and the holes come back -- the
 * counter in the histogram is how you find out.
 *
 * The monitor's own output is unaffected either way: esp_rom_printf() from core
 * 0 still blocks, so the trap histogram and every I:/W:/E: line stay
 * complete regardless. Set this back to 1 for a guaranteed
 * faithful capture of the guest's own output. */
#define HYP_UART_LOG_BLOCKING 0

/* ===========================================================================
 * Emulated 16550 UART
 *
 * Linux reaches this first through earlycon=uart8250,mmio,0x10000000 (polled),
 * then through the real serial8250 driver once console=ttyS0 hands over. The
 * handover is where input, IIR, DLAB and the TX interrupt start to matter.
 * ===========================================================================
 */

#define UART_RX_RING_SIZE   512u    /* 512 keystrokes; the producer is the
                                     * 4 Mbps serial wire and the consumer is
                                     * the guest's next MMIO read */

static struct {
    uint8_t  ier;       /* 1: bit0 ERBFI (rx avail), bit1 ETBEI (thr empty) */
    uint8_t  lcr;       /* 3: bit7 DLAB */
    uint8_t  mcr;       /* 4: bit4 loopback */
    uint8_t  scr;       /* 7: scratch */
    uint8_t  fcr;       /* 2 (write): bit0 FIFO enable */
    uint8_t  dll;       /* divisor latch low  (DLAB=1, offset 0) */
    uint8_t  dlm;       /* divisor latch high (DLAB=1, offset 1) */
    uint8_t  ring[UART_RX_RING_SIZE];
    uint32_t head;
    uint32_t tail;
    uint32_t overruns;
    uint32_t tx_dropped;    /* guest output the wire could not keep up with */
} uart16550;

static bool uart_rx_empty(void)
{
    return uart16550.head == uart16550.tail;
}

void uart_rx_push(uint8_t c)
{
    uint32_t next = (uart16550.head + 1u) % UART_RX_RING_SIZE;
    if (next == uart16550.tail) {
        uart16550.overruns++;       /* drop; never block inside a trap handler */
        return;
    }
    uart16550.ring[uart16550.head] = c;
    uart16550.head = next;
}

static uint8_t uart_rx_pop(void)
{
    if (uart_rx_empty()) {
        return 0u;
    }
    uint8_t c = uart16550.ring[uart16550.tail];
    uart16550.tail = (uart16550.tail + 1u) % UART_RX_RING_SIZE;
    return c;
}

/* Recompute the UART's interrupt line into the PLIC.
 *
 * Two conditions raise it, and the second is easy to miss: once the driver
 * takes over from earlycon, serial8250_start_tx() sets IER.ETBEI and then waits
 * for a THRE interrupt to send the rest of its buffer. Our THR is always empty,
 * so ETBEI set means the line is asserted continuously -- exactly what real
 * hardware does. Omit it and output stops dead after the first chunk. */
void v16550_reeval(void)
{
    bool rx_int  = ((uart16550.ier & 0x01u) != 0u) && !uart_rx_empty();
    bool tx_int  = ((uart16550.ier & 0x02u) != 0u);
    plic_set_pending(PLIC_UART_SOURCE, rx_int || tx_int);
}

static uint8_t uart_reg_read(uint32_t offset)
{
    if ((uart16550.lcr & 0x80u) != 0u) {        /* DLAB: divisor latches */
        if (offset == 0u) { return uart16550.dll; }
        if (offset == 1u) { return uart16550.dlm; }
    }

    switch (offset) {
    case 0u: {                                  /* RBR */
        uint8_t c = uart_rx_pop();
        v16550_reeval();
        return c;
    }
    case 1u:
        return uart16550.ier;
    case 2u: {                                  /* IIR */
        /* Returning 0 here means "modem status interrupt pending", which the
         * driver has no handler for. Enough of those and Linux prints
         * "irq N: nobody cared" and disables the line permanently. */
        uint8_t iir;
        if (((uart16550.ier & 0x01u) != 0u) && !uart_rx_empty()) {
            iir = 0x04u;                        /* received data available */
        } else if ((uart16550.ier & 0x02u) != 0u) {
            iir = 0x02u;                        /* transmitter holding empty */
        } else {
            iir = 0x01u;                        /* no interrupt pending */
        }
        return iir | (((uart16550.fcr & 0x01u) != 0u) ? 0xC0u : 0x00u);
    }
    case 3u:
        return uart16550.lcr;
    case 4u:
        return uart16550.mcr;
    case 5u:
        /* LSR. serial8250 spins until (lsr & 0x60) == 0x60: THR empty AND
         * transmitter idle. Bit 0 is DR, driven by the ring. */
        return 0x60u | (uart_rx_empty() ? 0x00u : 0x01u);
    case 6u:
        /* MSR: DCD|DSR|CTS asserted so flow control never blocks. In loopback
         * the driver expects MCR's low bits reflected back. */
        if ((uart16550.mcr & 0x10u) != 0u) {
            return (uint8_t)(((uart16550.mcr & 0x0Cu) << 4) |
                             ((uart16550.mcr & 0x02u) << 3) |
                             ((uart16550.mcr & 0x01u) << 5));
        }
        return 0xB0u;
    case 7u:
        return uart16550.scr;
    default:
        return 0u;
    }
}

static void uart_reg_write(uint32_t offset, uint8_t value)
{
    if ((uart16550.lcr & 0x80u) != 0u) {        /* DLAB: divisor latches */
        if (offset == 0u) { uart16550.dll = value; return; }
        if (offset == 1u) { uart16550.dlm = value; return; }
    }

    switch (offset) {
    case 0u:                                    /* THR */
        if ((uart16550.mcr & 0x10u) != 0u) {
            uart_rx_push(value);                /* loopback: back to our own RX */
        } else {
#if HYP_UART_LOG_BLOCKING
            while (esp_rom_output_tx_one_char(value) != 0) { }
#else
            /* Straight at the FIFO, and dropped if it is full. Deliberately not
             * esp_rom_output_tx_one_char(): that waits for room. */
            uart_dev_t *tx = UART_LL_GET_HW(0);
            if (uart_ll_get_txfifo_len(tx) > 0u) {
                uint8_t byte = (uint8_t)value;
                uart_ll_write_txfifo(tx, &byte, 1u);
            } else {
                uart16550.tx_dropped++;
            }
#endif
        }
        v16550_reeval();
        break;
    case 1u:
        uart16550.ier = value;
        v16550_reeval();    /* enabling ETBEI asserts the line immediately: there
                             * is no external event to trigger it later */
        break;
    case 2u:
        uart16550.fcr = value;                  /* FCR (write side of IIR) */
        break;
    case 3u:
        uart16550.lcr = value;
        break;
    case 4u:
        uart16550.mcr = value;
        break;
    case 7u:
        uart16550.scr = value;
        break;
    default:
        break;
    }
}

uint32_t uart_read(uint32_t offset, uint32_t width)
{
    uint32_t value = 0u;
    for (uint32_t i = 0u; i < width; i++) {
        value |= (uint32_t)uart_reg_read(offset + i) << (8u * i);
    }
    return value;
}

void uart_write(uint32_t offset, uint32_t value, uint32_t width)
{
    for (uint32_t i = 0u; i < width; i++) {
        uart_reg_write(offset + i, (uint8_t)(value >> (8u * i)));
    }
}



/* The two counters hyp_trace.c reports.
 *
 * Accessors rather than an exported struct: everything else in uart16550 --
 * IER, LCR, the DLAB latch, the receive ring -- is this module's business,
 * and exporting the lot to reach two fields is how a module stops being one.
 */
uint32_t hyp_uart_overruns(void)   { return uart16550.overruns; }
uint32_t hyp_uart_tx_dropped(void) { return uart16550.tx_dropped; }
