DEBUG ?= 0

CC = cc
CFLAGS = -MMD -MP
ifeq ($(DEBUG),1)
	CFLAGS := -g $(CFLAGS)
endif
LD = $(CC)
LDFLAGS = 
STRIP = strip
STRIPFLAGS = 
OBJDUMP = objdump
OBJDUMP_FLAGS = 
LIMINE_OUTFILE = limine
SUBDIR = tools

CHECK_OBJ := $(wildcard limine-binary/*.o)
ifneq ($(CHECK_OBJ),)
	ifneq ($(DEBUG),$(shell $(OBJDUMP) $(OBJDUMP_FLAGS) -h $(CHECK_OBJ) 2>/dev/null | grep -q '\.debug' && printf 1 || printf 0))
		FORCE_REBUILD := FORCE
	endif
endif

CHECK_FILES := $(wildcard limine-binary/$(LIMINE_OUTFILE))
ifneq ($(CHECK_FILES),)
	ifneq ($(DEBUG),$(shell if $(OBJDUMP) $(OBJDUMP_FLAGS) -h $(CHECK_FILES) 2>/dev/null | grep -q '\.debug'; then printf 1; else printf 0; fi))
		FORCE_REBUILD := FORCE
	endif
endif

undefine CHECK_OBJ
undefine CHECK_FILES
