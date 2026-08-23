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

menuconfig:
	@$(MAKE) -C kernel menuconfig

allyesconfig:
	@$(MAKE) -C kernel allyesconfig

allnoconfig:
	@$(MAKE) -C kernel allnoconfig

alldefconfig:
	@$(MAKE) -C kernel alldefconfig

defconfig:
	@$(MAKE) -C kernel defconfig

oldconfig:
	@$(MAKE) -C kernel oldconfig

olddefconfig:
	@$(MAKE) -C kernel oldefconfig

savedefconfig:
	@$(MAKE) -C kernel savedefconfig

tools:
	@$(MAKE) -C tools

initrd:
	@$(MAKE) -C initrd

iso:
	@$(MAKE) -C iso

qemu:
	@printf "  %-7s %s\n" "QEMU" "$(ISOFILE)"
	@$(QEMU) $(QEMUFLAGS) -cdrom $(ISOFILE)

clean:
	@$(MAKE) -C kernel clean
	@$(MAKE) -C initrd clean
	@$(MAKE) -C tools clean
	@$(MAKE) -C iso clean

mrproper:
	@$(MAKE) -C kernel mrproper
	@$(MAKE) -C initrd mrproper
	@$(MAKE) -C tools mrproper
	@$(MAKE) -C iso mrproper

.PHONY: all kernel menuconfig allyesconfig allnoconfig alldefconfig defconfig oldconfig olddefconfig savedefconfig tools initrd iso qemu clean mrproper
