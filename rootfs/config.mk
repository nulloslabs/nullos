KERNEL := $(shell uname -s)
ARCH := $(shell uname -m)

ifeq ($(KERNEL),Linux)
TAR = tar
TARFLAGS = -c --format=ustar --owner=0 --group=0 --numeric-owner --use-compress-program='gzip -9'
else ifeq ($(KERNEL),Darwin)
TAR = docker run --rm -v "$(PWD)":"$(PWD)" -w "$(PWD)" alpine tar
TARFLAGS = -c --format=ustar --owner=0 --group=0 --numeric-owner --use-compress-program='gzip -9'
endif

PACKAGE_NAMES := core/glibc core/ncurses core/readline extra/busybox core/bash

SUBDIR = rootfs
OUTFILE = rootfs.tar.gz
