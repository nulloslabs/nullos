ifeq ($(filter --no-print-directory,$(MAKEFLAGS)),)
MAKEFLAGS += --no-print-directory
endif
ifeq ($(filter --silent,$(MAKEFLAGS)),)
MAKEFLAGS += --silent
endif

include config.mk

all: userspace

userspace:
	@$(MAKE) -C main

clean:
	@$(MAKE) -C main clean

mrproper: clean

.PHONY: all userspace clean mrproper