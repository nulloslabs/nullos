ifeq ($(filter --no-print-directory,$(MAKEFLAGS)),)
MAKEFLAGS += --no-print-directory
endif
ifeq ($(filter --silent,$(MAKEFLAGS)),)
MAKEFLAGS += --silent
endif

# IDK why this works :sob:
.DEFAULT_GOAL := all

include ../../config.mk

# NOTE: crt0.S is a seperate object, not part of libc.
SRC = unistd.c string.c stdio.c fcntl.c errno.c ioctl.c stat.c dirent.c mount.c wait.c
OBJ = $(SRC:.c=.o)
DEP = $(OBJ:.o=.d)

-include $(DEP)

all: crt0_info ../../crt0.o libc_so_info ../../libc.so libc_a_info ../../libc.a

crt0_info:
	@if $(MAKE) -q ../../crt0.o; then printf "  %-7s %s\n" "INFO" "$(SUBDIR)/crt0.o is already up to date"; fi

libc_so_info:
	@if $(MAKE) -q ../../libc.so; then printf "  %-7s %s\n" "INFO" "$(SUBDIR)/libc.so is already up to date"; fi

libc_a_info:
	@if $(MAKE) -q ../../libc.a; then printf "  %-7s %s\n" "INFO" "$(SUBDIR)/libc.a is already up to date"; fi

%.o: %.c
	@printf "  %-7s %s\n" "CC" "$(SUBDIR)/main/libc/$@"
	@$(CC) $(CFLAGS) -c $< -o $@

../../crt0.o: crt0.S
	@printf "  %-7s %s\n" "AS" "$(SUBDIR)/crt0.o"
	@$(CC) $(CFLAGS) -c $< -o $@
	@rm -f ../../crt0.d

../../libc.so: $(OBJ)
	@printf "  %-7s %s\n" "LD" "$(SUBDIR)/libc.so"
	@$(LD) $(LDFLAGS) -shared $^ -o $@ -Wl,-soname,libc.so

../../libc.a: $(OBJ)
	@printf "  %-7s %s\n" "AR" "$(SUBDIR)/libc.a"
	@$(AR) $(ARFLAGS) rcs $@ $^

clean:
	@printf "  %-7s %s\n" "CLEAN" "$(SUBDIR)/main/libc/*.o $(SUBDIR)/main/libc/*.d"
	@rm -rf $(OBJ) $(DEP) ../../crt0.o ../../libc.so ../../libc.a

mrproper: clean

.PHONY: all clean mrproper
