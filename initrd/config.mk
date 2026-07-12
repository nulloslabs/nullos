CPIO = cpio
CPIOFLAGS = --null -o -H newc --owner=0:0

PACKAGE_NAMES := core/glibc core/ncurses core/readline extra/busybox core/bash core/nano core/file core/zstd core/xz core/bzip2 core/zlib

SUBDIR = initrd
OUTFILE = initrd.cpio.gz
