SECTIONS {
    /DISCARD/ : { *(.eh_frame) *(.eh_frame_hdr) *(.riscv.attributes) *(.comment) *(.note*) *(.init_array*) *(.fini_array*) *(.preinit_array*) }
}
INSERT AFTER .data;
