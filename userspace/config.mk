DEBUG := 0
KERNEL := $(shell uname -s)
ARCH := $(shell uname -m)

ifeq ($(KERNEL)-$(ARCH),Linux-x86_64)
	CC = gcc
else ifeq ($(KERNEL),Darwin)
	CC = x86_64-elf-gcc
else
	CC = x86_64-linux-gnu-gcc
endif

CFLAGS = -Wall -m64 -fPIC -fno-builtin -nostdlib -nostdinc -nodefaultlibs -I$(shell pwd | sed 's|/main.*||')/include -L../../ -L. -MMD -MP -std=c99 -march=x86-64 -mtune=generic
ifeq ($(DEBUG),1)
	CFLAGS := -g $(CFLAGS)
endif

# No need for if checks here...
AS = $(CC)
AFLAGS = $(CFLAGS) -D__ASSEMBLY__
LD = $(CC)
LDFLAGS = $(CFLAGS) -Wl,--hash-style=both

ifeq ($(KERNEL)-$(ARCH),Linux-x86_64)
	AR = ar
else ifeq ($(KERNEL),Darwin)
	AR = x86_64-elf-ar
else
	AR = x86_64-linux-gnu-ar
endif

ARFLAGS = 

ifeq ($(KERNEL)-$(ARCH),Linux-x86_64)
	STRIP = strip
else ifeq ($(KERNEL),Darwin)
	STRIP = x86_64-elf-strip
else
	STRIP = x86_64-linux-gnu-strip
endif
ifeq ($(DEBUG),1)
	STRIP := true
endif

STRIPFLAGS = 
SUBDIR = userspace

undefine KERNEL
undefine ARCH
