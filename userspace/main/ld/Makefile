ifeq ($(filter --no-print-directory,$(MAKEFLAGS)),)
MAKEFLAGS += --no-print-directory
endif
ifeq ($(filter --silent,$(MAKEFLAGS)),)
MAKEFLAGS += --silent
endif

.DEFAULT_GOAL := all

include ../../config.mk

# Find the sources down in main/ to check up-to-date status
SRC = $(wildcard main/*.c) $(wildcard main/*.S)
OBJ = $(SRC:.c=.o)
OBJ := $(OBJ:.S=.o)

# Extract just the file names (e.g., ld.o) for up-to-date reporting
FILES = $(notdir $(OBJ))

all: ld

ld: Makefile ../../config.mk
	@$(MAKE) -C main
	@needs_build=0; \
	if [ ! -f ../../ld.so ]; then \
		needs_build=1; \
	else \
		for obj in $(OBJ); do \
			if [ $$obj -nt ../../ld.so ] || [ Makefile -nt ../../ld.so ] || [ ../../config.mk -nt ../../ld.so ]; then \
				needs_build=1; \
				break; \
			fi; \
		done; \
	fi; \
	if [ $$needs_build -eq 0 ]; then \
		printf "  %-7s %s\n" "INFO" "$(SUBDIR)/ld.so is already up to date"; \
	else \
		printf "  %-7s %s\n" "LD" "$(SUBDIR)/ld.so" && \
		$(LD) $(LDFLAGS) -static -o ../../ld.so $(OBJ) && \
		printf "  %-7s %s\n" "STRIP" "$(SUBDIR)/ld.so" && \
		$(STRIP) ../../ld.so; \
	fi

clean:
	@$(MAKE) -C main clean

mrproper: clean

.PHONY: all ld_subdirs clean mrproper
