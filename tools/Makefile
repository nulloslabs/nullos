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
		curl -fsSL --retry 3 -o limine-binary.tar.xz https://github.com/Limine-Bootloader/Limine/releases/latest/download/limine-binary.tar.xz && \
		tar -xJf limine-binary.tar.xz; \
	fi
	@if [ ! -f limine-binary/limine ] || [ config.mk -nt limine-binary/limine ] || [ Makefile -nt limine-binary/limine ]; then \
		printf "  %-7s %s\n" "CC" "$(SUBDIR)/limine-binary/limine.o" && \
		(cd limine-binary && $(CC) $(CFLAGS) -c limine.c -o limine.o) && \
		printf "  %-7s %s\n" "LD" "$(SUBDIR)/limine-binary/limine" && \
		(cd limine-binary && $(LD) $(LDFLAGS) limine.o -o limine); \
		if [ "$(DEBUG)" -eq 0 ]; then \
			printf "  %-7s %s\n" "STRIP" "$(SUBDIR)/limine-binary/limine" && \
			(cd limine-binary && $(STRIP) $(STRIPFLAGS) limine); \
		fi; \
	else \
		printf "  %-7s %s\n" "INFO" "$(SUBDIR)/limine-binary/limine is already up to date"; \
	fi

clean:
	@printf "  %-7s %s\n" "CLEAN" "$(SUBDIR)/limine-binary/limine $(SUBDIR)/limine-binary/*.d $(SUBDIR)/limine-binary/*.o"
	@rm -rf limine-binary/limine limine-binary/*.d limine-binary/*.o

mrproper: clean
	@printf "  %-7s %s\n" "CLEAN" "$(SUBDIR)/limine-binary.tar.xz $(SUBDIR)/limine-binary/"
	@rm -rf limine-binary.tar.xz limine-binary

.PHONY: all tools clean mrproper
