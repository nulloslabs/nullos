ifeq ($(filter --no-print-directory,$(MAKEFLAGS)),)
MAKEFLAGS += --no-print-directory
endif
ifeq ($(filter --silent,$(MAKEFLAGS)),)
MAKEFLAGS += --silent
endif

.DEFAULT_GOAL := all

include ../../config.mk

PROGS = $(patsubst %.c,%,$(wildcard *.c))

all: $(foreach p,$(PROGS),../../$(p))

# Rule 1: Link the unstripped binary if the object file changes
../../%.unstripped: %.o
	@printf "  %-7s %s\n" "LD" "$(SUBDIR)/$*.unstripped"
	$(LD) $(LDFLAGS) ../../crt0.o $< -o $@ -Wl,--no-dynamic-linker -l:libc.so

# Rule 2: Strip the unstripped binary directly into the target directory
../../%: ../../%.unstripped
	@printf "  %-7s %s\n" "STRIP" "$(SUBDIR)/$*"
	$(STRIP) $(STRIPFLAGS) $< -o $@

%.o: %.c
	@printf "  %-7s %s\n" "CC" "$(SUBDIR)/main/progs/$@"
	@$(CC) $(CFLAGS) -c $< -o $@

clean:
	@printf "  %-7s %s\n" "CLEAN" "$(SUBDIR)/main/progs/*.o"
	@rm -f *.o

mrproper: clean
	@rm -f $(foreach p,$(PROGS),../../$(p) ../../$(p).unstripped)

.PHONY: all clean mrproper
