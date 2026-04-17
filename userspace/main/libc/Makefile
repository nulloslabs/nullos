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

# Print info if up to date
ifeq ($(shell \
	if [ ! -f ../../libc.so ]; then echo 0; exit; fi; \
	for f in $(SRC); do if [ "$$f" -nt "../../libc.so" ]; then echo 0; exit; fi; done; \
	echo 1),1)
$(info $(empty)  INFO    $(SUBDIR)/libc.so is already up to date)
endif

ifeq ($(shell \
	if [ ! -f ../../libc.a ]; then echo 0; exit; fi; \
	for f in $(SRC); do if [ "$$f" -nt "../../libc.a" ]; then echo 0; exit; fi; done; \
	echo 1),1)
$(info $(empty)  INFO    $(SUBDIR)/libc.a is already up to date)
endif

all: ../../libc.so ../../libc.a

%.o: %.c
	@printf "  %-7s %s\n" "CC" "$(SUBDIR)/main/libc/$@"
	@$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S
	@printf "  %-7s %s\n" "AS" "$(SUBDIR)/main/libc/$@"
	@$(CC) $(CFLAGS) -c $< -o $@

../../libc.so: $(OBJ)
	@printf "  %-7s %s\n" "LD" "$(SUBDIR)/libc.so"
	@$(LD) $(LDFLAGS) -shared $^ -o $@ -Wl,-soname,libc.so

../../libc.a: $(OBJ)
	@printf "  %-7s %s\n" "AR" "$(SUBDIR)/libc.a"
	@$(AR) $(ARFLAGS) rcs $@ $^

clean:
	@printf "  %-7s %s\n" "CLEAN" "$(SUBDIR)/main/libc/*.o $(SUBDIR)/main/libc/*.d"
	@rm -rf $(OBJ) $(DEP) ../../libc.so ../../libc.a

mrproper: clean

.PHONY: all clean mrproper
