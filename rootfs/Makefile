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
	@$(MAKE) -C ../userspace
	@if [ ! -f rootfs.tar.gz ] || [ ! -d main ] || [ Makefile -nt rootfs.tar.gz ] || [ config.mk -nt rootfs.tar.gz ] || $(foreach dep,$(DEP),[ $(dep) -nt rootfs.tar.gz ] ||) false; then \
		# Create folders && \
			mkdir -p main/dev main/proc main/etc main/usr/bin main/usr/sbin main/usr/lib main/usr/include && \
			ln -snf usr/bin main/bin && \
			ln -snf usr/sbin main/sbin && \
			ln -snf usr/lib main/lib && \
			cp ../userspace/init main/init && \
			cp ../userspace/libc.so ../userspace/libc.a ../userspace/crt0.o ../userspace/crt1.o ../userspace/Scrt1.o ../userspace/crti.o ../userspace/crtn.o main/usr/lib && \
			cp ../userspace/ld.so main/usr/lib/ld.so && \
		cp -a ../userspace/include main/usr/ && \
		cp $(filter-out ../userspace/init,$(USERSPACE_PROGS)) main/usr/bin && \
		# Create config files && \
		printf "\\\\s (\\\\r)\\n" > main/etc/issue && \
		printf "root:x:0:0:root:/root:/bin/sh\nuser:x:1000:1000:user:/home/user:/bin/sh\n" > main/etc/passwd && \
		printf "root:x:0:\nuser:x:1000:\n" > main/etc/group && \
		printf 'root:$$5$$nullos$$c6653994d36aafe046b7f4dd23e3ff371afc171c8ebcab43c37d65711e2339b7:19863:0:99999:7:::\n' > main/etc/shadow && \
		printf 'user:$$5$$user$$7126fcc656a61c971522f6f3f949e483c654490630046a1a8963604e91f74ba0:19863:0:99999:7:::\n' >> main/etc/shadow && \
		chmod 644 main/etc/passwd main/etc/group && \
		chmod 600 main/etc/shadow && \
		printf "Welcome to NullOS!\\n\\nAn OS that's fast, secure and private.\\n" > main/etc/motd && \
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
