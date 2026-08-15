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
	resolved_packages="$$(../scripts/resolve_packages.sh $(PACKAGE_NAMES))" || exit $$?; \
	resolved_packages="$$(printf '%s\n' "$$resolved_packages" | sed '/^core\/filesystem$$/d; /^core\/linux-api-headers$$/d')"; \
	current_names="$$(for p in $$resolved_packages; do basename $$p; done)"; \
	cached_names="$$(ls packages/*.pkg.tar.zst 2>/dev/null | xargs -n1 basename 2>/dev/null | sed 's/\.pkg\.tar\.zst$$//')"; \
	missing="$$(for n in $$current_names; do echo "$$cached_names" | grep -qxF "$$n" || echo missing_$$n; done)"; \
	extra="$$(for n in $$cached_names; do echo "$$current_names" | grep -qxF "$$n" || echo extra_$$n; done)"; \
	invalid="$$(for f in packages/*.pkg.tar.zst; do [ ! -f $$f ] || zstd -tq $$f || echo invalid_$$f; done)"; \
	if [ -n "$$extra" ]; then \
		for n in $$cached_names; do \
			echo "$$current_names" | grep -qxF "$$n" || { \
				if [ -f packages/$$n.pkg.tar.zst ] && [ -d main ]; then \
					tar --zstd -tf packages/$$n.pkg.tar.zst 2>/dev/null | while IFS= read -r f; do \
						case "$$f" in .*) continue ;; */) continue ;; esac; \
						if [ -f "main/$$f" ] || [ -L "main/$$f" ]; then rm -f -- "main/$$f"; fi; \
					done; \
				fi; \
				rm -f packages/$$n.pkg.tar.zst; \
			}; \
		done; \
		find main -depth -type d -empty -delete; \
	fi; \
	if [ ! -f $(OUTFILE) ] || ! gzip -t $(OUTFILE) 2>/dev/null || [ ! -d main ] || [ Makefile -nt $(OUTFILE) ] || [ config.mk -nt $(OUTFILE) ] || [ -n "$$missing" ] || [ -n "$$extra" ] || [ -n "$$invalid" ] || [ -n "$$(find main -newer $(OUTFILE) 2>/dev/null | head -1)" ]; then \
		mkdir -p main packages && \
		mkdir -p main/home main/root main/dev main/proc main/etc main/usr main/mnt main/var main/tmp main/run && \
		mkdir -p main/etc/init.d main/usr/bin main/usr/sbin main/usr/lib main/usr/include main/usr/share && \
		mkdir -p main/usr/share/udhcpc && \
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
			'tty1::respawn:/sbin/getty -L tty1 115200 linux' \
			'::ctrlaltdel:/sbin/reboot' \
			'::shutdown:/bin/umount -a -r' \
			> main/etc/inittab && \
		printf '%s\n' \
			'#!/bin/sh' \
			'' \
			'/bin/busybox --install -s' \
			'rm -rf /linuxrc' \
			'' \
			'mount -t devtmpfs devtmpfs /dev' \
			'mkdir -p /dev/pts' \
			'mount -t devpts devpts /dev/pts' \
			'mount -t proc proc /proc' \
			'mount -t tmpfs tmpfs /tmp' \
			'mount -t tmpfs run /run' \
			'hostname -F /etc/hostname' \
			'interface_index=0' \
			'while :; do' \
			'    interface=eth$$interface_index' \
			'    ip link show "$$interface" > /dev/null 2>&1 || break' \
			'    udhcpc -i "$$interface" -q -n > /dev/null 2>&1' \
			'    interface_index=$$((interface_index + 1))' \
			'done' \
			> main/etc/init.d/rcS && \
		chmod +x main/etc/init.d/rcS && \
		printf '%s\n' \
			'#!/bin/sh' \
			'' \
			'case "$$1" in' \
			'    deconfig)' \
			'        ifconfig "$$interface" 0.0.0.0' \
			'        ;;' \
			'    bound|renew)' \
			'        ifconfig "$$interface" "$$ip"' \
			'        [ -n "$$subnet" ] && ifconfig "$$interface" netmask "$$subnet"' \
			'        ifconfig "$$interface" up' \
			'        route del default dev "$$interface" 2>/dev/null' \
			'        for gateway_address in $$router; do' \
			'            route add default gw "$$gateway_address" dev "$$interface"' \
			'            break' \
			'        done' \
			'        : > /etc/resolv.conf' \
			'        for dns_address in $$dns; do' \
			'            echo "nameserver $$dns_address" >> /etc/resolv.conf' \
			'        done' \
			'        ;;' \
			'esac' \
			'' \
			'exit 0' \
			> main/usr/share/udhcpc/default.script && \
		chmod +x main/usr/share/udhcpc/default.script && \
		printf '%s\n' \
			'export PS1="\\w \\$$ "' \
			'export PATH=/usr/sbin:/usr/bin:/sbin:/bin' \
			'umask 022' \
			> main/etc/profile && \
		printf "NullOS (\\\\r)\n\n" > main/etc/issue && \
		printf "root:x:0:0:root:/root:/bin/bash\nuser:x:1000:1000:user:/home/user:/bin/bash\n" > main/etc/passwd && \
		printf "root:x:0:\nuser:x:1000:\n" > main/etc/group && \
		printf 'root:$$5$$nullos$$bdWJDaAMKrT0iGP6LnRmZJf5O8ImIPr0ChFp1JF2VID:19863:0:99999:7:::\n' > main/etc/shadow && \
		chmod 644 main/etc/passwd main/etc/group && \
		chmod 600 main/etc/shadow && \
		printf '%s\n' \
			'' \
			'Welcome to NullOS!' \
			'' \
			'An OS that'\''s secure, fast and private.' \
			'' \
			> main/etc/motd && \
		printf 'nullos' > main/etc/hostname && \
		printf '%s\n' \
			'nameserver 10.0.2.3' \
			'nameserver 1.1.1.1' \
			'nameserver 1.0.0.1' \
			> main/etc/resolv.conf && \
		printf '%s\n' \
			'[[ -f ~/.bashrc ]] && . ~/.bashrc' \
			> main/root/.bash_profile && \
		printf '%s\n' \
			'[[ $$- != *i* ]] && return' \
			'alias ls='\''ls --color=auto'\''' \
			> main/root/.bashrc && \
		for p in $$resolved_packages; do \
			repo=$$(dirname $$p) && \
			name=$$(basename $$p) && \
			package_file=packages/$$name.pkg.tar.zst && \
			package_tmp=packages/$$name.pkg.tar.zst.part && \
			{ [ ! -f $$package_file ] || zstd -tq $$package_file || rm -f $$package_file; } && \
			{ [ -f $$package_file ] || { rm -f $$package_tmp && \
			(curl -fsL -o $$package_tmp https://archlinux.org/packages/$$repo/x86_64/$$name/download/ || \
			 curl -fsL -o $$package_tmp https://archlinux.org/packages/$$repo/any/$$name/download/) && \
			zstd -tq $$package_tmp && mv $$package_tmp $$package_file; }; } && \
			tar --zstd -xf $$package_file -C main || exit 1; \
		done && \
		find main -maxdepth 1 -type f -regextype posix-extended -regex 'main/\.[A-Z]+' -delete && \
		(cd main && ln -sf /bin/busybox sbin/init && ln -sf sbin/init init) && \
		if [ -f main/bin/bash ] && [ ! -e main/bin/sh ]; then \
			(cd main && ln -sf bash bin/sh); \
		elif [ -f main/bin/busybox ] && [ ! -e main/bin/sh ]; then \
			(cd main && ln -sf /bin/busybox bin/sh); \
		fi && \
		rm -f main/linuxrc && \
		printf "  %-7s %s\n" "CPIO" "$(SUBDIR)/$(OUTFILE)" && \
		rm -f $(OUTFILE).cpio.part $(OUTFILE).part && \
		(cd main && find . -print0 | $(CPIO) $(CPIOFLAGS) 2>/dev/null > ../$(OUTFILE).cpio.part) && \
		gzip -9 < $(OUTFILE).cpio.part > $(OUTFILE).part && \
		gzip -t $(OUTFILE).part && \
		mv $(OUTFILE).part $(OUTFILE) && \
		rm -f $(OUTFILE).cpio.part; \
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
