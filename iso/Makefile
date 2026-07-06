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
	@if [ -f $(OUTFILE) ] && [ $(OUTFILE) -nt ../kernel/nullkrnl ] && \
	    [ $(OUTFILE) -nt Makefile ] && \
	    [ $(OUTFILE) -nt config.mk ] && \
	    [ $(OUTFILE) -nt ../rootfs/rootfs.tar.gz ] && \
	    [ $(OUTFILE) -nt ../config/limine.conf ]; then \
		printf "  %-7s %s\n" "INFO" "$(SUBDIR)/system.iso is already up to date"; \
	else \
		printf "  %-7s %s\n" "XORRISO" "$(SUBDIR)/system.iso" && \
		mkdir -p main/boot main/EFI/BOOT && \
		cp ../tools/limine/limine-bios.sys ../tools/limine/limine-bios-cd.bin ../tools/limine/limine-uefi-cd.bin main/boot && \
		cp ../tools/limine/BOOTX64.EFI main/EFI/BOOT && \
		cp ../kernel/nullkrnl main/boot && \
		cp ../rootfs/rootfs.tar.gz main/boot && \
		cp ../config/limine.conf main/boot && \
		$(XORRISO) -as mkisofs $(XORRISOFLAGS) main -o $(OUTFILE) > /dev/null 2>&1 && \
		../tools/limine/limine bios-install $(OUTFILE) > /dev/null 2>&1; \
	fi

clean:
	@printf "  %-7s %s\n" "CLEAN" "$(SUBDIR)/$(OUTFILE)"
	@rm -rf $(OUTFILE)

mrproper: clean
	@printf "  %-7s %s\n" "CLEAN" "$(SUBDIR)/main/"
	@rm -rf main

.PHONY: all iso tools kernel rootfs userspace clean mrproper
