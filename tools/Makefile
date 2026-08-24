ifeq ($(filter --no-print-directory,$(MAKEFLAGS)),)
MAKEFLAGS += --no-print-directory
endif
ifeq ($(filter --silent,$(MAKEFLAGS)),)
MAKEFLAGS += --silent
endif

include config.mk

all: tools

tools:
	@if [ ! -f limine-binary.tar.xz ] || [ config.mk -nt limine-binary.tar.xz ] || [ Makefile -nt limine-binary.tar.xz ]; then \
		printf "  %-7s %s\n" "FETCH" "$(SUBDIR)/limine-binary/" && \
		curl -fsSL --retry 3 -o limine-binary.tar.xz https://github.com/Limine-Bootloader/Limine/releases/latest/download/limine-binary.tar.xz && \
		tar -xJf limine-binary.tar.xz; \
	fi
	@needs_build=0; \
	has_debug="n"; \
	if [ -f "limine-binary/$(LIMINE_OUTFILE)" ] && $(OBJDUMP) -h "limine-binary/$(LIMINE_OUTFILE)" 2>/dev/null | grep -q '\.debug'; then \
		has_debug="y"; \
	fi; \
	has_debug_cfg="n"; if [ "$(DEBUG)" = "1" ]; then has_debug_cfg="y"; fi; \
	if [ ! -f "limine-binary/$(LIMINE_OUTFILE)" ] || [ "$$has_debug_cfg" != "$$has_debug" ] || [ "config.mk" -nt "limine-binary/$(LIMINE_OUTFILE)" ] || [ "Makefile" -nt "limine-binary/$(LIMINE_OUTFILE)" ]; then \
		needs_build=1; \
	fi; \
	if [ $$needs_build -eq 1 ]; then \
		printf "  %-7s %s\n" "CC" "$(SUBDIR)/limine-binary/limine.o" && \
		(cd limine-binary && $(CC) $(CFLAGS) -c limine.c -o limine.o) && \
		printf "  %-7s %s\n" "LD" "$(SUBDIR)/limine-binary/$(LIMINE_OUTFILE)" && \
		(cd limine-binary && $(LD) $(LDFLAGS) limine.o -o $(LIMINE_OUTFILE) $(LIBS)) && \
		if [ "$(DEBUG)" -eq 0 ]; then \
			printf "  %-7s %s\n" "STRIP" "$(SUBDIR)/limine-binary/$(LIMINE_OUTFILE)" && \
			(cd limine-binary && $(STRIP) $(STRIPFLAGS) $(LIMINE_OUTFILE)); \
		fi; \
	else \
		printf "  %-7s %s\n" "INFO" "$(SUBDIR)/limine-binary/$(LIMINE_OUTFILE) is already up to date"; \
	fi

clean:
	@printf "  %-7s %s\n" "CLEAN" "$(SUBDIR)/limine-binary/$(LIMINE_OUTFILE)"
	@rm -rf limine-binary/$(LIMINE_OUTFILE)
	@printf "  %-7s %s\n" "CLEAN" "$(SUBDIR)/limine-binary/*.o $(SUBDIR)/limine-binary/*.d"
	@rm -rf limine-binary/*.d limine-binary/*.o

mrproper: clean
	@printf "  %-7s %s\n" "CLEAN" "$(SUBDIR)/limine-binary.tar.xz $(SUBDIR)/limine-binary/"
	@rm -rf limine-binary.tar.xz limine-binary

.PHONY: all tools clean mrproper
