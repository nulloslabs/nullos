ifeq ($(filter --no-print-directory,$(MAKEFLAGS)),)
MAKEFLAGS += --no-print-directory
endif
ifeq ($(filter --silent,$(MAKEFLAGS)),)
MAKEFLAGS += --silent
endif

include ../config.mk

SRC = $(wildcard *.c) $(wildcard *.S)
OBJ = $(SRC:.c=.o)
OBJ := $(OBJ:.S=.o)
DEP := $(OBJ:.o=.d)

all: $(OBJ)

-include $(DEP)

# kernel.h is special since it holds our kernel version.
GIT_HEAD := $(shell git rev-parse --git-path HEAD)
GIT_REF := $(shell git symbolic-ref -q HEAD)
GIT_REF_FILE := $(if $(GIT_REF),$(shell git rev-parse --git-path $(GIT_REF)))

../include/main/kernel.h: $(GIT_HEAD) $(GIT_REF_FILE)
	@touch $@

undefine GIT_REF_FILE
undefine GIT_REF
undefine GIT_HEAD

%.o: %.c Makefile ../Makefile ../config.mk ../include/limine/limine.h
	@printf "  %-7s %s\n" "CC" "$(SUBDIR)/main/$@"
	@$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S Makefile ../Makefile ../config.mk ../include/limine/limine.h
	@printf "  %-7s %s\n" "AS" "$(SUBDIR)/main/$@"
	@$(AS) $(AFLAGS) -c $< -o $@

clean:
	@printf "  %-7s %s\n" "CLEAN" "$(SUBDIR)/main/*.o $(SUBDIR)/main/*.d"
	@rm -rf *.o *.d

mrproper: clean

.PHONY: all clean mrproper
