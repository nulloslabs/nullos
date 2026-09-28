ifeq ($(filter --no-print-directory,$(MAKEFLAGS)),)
MAKEFLAGS += --no-print-directory
endif
ifeq ($(filter --silent,$(MAKEFLAGS)),)
MAKEFLAGS += --silent
endif

INCLUDE_DIR := ../..

include ../../config.mk

SRC = $(wildcard *.c) $(wildcard *.S)
OBJ := $(SRC:.c=.o)
OBJ := $(OBJ:.S=.o)
DEP := $(OBJ:.o=.d)

all: $(OBJ)
	@$(MAKE) -C disk
	@$(MAKE) -C gpu
	@$(MAKE) -C net
	@$(MAKE) -C snd
	@$(MAKE) -C usb

-include $(DEP)

%.o: %.c Makefile ../../Makefile ../../config.mk $(FORCE_REBUILD) ../../include/limine/limine.h
	@printf "  %-7s %s\n" "CC" "$(SUBDIR)/drivers/pci/$@"
	@$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S Makefile ../../Makefile ../../config.mk $(FORCE_REBUILD) ../../include/limine/limine.h
	@printf "  %-7s %s\n" "AS" "$(SUBDIR)/drivers/pci/$@"
	@$(AS) $(AFLAGS) -c $< -o $@

clean:
	@printf "  %-7s %s\n" "CLEAN" "$(SUBDIR)/drivers/pci/*.o $(SUBDIR)/drivers/pci/*.d"
	@rm -rf *.o *.d
	@$(MAKE) -C disk clean
	@$(MAKE) -C gpu clean
	@$(MAKE) -C net clean
	@$(MAKE) -C snd clean
	@$(MAKE) -C usb clean

mrproper: clean

.PHONY: all clean mrproper

FORCE:
.PHONY: FORCE
