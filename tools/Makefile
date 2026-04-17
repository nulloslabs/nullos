ifeq ($(filter --no-print-directory,$(MAKEFLAGS)),)
MAKEFLAGS += --no-print-directory
endif
ifeq ($(filter --silent,$(MAKEFLAGS)),)
MAKEFLAGS += --silent
endif

include config.mk

all: tools

tools:
	@if [ ! -f limine/limine ] || [ config.mk -nt limine/limine ] || [ Makefile -nt limine/limine ]; then \
		if [ ! -d limine/.git ]; then \
			mkdir -p limine; \
			printf "  %-7s %s\n" "FETCH" "$(SUBDIR)/limine/"; \
			git clone --branch v11.x-binary --depth=1 https://github.com/limine-bootloader/limine 2>/dev/null; \
		fi; \
		printf "  %-7s %s\n" "CC" "$(SUBDIR)/limine/limine.o"; \
		(cd limine && $(CC) $(CFLAGS) -c limine.c -o limine.o); \
		printf "  %-7s %s\n" "LD" "$(SUBDIR)/limine/limine"; \
		(cd limine && $(LD) $(LDFLAGS) limine.o -o limine); \
	else \
		printf "  %-7s %s\n" "INFO" "$(SUBDIR)/limine/ is already up to date"; \
	fi

clean:
	@:

mrproper: clean
	@printf "  %-7s %s\n" "CLEAN" "$(SUBDIR)/limine/"
	@rm -rf limine

.PHONY: all tools clean mrproper