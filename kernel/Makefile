ifeq ($(filter --no-print-directory,$(MAKEFLAGS)),)
MAKEFLAGS += --no-print-directory
endif
ifeq ($(filter --silent,$(MAKEFLAGS)),)
MAKEFLAGS += --silent
endif

include config.mk

SRC := $(wildcard io/*.c) $(wildcard io/*.S) $(wildcard main/*.c) $(wildcard main/*.S) $(wildcard mm/*.c) $(wildcard mm/*.S) $(wildcard syscalls/*.c) $(wildcard syscalls/*.S)
UACPI_SRC = $(wildcard uacpi/source/*.c)

include Kconfig.mk
-include include/generated/auto.conf

OBJ := $(SRC:.c=.o)
OBJ := $(OBJ:.S=.o)
UACPI_OBJ = $(patsubst uacpi/source/%.c,uacpi/build/%.o,$(UACPI_SRC))

DEP := $(OBJ:.o=.d)
UACPI_DEP = $(UACPI_OBJ:.o=.d)

SUBDIRS = main io mm syscalls

-include $(DEP)
-include $(UACPI_DEP)

.DEFAULT_GOAL := all

all: kernel

menuconfig:
	@$(MAKE) -C ../scripts/kconfig
	@oldsum=$$(md5sum .config 2>/dev/null | cut -d' ' -f1); \
	KCONFIG_CONFIG=.config ../scripts/kconfig/main/mconf Kconfig; \
	ret=$$?; \
	newsum=$$(md5sum .config 2>/dev/null | cut -d' ' -f1); \
	if [ $$ret -ne 0 ]; then exit $$ret; fi; \
	if [ -f .config ] && { [ "$$oldsum" != "$$newsum" ] || [ ! -f include/generated/autoconf.h ]; }; then \
		rm -rf include/generated && mkdir -p include/generated && \
		KCONFIG_CONFIG=.config KCONFIG_AUTOHEADER=include/generated/autoconf.h KCONFIG_AUTOCONFIG=include/generated/auto.conf KCONFIG_TRISTATE=include/generated/tristate.conf ../scripts/kconfig/main/conf --silentoldconfig Kconfig && \
		find include/generated -mindepth 1 ! -name 'autoconf.h' ! -name 'auto.conf' -delete; \
	fi

allyesconfig:
	@$(MAKE) -C ../scripts/kconfig
	@KCONFIG_CONFIG=.config ../scripts/kconfig/main/conf --allyesconfig Kconfig && \
	if [ -f .config ]; then \
		rm -rf include/generated && mkdir -p include/generated && \
		KCONFIG_CONFIG=.config KCONFIG_AUTOHEADER=include/generated/autoconf.h KCONFIG_AUTOCONFIG=include/generated/auto.conf KCONFIG_TRISTATE=include/generated/tristate.conf ../scripts/kconfig/main/conf --silentoldconfig Kconfig && \
		find include/generated -mindepth 1 ! -name 'autoconf.h' ! -name 'auto.conf' -delete; \
	fi

allnoconfig:
	@$(MAKE) -C ../scripts/kconfig
	@KCONFIG_CONFIG=.config ../scripts/kconfig/main/conf --allnoconfig Kconfig && \
	if [ -f .config ]; then \
		rm -rf include/generated && mkdir -p include/generated && \
		KCONFIG_CONFIG=.config KCONFIG_AUTOHEADER=include/generated/autoconf.h KCONFIG_AUTOCONFIG=include/generated/auto.conf KCONFIG_TRISTATE=include/generated/tristate.conf ../scripts/kconfig/main/conf --silentoldconfig Kconfig && \
		find include/generated -mindepth 1 ! -name 'autoconf.h' ! -name 'auto.conf' -delete; \
	fi

alldefconfig:
	@$(MAKE) -C ../scripts/kconfig
	@KCONFIG_CONFIG=.config ../scripts/kconfig/main/conf --alldefconfig Kconfig && \
	if [ -f .config ]; then \
		rm -rf include/generated && mkdir -p include/generated && \
		KCONFIG_CONFIG=.config KCONFIG_AUTOHEADER=include/generated/autoconf.h KCONFIG_AUTOCONFIG=include/generated/auto.conf KCONFIG_TRISTATE=include/generated/tristate.conf ../scripts/kconfig/main/conf --silentoldconfig Kconfig && \
		find include/generated -mindepth 1 ! -name 'autoconf.h' ! -name 'auto.conf' -delete; \
	fi

defconfig: alldefconfig

oldconfig:
	@$(MAKE) -C ../scripts/kconfig
	@KCONFIG_CONFIG=.config ../scripts/kconfig/main/conf --oldconfig Kconfig && \
	if [ -f .config ]; then \
		rm -rf include/generated && mkdir -p include/generated && \
		KCONFIG_CONFIG=.config KCONFIG_AUTOHEADER=include/generated/autoconf.h KCONFIG_AUTOCONFIG=include/generated/auto.conf KCONFIG_TRISTATE=include/generated/tristate.conf ../scripts/kconfig/main/conf --silentoldconfig Kconfig && \
		find include/generated -mindepth 1 ! -name 'autoconf.h' ! -name 'auto.conf' -delete; \
	fi

olddefconfig:
	@$(MAKE) -C ../scripts/kconfig
	@KCONFIG_CONFIG=.config ../scripts/kconfig/main/conf --olddefconfig Kconfig && \
	if [ -f .config ]; then \
		rm -rf include/generated && mkdir -p include/generated && \
		KCONFIG_CONFIG=.config KCONFIG_AUTOHEADER=include/generated/autoconf.h KCONFIG_AUTOCONFIG=include/generated/auto.conf KCONFIG_TRISTATE=include/generated/tristate.conf ../scripts/kconfig/main/conf --silentoldconfig Kconfig && \
		find include/generated -mindepth 1 ! -name 'autoconf.h' ! -name 'auto.conf' -delete; \
	fi

savedefconfig:
	@$(MAKE) -C ../scripts/kconfig
	@../scripts/kconfig/main/conf --savedefconfig=defconfig Kconfig

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
	has_debug="n"; \
	if [ -f "$(UACPI_OUTFILE)" ] && $(OBJDUMP) -h "$(UACPI_OUTFILE)" 2>/dev/null | grep -q '\.debug'; then \
		has_debug="y"; \
	fi; \
	has_debug_cfg="n"; if [ "$(CONFIG_DEBUG)" = "y" ]; then has_debug_cfg="y"; fi; \
	if [ ! -f "$(UACPI_OUTFILE)" ] || [ "$$has_debug_cfg" != "$$has_debug" ]; then \
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
		if [ "$(CONFIG_DEBUG)" != "y" ]; then \
			printf "  %-7s %s\n" "STRIP" "$(SUBDIR)/$(UACPI_OUTFILE)" && \
			$(STRIP) $(UACPI_STRIPFLAGS) $(UACPI_OUTFILE); \
		fi; \
	else \
		printf "  %-7s %s\n" "INFO" "$(SUBDIR)/$(UACPI_OUTFILE) is already up to date"; \
	fi

uacpi/build/%.o: uacpi/source/%.c config.mk Makefile $(UACPI_FORCE_REBUILD)
	@mkdir -p uacpi/build
	@printf "  %-7s %s\n" "CC" "$(SUBDIR)/$@"
	@$(CC) $(UACPI_CFLAGS) -c $< -o $@

$(SUBDIRS): config.mk Makefile
	@$(MAKE) -C $@

kernel: config.mk Makefile linker.ld
	@if [ ! -f .config ]; then \
		$(MAKE) defconfig > /dev/null 2>&1 && \
		$(MAKE) kernel; \
		exit $$?; \
	fi
	@$(MAKE) limine_header
	@$(MAKE) uacpi
	@$(MAKE) $(SUBDIRS)
	@$(MAKE) $(UACPI_OUTFILE)
	@needs_link=0; \
	has_debug=0; \
	if [ -f "$(OUTFILE)" ] && $(OBJDUMP) -h "$(OUTFILE)" 2>/dev/null | grep -q '\.debug'; then \
		has_debug=1; \
	fi; \
	has_debug_cfg=0; if [ "$(CONFIG_DEBUG)" = "y" ]; then has_debug_cfg=1; fi; \
	if [ ! -f "$(OUTFILE)" ] || [ "$$has_debug_cfg" != "$$has_debug" ]; then \
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
		if [ "$(CONFIG_DEBUG)" != "y" ]; then \
			printf "  %-7s %s\n" "STRIP" "$(SUBDIR)/$(OUTFILE)" && \
			$(STRIP) $(STRIPFLAGS) $(OUTFILE); \
		fi; \
	else \
		printf "  %-7s %s\n" "INFO" "$(SUBDIR)/$(OUTFILE) is already up to date"; \
	fi

clean:
	@printf "  %-7s %s\n" "CLEAN" "$(SUBDIR)/$(OUTFILE)"
	@rm -f $(OUTFILE)
	@$(MAKE) -C main clean
	@$(MAKE) -C io clean
	@$(MAKE) -C mm clean
	@$(MAKE) -C syscalls clean
	@printf "  %-7s %s\n" "CLEAN" "$(SUBDIR)/uacpi/build/"
	@rm -rf uacpi/build

mrproper: clean
	@printf "  %-7s %s\n" "CLEAN" "$(SUBDIR)/include/generated/ $(SUBDIR)/.config $(SUBDIR)/.config.old $(SUBDIR)/.config.cmd"
	@rm -rf include/generated .config .config.old .config.cmd
	@printf "  %-7s %s\n" "CLEAN" "include/limine/"
	@rm -rf include/limine

FORCE:

.PHONY: all menuconfig allyesconfig allnoconfig alldefconfig defconfig oldconfig olddefconfig savedefconfig limine_header uacpi $(SUBDIRS) kernel clean mrproper FORCE
