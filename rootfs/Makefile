ifeq ($(filter --no-print-directory,$(MAKEFLAGS)),)
MAKEFLAGS += --no-print-directory
endif
ifeq ($(filter --silent,$(MAKEFLAGS)),)
MAKEFLAGS += --silent
endif

include config.mk

.DEFAULT_GOAL := all
USERSPACE_PROGS := $(patsubst ../userspace/main/progs/main/%.c,../userspace/%,$(wildcard ../userspace/main/progs/main/*.c)) $(patsubst ../userspace/main/progs/main/%.S,../userspace/%,$(wildcard ../userspace/main/progs/main/*.S))
DEP := $(wildcard ../userspace/main/progs/*.c) $(wildcard ../userspace/main/progs/*.S) $(USERSPACE_PROGS) $(shell find main/ 2>/dev/null) config.mk ../userspace/libc.so ../userspace/libc.a ../userspace/crt0.o ../userspace/crt1.o ../userspace/Scrt1.o ../userspace/crti.o ../userspace/crtn.o ../userspace/ld.so

all: rootfs

rootfs:
	@if [ ! -f rootfs.tar.gz ] || [ ! -d main ] || [ Makefile -nt rootfs.tar.gz ] || [ config.mk -nt rootfs.tar.gz ] || $(foreach dep,$(DEP),[ $(dep) -nt rootfs.tar.gz ] ||) false; then \
		mkdir -p main/home main/root main/dev main/proc main/etc main/usr && \
		mkdir -p main/home/user main/etc/init.d main/usr/bin main/usr/sbin main/usr/lib main/usr/include && \
		ln -snf usr/bin main/bin && \
		ln -snf usr/sbin main/sbin && \
		ln -snf usr/lib main/lib && \
		printf '%s\n' \
			'::sysinit:/etc/init.d/rcS' \
			'tty0::respawn:-/sbin/getty -L tty0 115200 vt100' \
			'::ctrlaltdel:/sbin/reboot' \
			'::shutdown:/bin/umount -a -r' \
			> main/etc/inittab && \
		printf '%s\n' \
			'#!/bin/sh' \
			'mount -t devtmpfs devtmpfs /dev' \
			'mkdir -p /dev/pts' \
			'mount -t devpts devpts /dev/pts' \
			'mount -t proc proc /proc' \
			> main/etc/init.d/rcS && \
		chmod +x main/etc/init.d/rcS && \
		printf "\\\\s (\\\\r)\n\n" > main/etc/issue && \
		printf "root:x:0:0:root:/root:/bin/sh\nuser:x:1000:1000:user:/home/user:/bin/sh\n" > main/etc/passwd && \
		printf "root:x:0:\nuser:x:1000:\n" > main/etc/group && \
		printf 'root:$$5$$nullos$$bdWJDaAMKrT0iGP6LnRmZJf5O8ImIPr0ChFp1JF2VID:19863:0:99999:7:::\n' > main/etc/shadow && \
		printf 'user:$$5$$user$$Lssaar/36bdTMOfh5iRLlvKBQ/QgRHkvu/nltxEXH/9:19863:0:99999:7:::\n' >> main/etc/shadow && \
		chmod 644 main/etc/passwd main/etc/group && \
		chmod 600 main/etc/shadow && \
		printf "\nWelcome to NullOS!\n\nAn OS that's fast, secure and private.\n\n" > main/etc/motd && \
		if [ ! -f main/usr/bin/busybox ]; then \
			printf "  %-7s %s\n" "FETCH" "$(SUBDIR)/main/usr/bin/busybox" && \
			wget https://busybox.net/downloads/binaries/1.35.0-x86_64-linux-musl/busybox -O main/usr/bin/busybox && \
			chmod +x main/usr/bin/busybox; \
		else \
			printf "  %-7s %s\n" "INFO" "$(SUBDIR)/main/usr/bin/busybox is already up to date"; \
		fi; \
		main/usr/bin/busybox --list-full | while read applet; do \
			mkdir -p main/$$(dirname $$applet) && \
			ln -sf /usr/bin/busybox main/$$applet; \
		done; \
		(cd main && ln -sf sbin/init init) && \
		rm -f main/linuxrc && \
		printf "  %-7s %s\n" "TAR" "$(SUBDIR)/rootfs.tar.gz" && \
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
