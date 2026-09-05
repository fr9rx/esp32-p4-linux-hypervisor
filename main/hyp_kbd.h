/*
 * A USB keyboard, wired into the guest's 16550 receive path.
 *
 * The monitor already owns the guest's UART in both directions, so a keyboard
 * does not need the guest to grow an input driver: bytes decoded here go into
 * the same RX ring that bytes from the physical serial port go into, and the
 * guest cannot tell the difference. That is the other half of the reason the
 * terminal is rendered by the monitor rather than by fbcon -- see hyp_console.h.
 *
 * Ownership, which matters because the two cores are not symmetric here:
 *
 *   core 0  hyp_kbd_init()   brings up the USB host stack. Needs FreeRTOS.
 *           the poll thread  decodes HID reports and pushes ASCII into a ring.
 *   core 1  hyp_kbd_pop()    drains that ring into the 16550, from
 *                            hyp_poll_host_devices().
 *
 * Core 1 stays the only writer of the 16550's own ring that way, so no locking
 * is needed anywhere: this ring has one producer (core 0) and one consumer
 * (core 1), and the 16550's has one producer (core 1).
 *
 * Latency note: a keystroke becomes visible to the guest at its next trap. The
 * guest takes timer traps continuously, so that is a tick at worst, and in
 * practice imperceptible. A keystroke cannot itself wake a wfi-blocked guest,
 * because core 0 raises no CLIC line on core 1 -- the pending timer does that.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Bring up the USB host stack. Core 0, needs FreeRTOS. Returns false and leaves
 * the keyboard absent if the stack cannot start; the serial port still works. */
bool hyp_kbd_init(void);

/* Next byte typed, or false if none. Safe from the trap handler on core 1. */
bool hyp_kbd_pop(uint8_t *out);

/* Keyboards seen, HID reports decoded, and bytes dropped on a full ring. */
uint32_t hyp_kbd_devices(void);
uint32_t hyp_kbd_reports(void);
uint32_t hyp_kbd_overruns(void);

/* Type "kbd-ok" as though it came from the keyboard, by driving the HID report
 * decoder with a synthetic report. Covers the whole input path except the USB
 * transfer -- see the note in hyp_kbd.c. Triggered by Ctrl-_ on the serial
 * port; safe to call from the trap handler on core 1. */
void hyp_kbd_selftest(void);
