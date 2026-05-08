ifeq ($(filter --no-print-directory,$(MAKEFLAGS)),)
MAKEFLAGS += --no-print-directory
endif
ifeq ($(filter --silent,$(MAKEFLAGS)),)
MAKEFLAGS += --silent
endif

# IDK why this works :sob:
.DEFAULT_GOAL := all

include ../../config.mk

SRC = $(wildcard *.c) $(wildcard *.S)
OBJ = $(SRC:.c=.o)
OBJ := $(OBJ:.S=.o)
DEP = $(OBJ:.o=.d)

-include $(DEP)

all: ../../libc.so ../../libc.a

%.o: %.c
	@printf "  %-7s %s\n" "CC" "$(SUBDIR)/main/libc/$@"
	@$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S
	@printf "  %-7s %s\n" "AS" "$(SUBDIR)/main/libc/$@"
	@$(CC) $(CFLAGS) -c $< -o $@

../../libc.so: $(OBJ)
	@if [ -f ../../libc.so ] && { for f in $(SRC); do [ "$$f" -nt "../../libc.so" ] && exit 1; done; true; }; then \
		printf "  %-7s %s\n" "INFO" "$(SUBDIR)/libc.so is already up to date"; \
	else \
		printf "  %-7s %s\n" "LD" "$(SUBDIR)/libc.so"; \
		$(LD) $(LDFLAGS) -shared $^ -o $@ -Wl,-soname,libc.so; \
	fi

../../libc.a: $(OBJ)
	@if [ -f ../../libc.a ] && { for f in $(SRC); do [ "$$f" -nt "../../libc.a" ] && exit 1; done; true; }; then \
		printf "  %-7s %s\n" "INFO" "$(SUBDIR)/libc.a is already up to date"; \
	else \
		printf "  %-7s %s\n" "AR" "$(SUBDIR)/libc.a"; \
		$(AR) $(ARFLAGS) rcs $@ $^; \
	fi

clean:
	@printf "  %-7s %s\n" "CLEAN" "$(SUBDIR)/main/libc/*.o $(SUBDIR)/main/libc/*.d"
	@rm -rf $(OBJ) $(DEP) ../../libc.so ../../libc.a

mrproper: clean

.PHONY: all ../../libc.so ../../libc.a clean mrproper
