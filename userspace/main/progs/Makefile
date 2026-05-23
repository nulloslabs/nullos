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
			if [ ! -f $$t ] || [ $$o -nt $$t ] || [ ../../crt0.o -nt $$t ] || [ ../../libc.a -nt $$t ] || [ ../../ld.so -nt $$t ]; then \
			needs_build=1; \
		fi; \
	done; \
	if [ $$needs_build -eq 0 ]; then \
		printf "  %-7s %s\n" "INFO" "$$(echo $(PROGS) | tr ' ' '\n' | sed 's|^|$(SUBDIR)/|' | paste -sd ' ' -) $$([ $$(echo $(PROGS) | wc -w) -eq 1 ] && echo 'is' || echo 'are') already up to date"; \
	else \
		$(MAKE) MAKEFLAGS="$(MAKEFLAGS)" $(TARGETS); \
	fi

# Inline execution blueprint for each application target
define PROG_rule
../../$(1): $(1).o ../../crt0.o ../../libc.so ../../libc.a ../../ld.so
	@printf "  %-7s %s\n" "LD" "$(SUBDIR)/$(1)"
	$(LD) $(LDFLAGS) ../../crt0.o $$< -o $$@ -Wl,--dynamic-linker=/ld.so -l:libc.so
	@printf "  %-7s %s\n" "STRIP" "$(SUBDIR)/$(1)"
	$(STRIP) $(STRIPFLAGS) $$@
endef

$(foreach p,$(PROGS),$(eval $(call PROG_rule,$(p))))

%.o: %.c

	@printf "  %-7s %s\n" "CC" "$(SUBDIR)/main/progs/$@"
	@$(CC) $(CFLAGS) -c $< -o $@

clean:
	@printf "  %-7s %s\n" "CLEAN" "$(foreach p,$(PROGS),$(SUBDIR)/$(p)) $(SUBDIR)/main/progs/*.o $(SUBDIR)/main/progs/*.d"
	@rm -f $(foreach p,$(PROGS),../../$(p)) *.o *.d

mrproper: clean
.PHONY: all clean mrproper progs
