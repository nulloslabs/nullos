# This keeps debug info when compiling, and skips stripping.
DEBUG := 1
KERNEL := $(shell uname -s)
ARCH := $(shell uname -m)

ifeq ($(KERNEL)-$(ARCH),Linux-x86_64)
	CC = gcc
else ifeq ($(KERNEL),Darwin)
	CC = x86_64-elf-gcc
else
	CC = x86_64-linux-gnu-gcc
endif

CFLAGS = -Wall -m64 -I../include/ -ffreestanding -nostdlib -nostdinc -fno-builtin -nodefaultlibs -nostartfiles -fno-stack-protector -fno-pic -fno-pie -no-pie -fno-lto -fno-stack-check -mno-red-zone -mcmodel=kernel -mno-red-zone -mcmodel=kernel -mno-80387 -mno-mmx -mabi=sysv -MMD -MP -std=c99 -mfpmath=sse -march=x86-64 -mtune=generic
ifeq ($(DEBUG),1)
	CFLAGS := -g $(CFLAGS)
endif

# No need for if checks here...
AS = $(CC)
AFLAGS = $(CFLAGS) -D__ASSEMBLY__

ifeq ($(KERNEL)-$(ARCH),Linux-x86_64)
	LD = ld
else ifeq ($(KERNEL),Darwin)
	LD = x86_64-elf-ld
else
	LD = x86_64-linux-gnu-ld
endif

LDFLAGS = -melf_x86_64 -T linker.ld

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
KERNELFILE = nullkrnl
SUBDIR = kernel

undefine KERNEL
undefine ARCH
