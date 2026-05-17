ifeq ($(filter --no-print-directory,$(MAKEFLAGS)),)
MAKEFLAGS += --no-print-directory
endif
ifeq ($(filter --silent,$(MAKEFLAGS)),)
MAKEFLAGS += --silent
endif

.DEFAULT_GOAL := all

include ../../config.mk

PROGS = $(patsubst %.c,%,$(wildcard *.c))

DEP :=
define PROG_template
$(shell echo $(1) | tr a-z A-Z)_OBJ = $(1).o
DEP += $$($(shell echo $(1) | tr a-z A-Z)_OBJ:.o=.d)
../../$(1): $$($(shell echo $(1) | tr a-z A-Z)_OBJ)
	@printf "  %-7s %s\n" "LD" "$(SUBDIR)/$(1)"; \
	$(LD) $(LDFLAGS) ../../crt0.o $$($(shell echo $(1) | tr a-z A-Z)_OBJ) -o $$@ -Wl,--no-dynamic-linker -l:libc.so
	$(eval BUILT := 1)
endef
$(foreach p,$(PROGS),$(eval $(call PROG_template,$(p))))

-include $(DEP)

all: progs

progs: $(foreach p,$(PROGS),$(p).o)
	@needs_build=0; \
	for p in $(PROGS); do \
		if [ ! -f ../../$$p ] || [ $$p.o -nt ../../$$p ]; then \
			needs_build=1; break; \
		fi; \
	done; \
	for p in $(PROGS); do \
		if [ ! -f ../../$$p ] || [ $$p.o -nt ../../$$p ]; then \
			printf "  %-7s %s\n" "LD" "$(SUBDIR)/$$p"; \
			$(LD) $(LDFLAGS) ../../crt0.o $$p.o -o ../../$$p -Wl,--no-dynamic-linker -l:libc.so; \
		fi; \
	done; \
	if [ $$needs_build -eq 0 ]; then \
		printf "  %-7s %s\n" "INFO" "$$(echo $(PROGS) | tr ' ' '\n' | sed 's|^|$(SUBDIR)/|' | paste -sd ' ' -) $$([ $$(echo $(PROGS) | wc -w) -eq 1 ] && echo 'is' || echo 'are') already up to date"; \
	fi

%.o: %.c
	@printf "  %-7s %s\n" "CC" "$(SUBDIR)/main/progs/$@"
	@$(CC) $(CFLAGS) -c $< -o $@

clean:
	@printf "  %-7s %s\n" "CLEAN" "$(SUBDIR)/main/progs/*.o $(SUBDIR)/main/progs/*.d"
	@rm -f $(foreach p,$(PROGS),$($(shell echo $(p) | tr a-z A-Z)_OBJ)) $(DEP)

mrproper: clean

.PHONY: all clean mrproper
