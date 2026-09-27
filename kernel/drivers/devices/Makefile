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

-include $(DEP)

%.o: %.c Makefile ../../Makefile ../../config.mk $(FORCE_REBUILD) ../../include/limine/limine.h
	@printf "  %-7s %s\n" "CC" "$(SUBDIR)/drivers/devices/$@"
	@$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S Makefile ../../Makefile ../../config.mk $(FORCE_REBUILD) ../../include/limine/limine.h
	@printf "  %-7s %s\n" "AS" "$(SUBDIR)/drivers/devices/$@"
	@$(AS) $(AFLAGS) -c $< -o $@

clean:
	@printf "  %-7s %s\n" "CLEAN" "$(SUBDIR)/drivers/devices/*.o $(SUBDIR)/drivers/devices/*.d"
	@rm -rf *.o *.d

mrproper: clean

.PHONY: all clean mrproper

FORCE:
.PHONY: FORCE
