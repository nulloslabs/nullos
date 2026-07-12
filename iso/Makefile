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

initrd:
	@$(MAKE) -C ../initrd

tools:
	@$(MAKE) -C ../tools

iso: tools kernel initrd
	@if [ ! -f $(OUTFILE) ] || [ ../kernel/nullkrnl -nt $(OUTFILE) ] || \
	    [ Makefile -nt $(OUTFILE) ] || \
	    [ config.mk -nt $(OUTFILE) ] || \
	    [ ../initrd/initrd.cpio.gz -nt $(OUTFILE) ] || \
	    [ ../config/limine.conf -nt $(OUTFILE) ] || \
		[ ../tools/limine-binary/limine -nt $(OUTFILE) ] || \
	    [ ../tools/limine-binary/limine-bios.sys -nt $(OUTFILE) ] || [ ../tools/limine-binary/limine-bios-cd.bin -nt $(OUTFILE) ] || [ ../tools/limine-binary/limine-uefi-cd.sys -nt $(OUTFILE) ] || \
		[ ../tools/limine-binary/BOOTX64.EFI -nt $(OUTFILE) ]; then \
		printf "  %-7s %s\n" "XORRISO" "$(SUBDIR)/system.iso" && \
		mkdir -p main/boot main/EFI/BOOT && \
		cp ../tools/limine-binary/limine-bios.sys ../tools/limine-binary/limine-bios-cd.bin ../tools/limine-binary/limine-uefi-cd.bin main/boot && \
		cp ../tools/limine-binary/BOOTX64.EFI main/EFI/BOOT && \
		cp ../kernel/nullkrnl main/boot && \
		cp ../initrd/initrd.cpio.gz main/boot && \
		cp ../config/limine.conf main/boot && \
		$(XORRISO) -as mkisofs $(XORRISOFLAGS) main -o $(OUTFILE) > /dev/null 2>&1 && \
		../tools/limine-binary/limine bios-install $(OUTFILE) > /dev/null 2>&1; \
	else \
		printf "  %-7s %s\n" "INFO" "$(SUBDIR)/system.iso is already up to date"; \
	fi

clean:
	@printf "  %-7s %s\n" "CLEAN" "$(SUBDIR)/$(OUTFILE)"
	@rm -rf $(OUTFILE)

mrproper: clean
	@printf "  %-7s %s\n" "CLEAN" "$(SUBDIR)/main/"
	@rm -rf main

.PHONY: all iso tools kernel initrd userspace clean mrproper
