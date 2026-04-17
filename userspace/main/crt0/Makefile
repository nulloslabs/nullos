ifeq ($(filter --no-print-directory,$(MAKEFLAGS)),)
MAKEFLAGS += --no-print-directory
endif
ifeq ($(filter --silent,$(MAKEFLAGS)),)
MAKEFLAGS += --silent
endif

# IDK why this works :sob:
.DEFAULT_GOAL := all

include ../../config.mk

-include $(DEP)

# Print info if up to date
ifeq ($(shell [ -f ../../crt0.o ] && [ ../../crt0.o -nt crt0.S ] && echo 1),1)
$(info $(empty)  INFO    $(SUBDIR)/crt0.o is already up to date)
endif


all: ../../crt0.o

../../crt0.o: crt0.S
	@printf "  %-7s %s\n" "AS" "$(SUBDIR)/crt0.o"
	@$(CC) $(CFLAGS) -c $< -o $@

clean:
	@printf "  %-7s %s\n" "CLEAN" "$(SUBDIR)/crt0.o"
	@rm -rf ../../crt0.o

mrproper: clean

.PHONY: all clean mrproper
