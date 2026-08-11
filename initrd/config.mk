CPIO = cpio
CPIOFLAGS = --null -o -H newc --owner=0:0

# TODO: Use my own packages instead of Arch packages
PACKAGE_NAMES := core/glibc core/ncurses core/readline extra/busybox core/bash core/nano

SUBDIR = initrd
OUTFILE = initrd.cpio.gz
