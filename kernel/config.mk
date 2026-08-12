# This keeps debug info when compiling, and skips stripping.
DEBUG := 0
KERNEL := $(shell uname -s)
ARCH := $(shell uname -m)

ifeq ($(KERNEL)-$(ARCH),Linux-x86_64)
	CROSS :=
else ifneq ($(shell command -v x86_64-elf-gcc 2>/dev/null),)
	CROSS := x86_64-elf-
else
	$(error No supported x86_64 compiler toolchain found)
endif

CC = $(CROSS)gcc
CFLAGS = -O2 -m64 -std=c23 -Wall -Wextra -I../include/ -I../include/freestanding/ -I../include/limine/ -I../uacpi/include/ -ffreestanding -nostdlib -nostdinc -fno-builtin -nodefaultlibs -nostartfiles -fstack-protector-strong -mstack-protector-guard=global -fno-pic -fno-pie -no-pie -fno-lto -fno-stack-check -mno-red-zone -mcmodel=kernel -mabi=sysv -march=x86-64 -mtune=generic -mno-sse -mno-sse2 -mno-mmx -mno-80387 -MMD -MP
UACPI_CFLAGS = $(filter-out -I../include/ -I../include/freestanding/ -I../include/limine/ -I../uacpi/include/,$(CFLAGS)) -I./include/ -I./include/freestanding/ -I./include/limine/ -I./uacpi/include
ifeq ($(DEBUG),1)
	CFLAGS := -g $(CFLAGS)
endif

AS = $(CC)
AFLAGS = $(CFLAGS) -D__ASSEMBLY__

LD = $(CROSS)ld
LDFLAGS = -melf_x86_64 -T linker.ld -L./uacpi/build/
LIBS = -luacpi

AR = $(CROSS)ar

STRIP = $(CROSS)strip
STRIPFLAGS =
UACPI_STRIPFLAGS = $(STRIPFLAGS) --strip-debug

OUTFILE = nullkrnl
UACPI_OUTFILE = uacpi/build/libuacpi.a
UACPI_BUILD = uacpi/build
SUBDIR = kernel
