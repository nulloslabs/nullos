ifeq ($(filter --no-print-directory,$(MAKEFLAGS)),)
MAKEFLAGS += --no-print-directory
endif
ifeq ($(filter --silent,$(MAKEFLAGS)),)
MAKEFLAGS += --silent
endif

.DEFAULT_GOAL := all

include ../../config.mk

SUBDIRS = main sys netinet
ALL_OBJ = $(wildcard */*.o)

all: $(SUBDIRS) libc

$(SUBDIRS):
	@$(MAKE) -C $@

libc: $(SUBDIRS)
	@needs_build_so=0; needs_build_a=0; \
	OBJ="$(wildcard */*.o)"; \
	if [ ! -f ../../libc.so ]; then needs_build_so=1; fi; \
	if [ ! -f ../../libc.a ]; then needs_build_a=1; fi; \
	for obj in $$OBJ; do \
		if [ ../../libc.so -ot $$obj ]; then needs_build_so=1; fi; \
		if [ ../../libc.a -ot $$obj ]; then needs_build_a=1; fi; \
	done; \
	if [ $$needs_build_so -eq 1 ]; then \
		printf "  %-7s %s\n" "LD" "$(SUBDIR)/libc.so" && \
		$(LD) $(LDFLAGS) -shared $$OBJ -o ../../libc.so -Wl,-soname,libc.so && \
		printf "  %-7s %s\n" "STRIP" "$(SUBDIR)/libc.so" && \
		$(STRIP) $(STRIPFLAGS) ../../libc.so; \
	fi; \
	if [ $$needs_build_a -eq 1 ]; then \
		printf "  %-7s %s\n" "AR" "$(SUBDIR)/libc.a" && \
		$(AR) $(ARFLAGS) rcs ../../libc.a $$OBJ && \
		printf "  %-7s %s\n" "STRIP" "$(SUBDIR)/libc.a" && \
		$(STRIP) $(STRIPFLAGS) --strip-debug ../../libc.a; \
	fi; \
	if [ $$needs_build_so -eq 0 ] && [ $$needs_build_a -eq 0 ]; then \
		printf "  %-7s %s\n" "INFO" "$(SUBDIR)/libc.so $(SUBDIR)/libc.a are already up to date"; \
	fi

clean:
	@for dir in $(SUBDIRS); do \
		$(MAKE) -C $$dir clean; \
	done
	@printf "  %-7s %s\n" "CLEAN" "$(SUBDIR)/libc.so $(SUBDIR)/libc.a"
	@rm -f ../../libc.so ../../libc.a

mrproper: clean

.PHONY: all libc $(SUBDIRS) clean mrproper
