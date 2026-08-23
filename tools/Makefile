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
	@if [ ! -f limine-binary/$(LIMINE_OUTFILE) ] || [ config.mk -nt limine-binary/$(LIMINE_OUTFILE) ] || [ Makefile -nt limine-binary/$(LIMINE_OUTFILE) ]; then \
		printf "  %-7s %s\n" "CC" "$(SUBDIR)/limine-binary/limine.o" && \
		(cd limine-binary && $(CC) $(CFLAGS) -c limine.c -o limine.o) && \
		printf "  %-7s %s\n" "LD" "$(SUBDIR)/limine-binary/$(LIMINE_OUTFILE)" && \
		(cd limine-binary && $(LD) $(LDFLAGS) limine.o -o limine $(LIBS)) && \
		printf "  %-7s %s\n" "STRIP" "$(SUBDIR)/limine-binary/$(LIMINE_OUTFILE)" && \
		(cd limine-binary && $(STRIP) $(STRIPFLAGS) $(LIMINE_OUTFILE)); \
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
