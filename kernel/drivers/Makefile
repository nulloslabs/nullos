ifeq ($(filter --no-print-directory,$(MAKEFLAGS)),)
MAKEFLAGS += --no-print-directory
endif
ifeq ($(filter --silent,$(MAKEFLAGS)),)
MAKEFLAGS += --silent
endif

INCLUDE_DIR := ..

include ../config.mk

SRC = $(wildcard *.c) $(wildcard *.S)
OBJ := $(SRC:.c=.o)
OBJ := $(OBJ:.S=.o)
DEP := $(OBJ:.o=.d)

all: $(OBJ)
	@$(MAKE) -C acpi
	@$(MAKE) -C clock
	@$(MAKE) -C devices
	@$(MAKE) -C disk
	@$(MAKE) -C fb
	@$(MAKE) -C input
	@$(MAKE) -C net
	@$(MAKE) -C pci
	@$(MAKE) -C ps2
	@$(MAKE) -C serial
	@$(MAKE) -C snd
	@$(MAKE) -C timer
	@$(MAKE) -C tty
	@$(MAKE) -C usb

-include $(DEP)

%.o: %.c Makefile ../Makefile ../config.mk $(FORCE_REBUILD) ../include/limine/limine.h
	@printf "  %-7s %s\n" "CC" "$(SUBDIR)/drivers/$@"
	@$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S Makefile ../Makefile ../config.mk $(FORCE_REBUILD) ../include/limine/limine.h
	@printf "  %-7s %s\n" "AS" "$(SUBDIR)/drivers/$@"
	@$(AS) $(AFLAGS) -c $< -o $@

clean:
	@printf "  %-7s %s\n" "CLEAN" "$(SUBDIR)/drivers/*.o $(SUBDIR)/drivers/*.d"
	@rm -rf *.o *.d
	@$(MAKE) -C acpi clean
	@$(MAKE) -C clock clean
	@$(MAKE) -C devices clean
	@$(MAKE) -C disk clean
	@$(MAKE) -C fb clean
	@$(MAKE) -C input clean
	@$(MAKE) -C net clean
	@$(MAKE) -C pci clean
	@$(MAKE) -C ps2 clean
	@$(MAKE) -C serial clean
	@$(MAKE) -C snd clean
	@$(MAKE) -C timer clean
	@$(MAKE) -C tty clean
	@$(MAKE) -C usb clean

mrproper: clean

.PHONY: all clean mrproper

FORCE:
.PHONY: FORCE
