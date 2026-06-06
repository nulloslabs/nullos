ifeq ($(filter --no-print-directory,$(MAKEFLAGS)),)
MAKEFLAGS += --no-print-directory
endif
ifeq ($(filter --silent,$(MAKEFLAGS)),)
MAKEFLAGS += --silent
endif

.DEFAULT_GOAL := all

include ../../../config.mk

SRC = $(wildcard *.c *.S)
OBJ = $(patsubst %.c,../../../%.o,$(filter %.c,$(SRC))) $(patsubst %.S,../../../%.o,$(filter %.S,$(SRC)))
DEP = $(filter %.c,$(SRC):.c=.d)

-include $(DEP)

.PHONY: all clean

all: $(OBJ)

../../../%.o: %.c
	@printf "  %-7s %s\n" "CC" "userspace/$(@F)"
	@$(CC) $(CFLAGS) -MF $*.d -c $< -o $@

../../../%.o: %.S
	@printf "  %-7s %s\n" "AS" "userspace/$(@F)"
	@$(AS) $(AFLAGS) -MF $*.d -c $< -o $@

clean:
	@rm -f ../../../crt*.o *.d
