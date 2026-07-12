KERNEL := $(shell uname -s)
ARCH := $(shell uname -m)

QEMU = qemu-system-x86_64
QEMUFLAGS = -smp 1 -m 512 -serial stdio -audiodev alsa,id=audio0 -device ac97,audiodev=audio0 -netdev user,id=net0 -device rtl8139,netdev=net0 -vga none -device VGA,edid=on,xres=800,yres=600

ifeq ($(KERNEL)-$(ARCH),Linux-x86_64)
	QEMUFLAGS := -M accel=kvm $(QEMUFLAGS)
else ifeq ($(KERNEL)-$(ARCH),Darwin-x86_64)
	QEMUFLAGS := -M accel=hvf $(QEMUFLAGS)
else
	QEMUFLAGS := -M accel=tcg $(QEMUFLAGS)
endif

ISOFILE = iso/system.iso

undefine ARCH
undefine KERNEL
