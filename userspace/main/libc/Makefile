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

all: 
	@start=$$(date +%s); \
	$(MAKE) ../../libc.so ../../libc.a; \
	needs_build=0; \
	for f in ../../libc.so ../../libc.a; do \
		if [ $$(date +%s -r $$f) -ge $$start ]; then \
			needs_build=1; break; \
		fi; \
	done; \
	if [ $$needs_build -eq 0 ]; then \
		printf "  %-7s %s\n" "INFO" "$(SUBDIR)/libc.so, $(SUBDIR)/libc.a are already up to date"; \
	fi

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
