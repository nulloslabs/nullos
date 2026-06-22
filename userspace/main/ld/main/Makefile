ifeq ($(filter --no-print-directory,$(MAKEFLAGS)),)
MAKEFLAGS += --no-print-directory
endif
ifeq ($(filter --silent,$(MAKEFLAGS)),)
MAKEFLAGS += --silent
endif

.DEFAULT_GOAL := all

include ../../../config.mk

SRC = $(wildcard *.c) $(wildcard *.S)
OBJ = $(SRC:.c=.o)
OBJ := $(OBJ:.S=.o)
DEP = $(OBJ:.o=.d)

-include $(DEP)

all: $(OBJ)

%.o: %.c Makefile ../../../config.mk
	@printf "  %-7s %s\n" "CC" "$(SUBDIR)/main/ld/main/$@"
	@$(CC) $(CFLAGS) -fno-stack-protector -c $< -o $@

%.o: %.S Makefile ../../../config.mk
	@printf "  %-7s %s\n" "AS" "$(SUBDIR)/main/ld/main/$@"
	@$(AS) $(AFLAGS) -fno-stack-protector -c $< -o $@

clean:
	@printf "  %-7s %s\n" "CLEAN" "$(SUBDIR)/main/ld/main/*.o $(SUBDIR)/main/ld/main/*.d"
	@rm -f *.o *.d

mrproper: clean

.PHONY: all clean mrproper
