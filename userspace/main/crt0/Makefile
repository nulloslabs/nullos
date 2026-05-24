ifeq ($(filter --no-print-directory,$(MAKEFLAGS)),)
MAKEFLAGS += --no-print-directory
endif
ifeq ($(filter --silent,$(MAKEFLAGS)),)
MAKEFLAGS += --silent
endif

.DEFAULT_GOAL := all

include ../../config.mk

-include $(DEP)

all: crt0

crt0: crt0.S Makefile ../../config.mk
	@if [ -f ../../crt0.o ] && [ ../../crt0.o -nt crt0.S ] && [ ../../crt0.o -nt Makefile ] && [ ../../crt0.o -nt ../../config.mk ]; then \
		printf "  %-7s %s\n" "INFO" "$(SUBDIR)/crt0.o is already up to date"; \
	else \
		printf "  %-7s %s\n" "AS" "$(SUBDIR)/crt0.o" && \
		# I fucking don't know why but it makes a crt0.o.d without -MF (wtf) && \
		$(CC) $(CFLAGS) -MF crt0.d -c $< -o ../../crt0.o && \
		printf "  %-7s %s\n" "STRIP" "$(SUBDIR)/crt0.o" && \
		$(STRIP) $(STRIPFLAGS) --strip-debug ../../crt0.o; \
	fi

clean:
	@printf "  %-7s %s\n" "CLEAN" "$(SUBDIR)/crt0.o $(SUBDIR)/userspace/main/*.d"
	@rm -rf ../../crt0.o *.d

mrproper: clean

.PHONY: all crt0 clean mrproper
