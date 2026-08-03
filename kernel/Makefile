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

limine_header: config.mk Makefile
	@if [ ! -f include/limine/limine.h ] || [ "config.mk" -nt "include/limine/limine.h" ] || [ "Makefile" -nt "include/limine/limine.h" ]; then \
		printf "  %-7s %s\n" "FETCH" "$(SUBDIR)/include/limine/limine.h" && \
		mkdir -p include/limine && \
		curl -sLo include/limine/limine.h https://raw.githubusercontent.com/limine-bootloader/limine-protocol/refs/heads/trunk/include/limine.h; \
	else \
		printf "  %-7s %s\n" "INFO" "$(SUBDIR)/include/limine/limine.h is already up to date"; \
	fi

uacpi: config.mk Makefile Makefile.uACPI config.mk.uACPI
	@if [ ! -e "uacpi/.git" ] || [ ! -f "uacpi/build/libuacpi.a" ] || [ "config.mk" -nt "uacpi/build/libuacpi.a" ] || [ "Makefile" -nt "uacpi/build/libuacpi.a" ] || [ "config.mk.uACPI" -nt "uacpi/build/libuacpi.a" ] || [ "Makefile.uACPI" -nt "uacpi/build/libuacpi.a" ]; then \
		printf "  %-7s %s\n" "FETCH" "$(SUBDIR)/uacpi/" && \
		git submodule update --init --depth 1 -- uacpi; \
	else \
		printf "  %-7s %s\n" "INFO" "$(SUBDIR)/uacpi/ is already up to date"; \
	fi
	@$(MAKE) -f Makefile.uACPI

$(SUBDIRS): config.mk Makefile limine_header
	@$(MAKE) -C $@

kernel: uacpi linker.ld config.mk Makefile $(SUBDIRS)
	@needs_link=0; \
	for obj in $(OBJ); do \
		if [ ! -f "$(OUTFILE)" ] || [ "uacpi/build/libuacpi.a" -nt "$(OUTFILE)" ] || [ "config.mk" -nt "$(OUTFILE)" ] || [ "linker.ld" -nt "$(OUTFILE)" ] || [ "Makefile" -nt "$(OUTFILE)" ] || [ "$$obj" -nt "$(OUTFILE)" ]; then \
			needs_link=1 && \
			break; \
		fi; \
	done; \
	if [ $$needs_link -eq 1 ]; then \
		printf "  %-7s %s\n" "LD" "$(OUTFILE)" && \
		$(LD) $(LDFLAGS) -o $(OUTFILE) $(OBJ) $(LIBS) && \
		if [ "$(DEBUG)" -eq 0 ]; then \
			printf "  %-7s %s\n" "STRIP" "$(OUTFILE)" && \
			$(STRIP) $(STRIPFLAGS) $(OUTFILE); \
		fi; \
	else \
		printf "  %-7s %s\n" "INFO" "$(SUBDIR)/$(OUTFILE) is already up to date"; \
	fi

clean:
	@printf "  %-7s %s\n" "CLEAN" "$(SUBDIR)/$(OUTFILE)"
	@rm -f $(OUTFILE)
	@make -C main clean
	@make -C io clean
	@make -C mm clean
	@make -C syscalls clean
	@if [ -d uacpi ]; then $(MAKE) -f Makefile.uACPI clean; fi

mrproper: clean
	@printf "  %-7s %s\n" "CLEAN" "include/limine/"
	@rm -rf include/limine

.PHONY: all limine_header uacpi $(SUBDIRS) kernel clean mrproper
