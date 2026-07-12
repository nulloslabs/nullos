ifeq ($(filter --no-print-directory,$(MAKEFLAGS)),)
MAKEFLAGS += --no-print-directory
endif
ifeq ($(filter --silent,$(MAKEFLAGS)),)
MAKEFLAGS += --silent
endif

include config.mk

.DEFAULT_GOAL := all

all: initrd

initrd:
	@current_names="$$(for p in $(PACKAGE_NAMES); do basename $$p; done)"; \
	cached_names="$$(ls packages/*.pkg.tar.zst 2>/dev/null | xargs -n1 basename 2>/dev/null | sed 's/\.pkg\.tar\.zst$$//')"; \
	missing="$$(for n in $$current_names; do echo "$$cached_names" | grep -qxF "$$n" || echo missing_$$n; done)"; \
	extra="$$(for n in $$cached_names; do echo "$$current_names" | grep -qxF "$$n" || echo extra_$$n; done)"; \
	if [ -n "$$extra" ]; then \
		for n in $$cached_names; do \
			echo "$$current_names" | grep -qxF "$$n" || rm -f packages/$$n.pkg.tar.zst; \
		done; \
	fi; \
	if [ ! -f $(OUTFILE) ] || [ ! -d main ] || [ Makefile -nt $(OUTFILE) ] || [ config.mk -nt $(OUTFILE) ] || [ -n "$$missing" ] || [ -n "$$extra" ] || [ -n "$$(find main -newer $(OUTFILE) 2>/dev/null | head -1)" ]; then \
		rm -rf main && \
		mkdir -p main packages && \
		mkdir -p main/home main/root main/dev main/proc main/etc main/var main/usr main/tmp main/run && \
		mkdir -p main/home/user main/etc/init.d main/usr/bin main/usr/sbin main/usr/lib main/usr/include && \
		mkdir -p main/var/db main/var/cache main/var/log main/var/lib main/var/tmp && \
		chmod 1777 main/tmp main/var/tmp && \
		chmod 755 main/run && \
		ln -snf ../run main/var/run && \
		ln -snf usr/bin main/bin && \
		ln -snf usr/sbin main/sbin && \
		ln -snf usr/lib main/lib && \
		ln -snf lib main/usr/lib64 && \
		ln -snf usr/lib64 main/lib64 && \
		printf '%s\n' \
			'::sysinit:/etc/init.d/rcS' \
			'tty0::respawn:/sbin/getty -L tty0 115200 vt100' \
			'::ctrlaltdel:/sbin/reboot' \
			'::shutdown:/bin/umount -a -r' \
			> main/etc/inittab && \
		printf '%s\n' \
			'#!/bin/sh' \
			'mount -t devtmpfs devtmpfs /dev' \
			'mkdir -p /dev/pts' \
			'mount -t devpts devpts /dev/pts' \
			'mount -t proc proc /proc' \
			'mount -t tmpfs tmpfs /tmp' \
			'mount -t tmpfs run /run' \
			> main/etc/init.d/rcS && \
		chmod +x main/etc/init.d/rcS && \
		printf '%s\n' \
			'export PS1="\\w \\$$ "' \
			'export PATH=/usr/sbin:/usr/bin:/sbin:/bin' \
			> main/etc/profile && \
		printf "NullOS (\\\\r)\n\n" > main/etc/issue && \
		printf "root:x:0:0:root:/root:/bin/bash\nuser:x:1000:1000:user:/home/user:/bin/bash\n" > main/etc/passwd && \
		printf "root:x:0:\nuser:x:1000:\n" > main/etc/group && \
		printf 'root:$$5$$nullos$$bdWJDaAMKrT0iGP6LnRmZJf5O8ImIPr0ChFp1JF2VID:19863:0:99999:7:::\n' > main/etc/shadow && \
		printf 'user:$$5$$user$$Lssaar/36bdTMOfh5iRLlvKBQ/QgRHkvu/nltxEXH/9:19863:0:99999:7:::\n' >> main/etc/shadow && \
		chmod 644 main/etc/passwd main/etc/group && \
		chmod 600 main/etc/shadow && \
		printf "\nWelcome to NullOS!\n\nAn OS that's fast, secure and private.\n\n" > main/etc/motd && \
		for p in $(PACKAGE_NAMES); do \
			repo=$$(dirname $$p) && \
			name=$$(basename $$p) && \
			[ -f packages/$$name.pkg.tar.zst ] || \
			(curl -fsL -o packages/$$name.pkg.tar.zst https://archlinux.org/packages/$$repo/x86_64/$$name/download/ || \
			 curl -fsL -o packages/$$name.pkg.tar.zst https://archlinux.org/packages/$$repo/any/$$name/download/) && \
			if [ "$(KERNEL)" = "Darwin" ]; then \
				docker run --rm -v "$(PWD)":"$(PWD)" -w "$(PWD)" alpine sh -c 'apk add --no-cache zstd >/dev/null 2>&1 && tar --zstd -xf packages/$$name.pkg.tar.zst -C main'; \
			else \
				tar --zstd -xf packages/$$name.pkg.tar.zst -C main; \
			fi; \
		done && \
		if [ "$(KERNEL)" = "Darwin" ]; then \
			docker run --rm -v "$(PWD)":"$(PWD)" -w "$(PWD)" alpine sh -c ' \
				main/usr/bin/busybox --list-full | grep -v "^bin/busybox$$" | while read applet; do \
					dir="main/$$(dirname $$applet)" && \
					target=$$(realpath --relative-to="$$dir" main/usr/bin/busybox) && \
					ln -sf "$$target" "main/$$applet"; \
				done \
			'; \
		else \
			main/usr/bin/busybox --list-full | grep -v '^bin/busybox$$' | while read applet; do \
				dir="main/$$(dirname $$applet)" && \
				target=$$(realpath --relative-to="$$dir" main/usr/bin/busybox) && \
				ln -sf "$$target" "main/$$applet"; \
			done; \
		fi && \
		(cd main && ln -sf sbin/init init) && \
		rm -f main/linuxrc && \
		printf "  %-7s %s\n" "CPIO" "$(SUBDIR)/$(OUTFILE)" && \
		(cd main && find . -print0 | $(CPIO) $(CPIOFLAGS) 2>/dev/null) | gzip -9 > $(OUTFILE); \
	else \
		printf "  %-7s %s\n" "INFO" "$(SUBDIR)/$(OUTFILE) is already up to date"; \
	fi

clean:
	@printf "  %-7s %s\n" "CLEAN" "$(SUBDIR)/$(OUTFILE)"
	@rm -rf $(OUTFILE)

mrproper: clean
	@printf "  %-7s %s\n" "CLEAN" "$(SUBDIR)/main/ $(SUBDIR)/packages/"
	@rm -rf main packages

.PHONY: all initrd clean mrproper
