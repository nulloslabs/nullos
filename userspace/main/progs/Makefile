# progs/Makefile - links and strips programs
ifeq ($(filter --no-print-directory,$(MAKEFLAGS)),)
MAKEFLAGS += --no-print-directory
endif
ifeq ($(filter --silent,$(MAKEFLAGS)),)
MAKEFLAGS += --silent
endif

.DEFAULT_GOAL := all

include ../../config.mk

PROGS = $(patsubst main/%.c,%,$(wildcard main/*.c)) $(patsubst main/%.S,%,$(wildcard main/*.S))
TARGETS = $(foreach p,$(PROGS),../../$(p))

all: main
	@needs_build=0; \
	for p in $(PROGS); do \
		t="../../$$p"; \
		o="main/$$p.o"; \
		if [ ! -f $$t ] || [ $$o -nt $$t ] || [ ../../crt0.o -nt $$t ] || [ ../../libc.a -nt $$t ] || [ ../../ld.so -nt $$t ]; then \
			needs_build=1; \
		fi; \
	done; \
	if [ $$needs_build -eq 0 ]; then \
		printf "  %-7s %s\n" "INFO" "$(foreach p,$(PROGS),$(SUBDIR)/$(p)) are already up to date"; \
	else \
		$(MAKE) $(TARGETS); \
	fi

main:
	@$(MAKE) -C main

define PROG_rule
../../$(1): main/$(1).o ../../crt0.o ../../libc.so ../../libc.a ../../ld.so Makefile ../../config.mk
	@printf "  %-7s %s\n" "LD" "$(SUBDIR)/$(1)"
	$(LD) $(LDFLAGS) ../../crt0.o main/$(1).o -o $$@ -L../.. -Wl,--dynamic-linker=/usr/lib/ld.so -l:libc.so
	@printf "  %-7s %s\n" "STRIP" "$(SUBDIR)/$(1)"
	$(STRIP) $(STRIPFLAGS) $$@
endef
$(foreach p,$(PROGS),$(eval $(call PROG_rule,$(p))))

clean:
	@$(MAKE) -C main clean
	@printf "  %-7s %s\n" "CLEAN" "$(foreach p,$(PROGS),$(SUBDIR)/$(p))"
	@rm -f $(TARGETS)

mrproper: clean

.PHONY: all main clean mrproper
