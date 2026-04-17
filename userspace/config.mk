CC = gcc
CFLAGS = -m64 -fno-stack-protector -fPIC -nostdlib -nostdinc -nodefaultlibs -I./../../include/ -L../../ -L. -MMD -MP -std=gnu99 -march=x86-64 -mtune=generic
LD = $(CC)
LDFLAGS = $(CFLAGS)
AR = ar
ARFLAGS = 
SUBDIR = userspace
