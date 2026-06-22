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

../../../%.o: %.c Makefile ../../../config.mk
	@printf "  %-7s %s\n" "CC" "userspace/$(@F)"
	@$(CC) $(CFLAGS) -fno-stack-protector -MF $*.d -c $< -o $@

../../../%.o: %.S Makefile ../../../config.mk
	@printf "  %-7s %s\n" "AS" "userspace/$(@F)"
	@$(AS) $(AFLAGS) -fno-stack-protector -MF $*.d -c $< -o $@
ifeq ($(DEBUG),0)
	@printf "  %-7s %s\n" "STRIP" "userspace/$(@F)"
	@$(STRIP) $(STRIPFLAGS) --strip-debug $@
endif

clean:
	@rm -f ../../../crt*.o ../../../Scrt*.o *.d
