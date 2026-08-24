DEBUG ?= 0

KERNEL := $(shell uname -s)
ARCH := $(shell uname -m)

KERNEL_RELEASE := $(shell git rev-parse --short=7 HEAD 2>/dev/null || printf unknown)

ifeq ($(KERNEL)-$(ARCH),Linux-x86_64)
	CROSS :=
else ifneq ($(shell command -v x86_64-elf-gcc 2>/dev/null),)
	CROSS := x86_64-elf-
else
	$(error No supported x86_64 compiler toolchain found)
endif

CC = $(CROSS)gcc
CFLAGS := -O2 -m64 -std=c11 -Wall -Wextra -I$(INCLUDE_DIR)/include/ -I$(INCLUDE_DIR)/include/freestanding/ -I$(INCLUDE_DIR)/include/generated/ -I$(INCLUDE_DIR)/include/limine/ -I$(INCLUDE_DIR)/uacpi/include/ -DKERNEL_RELEASE=\"$(KERNEL_RELEASE)\" -ffreestanding -nostdlib -nostdinc -fno-builtin -nodefaultlibs -nostartfiles -fstack-protector-strong -mstack-protector-guard=global -fno-pic -fno-pie -fno-lto -fno-stack-check -mno-red-zone -mcmodel=kernel -mabi=sysv -march=x86-64 -mtune=generic -mno-sse -mno-sse2 -mno-mmx -mno-80387 -MMD -MP
ifeq ($(DEBUG),1)
	CFLAGS := -g $(CFLAGS)
endif

AS = $(CC)
AFLAGS = $(CFLAGS) -D__ASSEMBLY__

LD = $(CROSS)ld
LDFLAGS = -melf_x86_64 -T linker.ld -z max-page-size=0x200000 -no-pie -L./uacpi/build/
LIBS = -luacpi

AR = $(CROSS)ar

STRIP = $(CROSS)strip
STRIPFLAGS =

OBJDUMP = $(CROSS)objdump
OBJDUMP_FLAGS = 

OUTFILE = nullkrnl

UACPI_CFLAGS = $(filter-out -I$(INCLUDE_DIR)/include/ -I$(INCLUDE_DIR)/include/freestanding/ -I$(INCLUDE_DIR)/include/generated/ -I$(INCLUDE_DIR)/include/limine/ -I$(INCLUDE_DIR)/uacpi/include/,$(CFLAGS)) -I./include/ -I./include/freestanding/ -I./include/limine/ -I./uacpi/include
UACPI_STRIPFLAGS = $(STRIPFLAGS) --strip-debug
UACPI_OUTFILE = uacpi/build/libuacpi.a

SUBDIR = kernel

# Only top-level kernel invocation checks DEBUG mismatch; subdirs inherit via export
ifeq ($(notdir $(CURDIR)),kernel)
CHECK_OBJ := $(firstword $(wildcard main/*.o io/*.o mm/*.o syscalls/*.o))
ifneq ($(CHECK_OBJ),)
	DEBUG_VAL := $(DEBUG)
	ifneq ($(DEBUG_VAL),$(shell $(OBJDUMP) $(OBJDUMP_FLAGS) -h $(CHECK_OBJ) 2>/dev/null | grep -q '\.debug' && printf 1 || printf 0))
		FORCE_REBUILD := FORCE
	endif
endif

UACPI_CHECK_OBJ := $(firstword $(wildcard uacpi/build/*.o))
ifneq ($(UACPI_CHECK_OBJ),)
	UACPI_DEBUG_VAL := $(DEBUG)
	ifneq ($(UACPI_DEBUG_VAL),$(shell $(OBJDUMP) $(OBJDUMP_FLAGS) -h $(UACPI_CHECK_OBJ) 2>/dev/null | grep -q '\.debug' && printf 1 || printf 0))
		UACPI_FORCE_REBUILD := FORCE
	endif
endif
endif
export FORCE_REBUILD
export UACPI_FORCE_REBUILD

undefine CHECK_OBJ
undefine DEBUG_VAL
undefine UACPI_CHECK_OBJ
undefine UACPI_DEBUG_VAL
undefine KERNEL_RELEASE
undefine ARCH
undefine KERNEL
