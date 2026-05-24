ifeq ($(filter --no-print-directory,$(MAKEFLAGS)),)
MAKEFLAGS += --no-print-directory
endif
ifeq ($(filter --silent,$(MAKEFLAGS)),)
MAKEFLAGS += --silent
endif

.DEFAULT_GOAL := all

include ../../config.mk

SRC = $(wildcard *.S) $(wildcard *.c)
OBJ = $(SRC:.c=.o)
DEP = $(SRC:.c=.d)

-include $(DEP)

all: ld

ld: $(OBJ)
	@needs_build=0; \
	if [ ! -f ../../ld.so ] || [ $(OBJ) -nt ../../ld.so ] || [ Makefile -nt ../../ld.so ] || [ ../../config.mk -nt ../../ld.so ]; then needs_build=1; fi; \
	if [ $$needs_build -eq 1 ]; then \
		printf "  %-7s %s\n" "LD" "$(SUBDIR)/ld.so" && \
		$(LD) $(LDFLAGS) -shared -fPIC -nostdlib -e _start $(OBJ) -o ../../ld.so && \
		printf "  %-7s %s\n" "STRIP" "$(SUBDIR)/ld.so" && \
		$(STRIP) $(STRIPFLAGS) ../../ld.so; \
	else \
		printf "  %-7s %s\n" "INFO" "$(SUBDIR)/ld.so is already up to date"; \
	fi

%.o: %.c Makefile ../../config.mk
	@printf "  %-7s %s\n" "CC" "$(SUBDIR)/main/ld/$@"
	@$(CC) $(CFLAGS) -c $< -o $@

clean:
	@printf "  %-7s %s\n" "CLEAN" "$(SUBDIR)/ld.so $(SUBDIR)/main/ld/*.o $(SUBDIR)/main/ld/*.d"
	@rm -rf ../../ld.so $(OBJ) $(DEP)
mrproper: clean

.PHONY: all ld clean mrproper
