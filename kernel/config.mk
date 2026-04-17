CC = gcc
CFLAGS = -m64 -I../include/ -ffreestanding -nostdlib -nostdinc -nodefaultlibs -nostartfiles -fno-stack-protector -fno-pic -fno-pie -no-pie -fno-lto -fno-stack-check -mno-red-zone -mcmodel=kernel -mno-red-zone -mcmodel=kernel -mno-80387 -mno-mmx -mabi=sysv -MMD -MP -std=gnu99 -mfpmath=sse -march=x86-64 -mtune=generic
AS = $(CC)
AFLAGS = $(CFLAGS) -D__ASSEMBLY__
LD = ld
LDFLAGS = -melf_x86_64 -T linker.ld
STRIP = strip
STRIPFLAGS = 
KERNELFILE = nullkrnl
SUBDIR = kernel
