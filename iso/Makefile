ifeq ($(filter --no-print-directory,$(MAKEFLAGS)),)
MAKEFLAGS += --no-print-directory
endif
ifeq ($(filter --silent,$(MAKEFLAGS)),)
MAKEFLAGS += --silent
endif

include config.mk

all: iso

kernel:
	@$(MAKE) -C ../kernel

rootfs:
	@$(MAKE) -C ../rootfs

tools:
	@$(MAKE) -C ../tools

iso: tools kernel rootfs
	@if [ -f $(OUTFILE) ] || [ $(OUTFILE) -nt ../kernel/nullkrnl ] || \
	    [ $(OUTFILE) -nt Makefile ] || \
	    [ $(OUTFILE) -nt config.mk ] || \
	    [ $(OUTFILE) -nt ../rootfs/rootfs.tar.gz ] || \
	    [ $(OUTFILE) -nt ../config/limine.conf ] || \
		[ $(OUTFILE) -nt ../tools/limine-binary/limine ] || \
	    [ $(OUTFILE) -nt ../tools/limine-binary/limine-bios.sys ] || [ $(OUTFILE) -nt ../tools/limine-binary/limine-bios-cd.bin ] || [ $(OUTFILE) -nt ../tools/limine-binary/limine-uefi-cd.sys ] || \
		[ $(OUTFILE) -nt ../tools/limine-binary/BOOTX64.EFI ]; then \
		printf "  %-7s %s\n" "INFO" "$(SUBDIR)/system.iso is already up to date"; \
	else \
		printf "  %-7s %s\n" "XORRISO" "$(SUBDIR)/system.iso" && \
		mkdir -p main/boot main/EFI/BOOT && \
		cp ../tools/limine-binary/limine-bios.sys ../tools/limine-binary/limine-bios-cd.bin ../tools/limine-binary/limine-uefi-cd.bin main/boot && \
		cp ../tools/limine-binary/BOOTX64.EFI main/EFI/BOOT && \
		cp ../kernel/nullkrnl main/boot && \
		cp ../rootfs/rootfs.tar.gz main/boot && \
		cp ../config/limine.conf main/boot && \
		$(XORRISO) -as mkisofs $(XORRISOFLAGS) main -o $(OUTFILE) > /dev/null 2>&1 && \
		../tools/limine-binary/limine bios-install $(OUTFILE) > /dev/null 2>&1; \
	fi

clean:
	@printf "  %-7s %s\n" "CLEAN" "$(SUBDIR)/$(OUTFILE)"
	@rm -rf $(OUTFILE)

mrproper: clean
	@printf "  %-7s %s\n" "CLEAN" "$(SUBDIR)/main/"
	@rm -rf main

.PHONY: all iso tools kernel rootfs userspace clean mrproper
