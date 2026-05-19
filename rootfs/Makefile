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
	@if [ ! -f rootfs.tar.gz ] || [ Makefile -nt rootfs.tar.gz ] || [ config.mk -nt rootfs.tar.gz ] || $(foreach dep,$(DEP),[ $(dep) -nt rootfs.tar.gz ] ||) false; then \
		printf "  %-7s %s\n" "TAR" "$(SUBDIR)/rootfs.tar.gz" && \
		# Create folders && \
		mkdir -p main/dev main/etc main/usr/bin main/usr/sbin main/usr/lib main/usr/include && \
		ln -snf usr/bin main/bin && \
		ln -snf usr/sbin main/sbin && \
		ln -snf usr/lib main/lib && \
		cp ../userspace/libc.so ../userspace/libc.a ../userspace/crt0.o main/usr/lib && \
		cp -a ../userspace/include/ main/usr && \
		cp ../userspace/init main && \
		$(foreach prog,$(filter-out init,$(patsubst ../userspace/main/progs/%.c,%,$(wildcard ../userspace/main/progs/*.c))),cp ../userspace/$(prog) main/usr/bin &&) \
		# Create config files && \
		printf "\\\\s (\\\\r)\\n" > main/etc/issue && \
		printf "root::0:0:root:/root:/bin/sh" > main/etc/passwd && \
		printf "root:4813494d137e1631bba301d5acab6e7bb7aa74ce1185d456565ef51d737677b2" > main/etc/shadow && \
		printf "Welcome to NullOS!\\n\\nAn OS that respects your privacy, is fast, lightweight and secure.\\n" > main/etc/motd && \
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
