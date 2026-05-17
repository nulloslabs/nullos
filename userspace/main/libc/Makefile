ifeq ($(filter --no-print-directory,$(MAKEFLAGS)),)
MAKEFLAGS += --no-print-directory
endif
ifeq ($(filter --silent,$(MAKEFLAGS)),)
MAKEFLAGS += --silent
endif

.DEFAULT_GOAL := all

include ../../config.mk

SRC = $(wildcard *.c) $(wildcard *.S)
OBJ = $(SRC:.c=.o)
OBJ := $(OBJ:.S=.o)
DEP = $(OBJ:.o=.d)

-include $(DEP)

all: libc

libc: $(OBJ)
	@needs_build_so=0; needs_build_a=0; \
	for obj in $(OBJ); do \
		if [ ! -f ../../libc.so ] || [ $$obj -nt ../../libc.so ]; then needs_build_so=1; fi; \
		if [ ! -f ../../libc.a ] || [ $$obj -nt ../../libc.a ]; then needs_build_a=1; fi; \
	done; \
	if [ $$needs_build_so -eq 1 ]; then \
		printf "  %-7s %s\n" "LD" "$(SUBDIR)/libc.so.unstripped" && \
		$(LD) $(LDFLAGS) -shared $(OBJ) -o ../../libc.so.unstripped -Wl,-soname,libc.so && \
		printf "  %-7s %s\n" "STRIP" "$(SUBDIR)/libc.so" && \
		$(STRIP) $(STRIPFLAGS) libc.so.unstripped -o libc.so; \
	fi; \
	if [ $$needs_build_a -eq 1 ]; then \
		printf "  %-7s %s\n" "AR" "$(SUBDIR)/libc.a.unstripped" && \
		$(AR) $(ARFLAGS) rcs ../../libc.a.unstripped $(OBJ) && \
		printf "  %-7s %s\n" "STRIP" "$(SUBDIR)/libc.a" && \
		$(STRIP) $(STRIPFLAGS) --strip-debug libc.a.unstripped -o libc.a; \
	fi; \
	if [ $$needs_build_so -eq 0 ] && [ $$needs_build_a -eq 0 ]; then \
		printf "  %-7s %s\n" "INFO" "$(SUBDIR)/libc.so $(SUBDIR)/libc.a are already up to date"; \
	fi

%.o: %.c
	@printf "  %-7s %s\n" "CC" "$(SUBDIR)/main/libc/$@"
	@$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S
	@printf "  %-7s %s\n" "AS" "$(SUBDIR)/main/libc/$@"
	@$(CC) $(CFLAGS) -c $< -o $@

clean:
	@printf "  %-7s %s\n" "CLEAN" "$(SUBDIR)/main/libc/*.o $(SUBDIR)/main/libc/*.d"
	@rm -rf $(OBJ) $(DEP) ../../libc.so ../../libc.a

mrproper: clean

.PHONY: all libc clean mrproper
