ifeq ($(filter --no-print-directory,$(MAKEFLAGS)),)
MAKEFLAGS += --no-print-directory
endif
ifeq ($(filter --silent,$(MAKEFLAGS)),)
MAKEFLAGS += --silent
endif

include config.mk

.DEFAULT_GOAL := all
DEP := $(wildcard ../userspace/main/progs/*.c) $(shell find main/ -type f 2>/dev/null) config.mk ../userspace/libc.so ../userspace/libc.a ../userspace/crt0.o

all: rootfs

rootfs:
	@$(MAKE) -C ../userspace
	@if [ ! -f rootfs.tar.gz ] || $(foreach dep,$(DEP),[ $(dep) -nt rootfs.tar.gz ] ||) false; then \
		printf "  %-7s %s\n" "TAR" "$(SUBDIR)/rootfs.tar.gz" && \
		mkdir -p main/dev main/etc main/usr/bin main/usr/sbin main/usr/lib main/usr/include && \
		ln -snf usr/bin main/bin && \
		ln -snf usr/sbin main/sbin && \
		ln -snf usr/lib main/lib && \
		cp ../userspace/libc.so ../userspace/libc.a ../userspace/crt0.o main/usr/lib && \
		cp -a ../userspace/include/ main/usr && \
		cp ../userspace/init main && \
		$(foreach prog,$(patsubst ../userspace/main/progs/%.c,%,$(wildcard ../userspace/main/progs/*.c)),cp ../userspace/$(prog) main/usr/bin &&) true && \
		$(TAR) $(TARFLAGS) -f rootfs.tar.gz -C main .; \
	else \
		printf "  %-7s %s\n" "INFO" "$(SUBDIR)/rootfs.tar.gz is already up to date"; \
	fi

clean:
	@printf "  %-7s %s\n" "CLEAN" "$(SUBDIR)/rootfs.tar.gz"
	@rm -rf rootfs.tar.gz

mrproper: clean
	@printf "  %-7s %s\n" "CLEAN" "$(SUBDIR)/main/"
	@rm -rf main

.PHONY: all rootfs clean mrproper
