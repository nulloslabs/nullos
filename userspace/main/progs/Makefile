ifeq ($(filter --no-print-directory,$(MAKEFLAGS)),)
MAKEFLAGS += --no-print-directory
endif
ifeq ($(filter --silent,$(MAKEFLAGS)),)
MAKEFLAGS += --silent
endif

# IDK why this works :sob:
.DEFAULT_GOAL := all

include ../../config.mk

PROGS = $(patsubst %.c,%,$(wildcard *.c))

DEP :=
define PROG_template
$(shell echo $(1) | tr a-z A-Z)_OBJ = $(1).o
DEP += $$($(shell echo $(1) | tr a-z A-Z)_OBJ:.o=.d)
../../$(1): $$($(shell echo $(1) | tr a-z A-Z)_OBJ) ../../crt0.o ../../libc.so
	@printf "  %-7s %s\n" "LD" "$(SUBDIR)/$(1)"; \
	$(LD) $(LDFLAGS) ../../crt0.o $$($(shell echo $(1) | tr a-z A-Z)_OBJ) -o $$@ -Wl,--no-dynamic-linker -l:libc.so
	$(eval BUILT := 1)
endef
$(foreach p,$(PROGS),$(eval $(call PROG_template,$(p))))

-include $(DEP)

all: $(addprefix ../../,$(PROGS))
	@needs_build=0; \
	for p in $(PROGS); do \
		if [ ! -f ../../$$p ] || [ $$p.c -nt ../../$$p ] || [ $$p.o -nt ../../$$p ] || [ ../../crt0.o -nt ../../$$p ] || [ ../../libc.so -nt ../../$$p ]; then \
			needs_build=1 && break; \
		fi; \
	done; \
	if [ $$needs_build -eq 0 ]; then \
		printf "  %-7s %s\n" "INFO" "$$(echo $(PROGS) | tr ' ' '\n' | sed 's|^|$(SUBDIR)/|' | tr '\n' ',' | sed 's/,$$//' | sed 's/,/, /g') $$([ $$(echo $(PROGS) | wc -w) -eq 1 ] && echo 'is' || echo 'are') already up to date"; \
	fi

%.o: %.c
	@printf "  %-7s %s\n" "CC" "$(SUBDIR)/main/progs/$@"
	@$(CC) $(CFLAGS) -c $< -o $@

clean:
	@printf "  %-7s %s\n" "CLEAN" "$(SUBDIR)/main/progs/*.o $(SUBDIR)/main/progs/*.d"
	@rm -f $(foreach p,$(PROGS),$($(shell echo $(p) | tr a-z A-Z)_OBJ)) $(DEP)

mrproper: clean

.PHONY: all clean mrproper
