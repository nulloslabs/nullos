ifeq ($(filter --no-print-directory,$(MAKEFLAGS)),)
MAKEFLAGS += --no-print-directory
endif
ifeq ($(filter --silent,$(MAKEFLAGS)),)
MAKEFLAGS += --silent
endif

include ../config.mk

all:
	@$(MAKE) -C crt0
	@$(MAKE) -C libc
	@$(MAKE) -C progs

clean:
	@$(MAKE) -C crt0 clean
	@$(MAKE) -C libc clean
	@$(MAKE) -C progs clean

mrproper: clean

.PHONY: all clean mrproper