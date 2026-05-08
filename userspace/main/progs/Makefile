ifeq ($(filter --no-print-directory,$(MAKEFLAGS)),)
MAKEFLAGS += --no-print-directory
endif
ifeq ($(filter --silent,$(MAKEFLAGS)),)
MAKEFLAGS += --silent
endif

# IDK why this works :sob:
.DEFAULT_GOAL := all

include ../../config.mk

PROGS = init login

DEP :=
define PROG_template
$(shell echo $(1) | tr a-z A-Z)_OBJ = $(1).o
DEP += $$($(shell echo $(1) | tr a-z A-Z)_OBJ:.o=.d)
../../$(1): $$($(shell echo $(1) | tr a-z A-Z)_OBJ) ../../crt0.o ../../libc.so
	@if [ -f ../../$(1) ] && [ ! $(1).c -nt ../../$(1) ] && [ ! $(1).o -nt ../../$(1) ] && [ ! ../../crt0.o -nt ../../$(1) ] && [ ! ../../libc.so -nt ../../$(1) ]; then \
		printf "  %-7s %s\n" "INFO" "$(SUBDIR)/$(1) is already up to date"; \
	else \
		printf "  %-7s %s\n" "LD" "$(SUBDIR)/$(1)"; \
		$(LD) $(LDFLAGS) ../../crt0.o $$($(shell echo $(1) | tr a-z A-Z)_OBJ) -o $$@ -Wl,--no-dynamic-linker -l:libc.so; \
	fi
endef
$(foreach p,$(PROGS),$(eval $(call PROG_template,$(p))))

-include $(DEP)

all: $(addprefix ../../,$(PROGS))

%.o: %.c
	@printf "  %-7s %s\n" "CC" "$(SUBDIR)/main/progs/$@"
	@$(CC) $(CFLAGS) -c $< -o $@

clean:
	@printf "  %-7s %s\n" "CLEAN" "$(SUBDIR)/main/progs/*.o $(SUBDIR)/main/progs/*.d"
	@rm -f $(foreach p,$(PROGS),$($(shell echo $(p) | tr a-z A-Z)_OBJ)) $(DEP)

mrproper: clean

.PHONY: all $(addprefix ../../,$(PROGS)) clean mrproper
