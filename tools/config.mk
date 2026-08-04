DEBUG := 0

CC = cc
LD = $(CC)
CFLAGS = -MMD -MP
ifeq ($(DEBUG),1)
	CFLAGS := -g $(CFLAGS)
endif
LDFLAGS = 
STRIP = strip
STRIPFLAGS = 
SUBDIR = tools
