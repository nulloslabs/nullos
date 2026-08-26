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

-include $(DEP)

GIT_HEAD := $(shell git rev-parse --git-path HEAD)
GIT_REF := $(shell git symbolic-ref -q HEAD)
GIT_REF_FILE := $(if $(GIT_REF),$(shell git rev-parse --git-path $(GIT_REF)))
GIT_PACKED := $(shell git rev-parse --git-path packed-refs)

../include/main/kernel.h: $(GIT_HEAD) $(GIT_REF_FILE) $(GIT_PACKED)
	@touch $@

undefine GIT_PACKED
undefine GIT_REF_FILE
undefine GIT_REF
undefine GIT_HEAD

%.o: %.c Makefile ../Makefile ../config.mk $(FORCE_REBUILD) ../include/limine/limine.h
	@printf "  %-7s %s\n" "CC" "$(SUBDIR)/main/$@"
	@$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S Makefile ../Makefile ../config.mk $(FORCE_REBUILD) ../include/limine/limine.h
	@printf "  %-7s %s\n" "AS" "$(SUBDIR)/main/$@"
	@$(AS) $(AFLAGS) -c $< -o $@

clean:
	@printf "  %-7s %s\n" "CLEAN" "$(SUBDIR)/main/*.o $(SUBDIR)/main/*.d"
	@rm -rf *.o *.d

mrproper: clean

.PHONY: all clean mrproper
