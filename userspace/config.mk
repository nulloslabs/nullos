CC = gcc
CFLAGS = -Wall -m64 -fno-stack-protector -fPIC -fno-builtin -nostdlib -nostdinc -nodefaultlibs -I./../../include/ -L../../ -L. -MMD -MP -std=gnu99 -march=x86-64 -mtune=generic
AS = $(CC)
AFLAGS = $(CFLAGS) -D__ASSEMBLY__
LD = $(CC)
LDFLAGS = $(CFLAGS)
AR = ar
ARFLAGS = 
STRIP = strip
STRIPFLAGS = 
SUBDIR = userspace
