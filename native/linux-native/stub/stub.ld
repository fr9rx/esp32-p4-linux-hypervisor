/*
 * Linker script for the bare-metal boot stub.
 * Linked at 0x48000000 — start of PSRAM on ESP32-P4.
 * The boot shim copies stub.bin to this address and jumps to it.
 */

ENTRY(_start)

MEMORY {
    psram (rwx) : ORIGIN = 0x48000000, LENGTH = 64K
}

SECTIONS {
    . = 0x48000000;

    .text : {
        *(.text.start)      /* entry point first */
        *(.text*)
    } > psram

    .rodata : {
        *(.rodata*)
    } > psram

    .data : {
        *(.data*)
    } > psram

    .bss : {
        *(.bss*)
        *(COMMON)
    } > psram

    _end = .;
}
