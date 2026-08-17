DTS  := $(wildcard p4-phase*.dts)
DTB  := $(DTS:.dts=.dtb)
HDR  := $(DTS:.dts=_dtb.h)

# Which phase the hypervisor build should embed.
PHASE ?= 1

all: $(DTB) guest_dtb.h

%.dtb: %.dts
	dtc -I dts -O dtb -W no-unit_address_vs_reg -o $@ $<

# C header so the ESP-IDF app can embed the blob directly
define MKHDR
	{ echo "/* generated from $(1) — do not edit */"; \
	  echo "#pragma once"; \
	  echo "#include <stdint.h>"; \
	  echo "static const uint8_t guest_dtb[] __attribute__((aligned(8))) = {"; \
	  od -An -v -tx1 $(1) | sed -e 's/[0-9a-f][0-9a-f]/0x&,/g' -e 's/^/ /'; \
	  echo "};"; \
	  echo "#define GUEST_DTB_SIZE $$(stat -c%s $(1))"; } > $(2)
endef

%_dtb.h: %.dtb
	$(call MKHDR,$<,$@)

guest_dtb.h: p4-phase$(PHASE).dtb
	$(call MKHDR,$<,$@)
	@echo "guest_dtb.h <- phase $(PHASE) ($$(stat -c%s $<) bytes)"

# Show what Linux will actually parse
check: $(DTB)
	@for f in $(DTB); do \
		echo "=== $$f ==="; \
		dtc -I dtb -O dts $$f 2>&1 | head -20; \
	done

dump: p4-phase$(PHASE).dtb
	fdtdump $<

clean:
	rm -f $(DTB) $(HDR) guest_dtb.h

.PHONY: all check dump clean
