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

%.o: %.c ../../../config.mk
	@printf "  %-7s %s\n" "CC" "$(SUBDIR)/main/libc/__start/$@"
	@$(CC) $(CFLAGS) -fno-stack-protector -c $< -o $@

%.o: %.S ../../../config.mk
	@printf "  %-7s %s\n" "AS" "$(SUBDIR)/main/libc/__start/$@"
	@$(AS) $(AFLAGS) -fno-stack-protector -c $< -o $@

clean:
	@printf "  %-7s %s\n" "CLEAN" "$(SUBDIR)/main/libc/__start/*.o $(SUBDIR)/main/libc/__start/*.d"
	@rm -f *.o *.d

mrproper: clean

.PHONY: all clean mrproper
