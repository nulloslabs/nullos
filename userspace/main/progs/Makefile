ifeq ($(filter --no-print-directory,$(MAKEFLAGS)),)
MAKEFLAGS += --no-print-directory
endif
ifeq ($(filter --silent,$(MAKEFLAGS)),)
MAKEFLAGS += --silent
endif

.DEFAULT_GOAL := all

include ../../config.mk

PROGS = $(patsubst %.c,%,$(wildcard *.c))
TARGETS = $(foreach p,$(PROGS),../../$(p))

all: progs

progs: $(PROGS:=.o)
	@needs_build=0; \
	for p in $(PROGS); do \
		t="../../$$p"; \
		o="$$p.o"; \
		if [ ! -f $$t ] || [ $$o -nt $$t ]; then \
			needs_build=1; \
		fi; \
	done; \
	if [ $$needs_build -eq 0 ]; then \
		printf "  %-7s %s\n" "INFO" "$$(echo $(PROGS) | tr ' ' '\n' | sed 's|^|$(SUBDIR)/|' | paste -sd ' ' -) $$([ $$(echo $(PROGS) | wc -w) -eq 1 ] && echo 'is' || echo 'are') already up to date"; \
	fi
	@$(MAKE) MAKEFLAGS="$(MAKEFLAGS)" $(TARGETS)

# Inline execution blueprint for each application target
define PROG_rule
../../$(1): $(1).o
	@printf "  %-7s %s\n" "LD" "$(SUBDIR)/$(1).unstripped"
	$(LD) $(LDFLAGS) ../../crt0.o $$< -o ../../$(1).unstripped -Wl,--no-dynamic-linker -l:libc.so
	@printf "  %-7s %s\n" "STRIP" "$(SUBDIR)/$(1)"
	$(STRIP) $(STRIPFLAGS) ../../$(1).unstripped -o $$@
endef
$(foreach p,$(PROGS),$(eval $(call PROG_rule,$(p))))

%.o: %.c
	@printf "  %-7s %s\n" "CC" "$(SUBDIR)/main/progs/$@"
	@$(CC) $(CFLAGS) -c $< -o $@

clean:
	@printf "  %-7s %s\n" "CLEAN" "$(SUBDIR)/main/progs/*.o"
	@rm -f *.o

mrproper: clean
	@rm -f $(foreach p,$(PROGS),../../$(p) ../../$(p).unstripped)

.PHONY: all clean mrproper progs
