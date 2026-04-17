TAR = tar
TARFLAGS = -c --format=ustar --use-compress-program='gzip -9'
SUBDIR = rootfs
USERSPACE_OUTPUTS = ../userspace/login ../userspace/init ../userspace/libc.so ../userspace/libc.a ../userspace/crt0.o