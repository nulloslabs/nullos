# This keeps debug info when compiling, and skips stripping.
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

CFLAGS = -Wall -m64 -I../include/ -I../include/freestanding/ -I../include/limine/ -I../uacpi/include/ -ffreestanding -nostdlib -nostdinc -fno-builtin -nodefaultlibs -nostartfiles -fstack-protector-strong -mstack-protector-guard=global -fno-pic -fno-pie -no-pie -fno-lto -fno-stack-check -mno-red-zone -mcmodel=kernel -mabi=sysv -MMD -MP -std=c23 -mfpmath=sse -march=x86-64 -mtune=generic
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

LDFLAGS = -melf_x86_64 -T linker.ld -L./uacpi/build/
LIBS = -luacpi

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

OUTFILE = nullkrnl
SUBDIR = kernel
