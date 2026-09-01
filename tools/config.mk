DEBUG ?= 0

CC = cc
CFLAGS = -std=c11 -MMD -MP
ifeq ($(DEBUG),1)
	CFLAGS := -g $(CFLAGS)
endif
LD = $(CC)
LDFLAGS = 
LIBS = 
STRIP = strip
STRIPFLAGS = 
OBJDUMP = objdump
OBJDUMP_FLAGS = 
LIMINE_OUTFILE = limine
SUBDIR = tools


