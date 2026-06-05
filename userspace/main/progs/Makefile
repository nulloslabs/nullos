ifeq ($(filter --no-print-directory,$(MAKEFLAGS)),)
MAKEFLAGS += --no-print-directory
endif
ifeq ($(filter --silent,$(MAKEFLAGS)),)
MAKEFLAGS += --silent
endif

.DEFAULT_GOAL := all

include ../../config.mk

SRC = $(wildcard main/*.c) $(wildcard main/*.S)
OBJ = $(SRC:.c=.o)
OBJ := $(OBJ:.S=.o)

PROGS = $(patsubst main/%,%,$(basename $(OBJ)))
TARGETS = $(foreach p,$(PROGS),../../$(p))

all: progs

progs: $(OBJ)
	@needs_build=0; \
	for p in $(PROGS); do \
		t="../../$$p"; \
		o="main/$$p.o"; \
		if [ ! -f $$t ] || [ $$o -nt $$t ] || [ ../../crt0.o -nt $$t ] || [ ../../libc.a -nt $$t ] || [ ../../ld.so -nt $$t ] || [ Makefile -nt $$t ] || [ ../../config.mk -nt $$t ]; then \
			needs_build=1; \
		fi; \
	done; \
	if [ $$needs_build -eq 0 ]; then \
		printf "  %-7s %s\n" "INFO" "$$(echo $(PROGS) | tr ' ' '\n' | sed 's|^|$(SUBDIR)/|' | paste -sd ' ' -) $$([ $$(echo $(PROGS) | wc -w) -eq 1 ] && echo 'is' || echo 'are') already up to date"; \
	else \
		$(MAKE) MAKEFLAGS="$(MAKEFLAGS)" $(TARGETS); \
	fi

define PROG_rule
../../$(1): main/$(1).o ../../crt0.o ../../libc.so ../../libc.a ../../ld.so Makefile ../../config.mk
	@printf "  %-7s %s\n" "LD" "$(SUBDIR)/$(1)"
	$(LD) $(LDFLAGS) ../../crt0.o $$< -o $$@ -L../.. -Wl,--dynamic-linker=/usr/lib/ld.so -l:libc.so
	@printf "  %-7s %s\n" "STRIP" "$(SUBDIR)/$(1)"
	$(STRIP) $(STRIPFLAGS) $$@
endef

$(foreach p,$(PROGS),$(eval $(call PROG_rule,$(p))))

main/%.o: main/%.c Makefile ../../config.mk
	@printf "  %-7s %s\n" "CC" "$(SUBDIR)/$@"
	@$(CC) $(CFLAGS) -c $< -o $@

main/%.o: main/%.S Makefile ../../config.mk
	@printf "  %-7s %s\n" "AS" "$(SUBDIR)/$@"
	@$(AS) $(AFLAGS) -c $< -o $@

clean:
	@printf "  %-7s %s\n" "CLEAN" "$(foreach p,$(PROGS),$(SUBDIR)/$(p)) $(SUBDIR)/main/*.o $(SUBDIR)/main/*.d"
	@rm -f $(foreach p,$(PROGS),../../$(p)) main/*.o main/*.d

mrproper: clean

.PHONY: all progs clean mrproper
