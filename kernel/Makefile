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
	@if [ ! -f include/limine/limine.h ] || [ "config.mk" -nt "include/limine/limine.h" ] || [ "Makefile" -nt "include/limine/limine.h" ]; then \
		printf "  %-7s %s\n" "FETCH" "$(SUBDIR)/include/limine/limine.h" && \
		mkdir -p include/limine && \
		curl -sLo include/limine/limine.h https://raw.githubusercontent.com/limine-bootloader/limine-protocol/refs/heads/trunk/include/limine.h; \
	else \
		printf "  %-7s %s\n" "INFO" "$(SUBDIR)/include/limine/limine.h is already up to date"; \
	fi

uacpi: config.mk Makefile
	@if [ ! -e "uacpi/.git" ] || [ ! -f "$(UACPI_OUTFILE)" ] || [ "config.mk" -nt "$(UACPI_OUTFILE)" ] || [ "Makefile" -nt "$(UACPI_OUTFILE)" ]; then \
		printf "  %-7s %s\n" "FETCH" "$(SUBDIR)/uacpi/" && \
		git submodule update --init --depth 1 -- uacpi; \
	else \
		printf "  %-7s %s\n" "INFO" "$(SUBDIR)/uacpi/ is already up to date"; \
	fi
	@$(MAKE) uacpi_build

uacpi_build: $(UACPI_OUTFILE)

$(UACPI_OUTFILE): $(UACPI_OBJ) FORCE
	@needs_archive=0; \
	for obj in $(UACPI_OBJ); do \
		if [ ! -f "$(UACPI_OUTFILE)" ] || [ "config.mk" -nt "$(UACPI_OUTFILE)" ] || [ "Makefile" -nt "$(UACPI_OUTFILE)" ] || [ "$$obj" -nt "$(UACPI_OUTFILE)" ]; then \
			needs_archive=1; \
			break; \
		fi; \
	done; \
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

$(UACPI_BUILD)/%.o: uacpi/source/%.c config.mk Makefile
	@mkdir -p $(UACPI_BUILD)
	@printf "  %-7s %s\n" "CC" "$(SUBDIR)/$@"
	@$(CC) $(UACPI_CFLAGS) -c $< -o $@

$(SUBDIRS): config.mk Makefile limine_header
	@$(MAKE) -C $@

kernel: $(SUBDIRS) config.mk Makefile linker.ld uacpi
	@needs_link=0; \
	for obj in $(OBJ); do \
		if [ ! -f "$(OUTFILE)" ] || [ "$(UACPI_OUTFILE)" -nt "$(OUTFILE)" ] || [ "config.mk" -nt "$(OUTFILE)" ] || [ "linker.ld" -nt "$(OUTFILE)" ] || [ "Makefile" -nt "$(OUTFILE)" ] || [ "$$obj" -nt "$(OUTFILE)" ]; then \
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

.PHONY: all limine_header uacpi uacpi_build $(SUBDIRS) kernel FORCE clean mrproper
