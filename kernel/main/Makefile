ifeq ($(filter --no-print-directory,$(MAKEFLAGS)),)
MAKEFLAGS += --no-print-directory
endif
ifeq ($(filter --silent,$(MAKEFLAGS)),)
MAKEFLAGS += --silent
endif

include ../config.mk

SRC = $(wildcard *.c) $(wildcard *.S)
OBJ = $(SRC:.c=.o)
OBJ := $(OBJ:.S=.o)
DEP := $(OBJ:.o=.d)

all: $(OBJ) ../config.mk

-include $(DEP)

%.o: %.c ../config.mk ../include/limine/limine.h
	@printf "  %-7s %s\n" "CC" "$(SUBDIR)/main/$@"
	@$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S ../config.mk ../include/limine/limine.h
	@printf "  %-7s %s\n" "AS" "$(SUBDIR)/main/$@"
	@$(AS) $(AFLAGS) -c $< -o $@

clean:
	@printf "  %-7s %s\n" "CLEAN" "$(SUBDIR)/main/*.o $(SUBDIR)/main/*.d"
	@rm -rf *.o *.d

mrproper: clean

.PHONY: all clean mrproper
