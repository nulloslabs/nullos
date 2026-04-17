ifeq ($(filter --no-print-directory,$(MAKEFLAGS)),)
MAKEFLAGS += --no-print-directory
endif
ifeq ($(filter --silent,$(MAKEFLAGS)),)
MAKEFLAGS += --silent
endif

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
	@printf "  %-7s %s\n" "RUN" "iso/system.iso"
	@qemu-system-x86_64 -cdrom iso/system.iso -enable-kvm -smp 1 -m 512 -serial stdio -audiodev alsa,id=audio0 -device ac97,audiodev=audio0 -netdev user,id=net0 -device rtl8139,netdev=net0

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
