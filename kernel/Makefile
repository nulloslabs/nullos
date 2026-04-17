ifeq ($(filter --no-print-directory,$(MAKEFLAGS)),)
MAKEFLAGS += --no-print-directory
endif
ifeq ($(filter --silent,$(MAKEFLAGS)),)
MAKEFLAGS += --silent
endif

include config.mk

SRC = $(wildcard io/*.c) $(wildcard io/*.S) $(wildcard main/*.c) $(wildcard main/*.S) $(wildcard mm/*.c) $(wildcard mm/*.S) $(wildcard syscalls/*.c) $(wildcard syscalls/*.S)
OBJ = $(SRC:.c=.o)
OBJ := $(OBJ:.S=.o)
DEP := $(OBJ:.o=.d)
SUBDIRS = main io mm syscalls

all: kernel

-include $(DEP)

limine_header:
	@if [ -f include/limine/limine.h ]; then \
		printf "  %-7s %s\n" "INFO" "include/limine/limine.h is already up to date"; \
	else \
		printf "  %-7s %s\n" "FETCH" "$(SUBDIR)/include/limine/limine.h"; \
		mkdir -p include/limine; \
		curl -sLo include/limine/limine.h https://raw.githubusercontent.com/limine-bootloader/limine-protocol/refs/heads/trunk/include/limine.h; \
		perl -pi -e 's/<stdint\.h>/<freestanding\/stdint.h> \/\/ Changed to match NullOS headers/g' include/limine/limine.h; \
		perl -pi -e 's/<stdbool\.h>/<freestanding\/stdbool.h> \/\/ Changed to match NullOS headers/g' include/limine/limine.h; \
		perl -pi -e 's/<stddef\.h>/<freestanding\/stddef.h> \/\/ Changed to match NullOS headers/g' include/limine/limine.h; \
	fi

$(SUBDIRS): config.mk Makefile limine_header
	@$(MAKE) -C $@

kernel: config.mk linker.ld Makefile $(SUBDIRS)
	@needs_link=0; \
	for obj in $(OBJ); do \
		if [ ! -f "$(KERNELFILE)" ] || [ "config.mk" -nt "$(KERNELFILE)" ] || [ "linker.ld" -nt "$(KERNELFILE)" ] || [ "Makefile" -nt "$(KERNELFILE)" ] || [ "$$obj" -nt "$(KERNELFILE)" ]; then \
			needs_link=1; break; \
		fi; \
	done; \
	if [ $$needs_link -eq 1 ]; then \
		printf "  %-7s %s\n" "LD" "$(KERNELFILE).unstripped"; \
		$(LD) $(LDFLAGS) -o $(KERNELFILE).unstripped $(OBJ); \
		printf "  %-7s %s\n" "STRIP" "$(KERNELFILE)"; \
		$(STRIP) $(STRIPFLAGS) $(KERNELFILE).unstripped -o $(KERNELFILE); \
	else \
		printf "  %-7s %s\n" "INFO" "$(SUBDIR)/$(KERNELFILE) is already up to date"; \
	fi

clean:
	@printf "  %-7s %s\n" "CLEAN" "$(SUBDIR)/$(KERNELFILE) $(SUBDIR)/$(KERNELFILE).unstripped"
	@rm -f $(KERNELFILE) $(KERNELFILE).unstripped
	@make -C main clean
	@make -C io clean
	@make -C mm clean
	@make -C syscalls clean

mrproper: clean
	@printf "  %-7s %s\n" "CLEAN" "include/limine/"
	@rm -rf include/limine

.PHONY: all kernel limine_header $(SUBDIRS) clean mrproper