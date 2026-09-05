CROSS  ?= riscv32-esp-elf
CC      = $(CROSS)-gcc
OBJCOPY = $(CROSS)-objcopy
OBJDUMP = $(CROSS)-objdump

CFLAGS  = -march=rv32imafc -mabi=ilp32f -nostdlib -ffreestanding -O2

all: stub.bin

stub.elf: stub.S stub.ld
	$(CC) $(CFLAGS) -T stub.ld -o $@ stub.S

stub.bin: stub.elf
	$(OBJCOPY) -O binary $< $@
	@echo "stub.bin: $$(wc -c < stub.bin) bytes"
	@echo "Entry point:"
	@$(OBJDUMP) -d stub.elf | head -30

disasm: stub.elf
	$(OBJDUMP) -d $<

clean:
	rm -f stub.elf stub.bin

.PHONY: all clean disasm
