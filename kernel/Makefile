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

UACPI_SRC = $(wildcard uacpi/source/*.c)
UACPI_OBJ = $(patsubst uacpi/source/%.c,$(UACPI_BUILD)/%.o,$(UACPI_SRC))
UACPI_DEP = $(UACPI_OBJ:.o=.d)

SUBDIRS = main io mm syscalls

-include $(UACPI_DEP)

.DEFAULT_GOAL := all

all: kernel

limine_header: config.mk Makefile
	@if [ ! -f include/limine/limine.h ]; then \
		printf "  %-7s %s\n" "FETCH" "$(SUBDIR)/include/limine/limine.h" && \
		mkdir -p include/limine && \
		curl -sLo include/limine/limine.h https://raw.githubusercontent.com/limine-bootloader/limine-protocol/refs/heads/trunk/include/limine.h; \
	else \
		printf "  %-7s %s\n" "INFO" "$(SUBDIR)/include/limine/limine.h is already up to date"; \
	fi

uacpi: config.mk Makefile
	@if [ ! -e "uacpi/.git" ]; then \
		printf "  %-7s %s\n" "FETCH" "$(SUBDIR)/uacpi/" && \
		git submodule update --init --depth 1 -- uacpi > /dev/null 2>&1; \
	else \
		printf "  %-7s %s\n" "INFO" "$(SUBDIR)/uacpi/ is already up to date"; \
	fi

$(UACPI_OUTFILE): $(UACPI_OBJ) FORCE
	@needs_archive=0; \
	has_debug=0; \
	if [ -f "$(UACPI_OUTFILE)" ] && $(OBJDUMP) -h "$(UACPI_OUTFILE)" 2>/dev/null | grep -q '\.debug'; then \
		has_debug=1; \
	fi; \
	if [ ! -f "$(UACPI_OUTFILE)" ] || [ "$(DEBUG)" != "$$has_debug" ]; then \
		needs_archive=1; \
	else \
		for obj in $(UACPI_OBJ); do \
			if [ "config.mk" -nt "$(UACPI_OUTFILE)" ] || [ "Makefile" -nt "$(UACPI_OUTFILE)" ] || [ "$$obj" -nt "$(UACPI_OUTFILE)" ]; then \
				needs_archive=1; \
				break; \
			fi; \
		done; \
	fi; \
	if [ $$needs_archive -eq 1 ]; then \
		printf "  %-7s %s\n" "AR" "$(SUBDIR)/$(UACPI_OUTFILE)" && \
		$(AR) rcs $(UACPI_OUTFILE) $(UACPI_OBJ) && \
		if [ "$(DEBUG)" -eq 0 ]; then \
			printf "  %-7s %s\n" "STRIP" "$(SUBDIR)/$(UACPI_OUTFILE)" && \
			$(STRIP) $(UACPI_STRIPFLAGS) $(UACPI_OUTFILE); \
		fi; \
	else \
		printf "  %-7s %s\n" "INFO" "$(SUBDIR)/$(UACPI_OUTFILE) is already up to date"; \
	fi

$(UACPI_BUILD)/%.o: uacpi/source/%.c config.mk Makefile $(FORCE_REBUILD)
	@mkdir -p $(UACPI_BUILD)
	@printf "  %-7s %s\n" "CC" "$(SUBDIR)/$@"
	@$(CC) $(UACPI_CFLAGS) -c $< -o $@

$(SUBDIRS): config.mk Makefile
	@$(MAKE) -C $@

kernel: config.mk Makefile linker.ld
	@$(MAKE) limine_header
	@$(MAKE) uacpi
	@$(MAKE) $(SUBDIRS)
	@$(MAKE) $(UACPI_OUTFILE)
	@needs_link=0; \
	has_debug=0; \
	if [ -f "$(OUTFILE)" ] && $(OBJDUMP) -h "$(OUTFILE)" 2>/dev/null | grep -q '\.debug'; then \
		has_debug=1; \
	fi; \
	if [ ! -f "$(OUTFILE)" ] || [ "$(DEBUG)" != "$$has_debug" ]; then \
		needs_link=1; \
	else \
		for obj in $(OBJ); do \
			if [ "$(UACPI_OUTFILE)" -nt "$(OUTFILE)" ] || [ "config.mk" -nt "$(OUTFILE)" ] || [ "linker.ld" -nt "$(OUTFILE)" ] || [ "Makefile" -nt "$(OUTFILE)" ] || [ "$$obj" -nt "$(OUTFILE)" ]; then \
				needs_link=1 && \
				break; \
			fi; \
		done; \
	fi; \
	if [ $$needs_link -eq 1 ]; then \
		printf "  %-7s %s\n" "LD" "$(SUBDIR)/$(OUTFILE)" && \
		$(LD) $(LDFLAGS) -o $(OUTFILE) $(OBJ) $(LIBS) && \
		if [ "$(DEBUG)" -eq 0 ]; then \
			printf "  %-7s %s\n" "STRIP" "$(SUBDIR)/$(OUTFILE)" && \
			$(STRIP) $(STRIPFLAGS) $(OUTFILE); \
		fi; \
	else \
		printf "  %-7s %s\n" "INFO" "$(SUBDIR)/$(OUTFILE) is already up to date"; \
	fi

FORCE:

clean:
	@printf "  %-7s %s\n" "CLEAN" "$(SUBDIR)/$(OUTFILE)"
	@rm -f $(OUTFILE)
	@$(MAKE) -C main clean
	@$(MAKE) -C io clean
	@$(MAKE) -C mm clean
	@$(MAKE) -C syscalls clean
	@printf "  %-7s %s\n" "CLEAN" "$(SUBDIR)/$(UACPI_BUILD)/"
	@rm -rf $(UACPI_BUILD)

mrproper: clean
	@printf "  %-7s %s\n" "CLEAN" "include/limine/"
	@rm -rf include/limine

.PHONY: all limine_header uacpi $(SUBDIRS) kernel FORCE clean mrproper
