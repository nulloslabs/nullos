CPIO = cpio
CPIOFLAGS = --null -o -H newc --owner=0:0

TAR = tar
TARFLAGS = 

GZIP = gzip
GZIPFLAGS = 

ZSTD = zstd
ZSTDFLAGS = 

CURL = curl
CURLFLAGS = --retry 3 --retry-delay 2 --retry-all-errors

# TODO: Use my own packages instead of Arch packages
PACKAGE_NAMES := core/glibc core/ncurses core/readline core/bash core/nano extra/busybox

SUBDIR = initrd
OUTFILE = initrd.cpio.gz
