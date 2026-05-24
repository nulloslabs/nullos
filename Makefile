ifeq ($(filter --no-print-directory,$(MAKEFLAGS)),)
MAKEFLAGS += --no-print-directory
endif
ifeq ($(filter --silent,$(MAKEFLAGS)),)
MAKEFLAGS += --silent
endif

include config.mk

all: iso

kernel:
	@$(MAKE) -C kernel

userspace:
	@$(MAKE) -C userspace

rootfs:
	@$(MAKE) -C rootfs

tools:
	@$(MAKE) -C tools

iso:
	@$(MAKE) -C iso

run:
	@printf "  %-7s %s\n" "RUN" "$(ISOFILE)"
	@$(QEMU) $(QEMUFLAGS) -cdrom $(ISOFILE)

clean:
	@$(MAKE) -C kernel clean
	@$(MAKE) -C rootfs clean
	@$(MAKE) -C tools clean
	@$(MAKE) -C iso clean
	@$(MAKE) -C userspace clean

mrproper:
	@$(MAKE) -C kernel mrproper
	@$(MAKE) -C rootfs mrproper
	@$(MAKE) -C tools mrproper
	@$(MAKE) -C iso mrproper
	@$(MAKE) -C userspace mrproper

.PHONY: all tools kernel rootfs userspace iso run clean mrproper
