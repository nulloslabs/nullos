ifeq ($(filter --no-print-directory,$(MAKEFLAGS)),)
MAKEFLAGS += --no-print-directory
endif
ifeq ($(filter -s,$(MAKEFLAGS)),)
MAKEFLAGS += -s
endif

include ../config.mk

SRC := $(wildcard *.c) $(wildcard *.S)

include ../Kconfig.mk

OBJ := $(SRC:.c=.o)
OBJ := $(OBJ:.S=.o)
DEP := $(OBJ:.o=.d)

all: $(OBJ)

-include $(DEP)

vmm.o: mm.o

%.o: %.c Makefile ../Makefile ../config.mk $(FORCE_REBUILD) ../include/limine/limine.h
	@printf "  %-7s %s\n" "CC" "$(SUBDIR)/mm/$@"
	@$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S Makefile ../Makefile ../config.mk $(FORCE_REBUILD) ../include/limine/limine.h
	@printf "  %-7s %s\n" "AS" "$(SUBDIR)/mm/$@"
	@$(AS) $(AFLAGS) -c $< -o $@

FORCE:

clean:
	@printf "  %-7s %s\n" "CLEAN" "$(SUBDIR)/mm/*.o $(SUBDIR)/mm/*.d"
	@rm -rf *.o *.d

mrproper: clean

.PHONY: all FORCE clean mrproper
