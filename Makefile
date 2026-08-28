ifeq ($(filter --no-print-directory,$(MAKEFLAGS)),)
MAKEFLAGS += --no-print-directory
endif
ifeq ($(filter --silent,$(MAKEFLAGS)),)
MAKEFLAGS += --silent
endif

include config.mk

all: iso

tools:
	@$(MAKE) -C tools

kernel:
	@$(MAKE) -C kernel

initrd:
	@$(MAKE) -C initrd

iso:
	@$(MAKE) -C iso

qemu:
	@printf "  %-7s %s\n" "QEMU" "$(ISOFILE)"
	@$(QEMU) $(QEMUFLAGS) -cdrom $(ISOFILE) -boot d

clean:
	@$(MAKE) -C tools clean
	@$(MAKE) -C kernel clean
	@$(MAKE) -C initrd clean
	@$(MAKE) -C iso clean

mrproper:
	@$(MAKE) -C tools mrproper
	@$(MAKE) -C kernel mrproper
	@$(MAKE) -C initrd mrproper
	@$(MAKE) -C iso mrproper

.PHONY: all tools kernel initrd iso qemu clean mrproper
