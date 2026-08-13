KERNEL := $(shell uname -s)
ARCH := $(shell uname -m)

QEMU := qemu-system-x86_64

ifeq ($(KERNEL)-$(ARCH),Linux-x86_64)
	ACCEL := kvm
	AUDIODEV := alsa
else ifeq ($(KERNEL)-$(ARCH),Darwin-x86_64)
	ACCEL := hvf
	AUDIODEV := coreaudio
else
	ACCEL := tcg
	AUDIODEV := none
endif

QEMUFLAGS := -M pc,accel=$(ACCEL) -smp 1 -m 1024 -serial stdio -audiodev $(AUDIODEV),id=audio0 -device ac97,audiodev=audio0 -netdev user,id=net0 -device rtl8139,netdev=net0 -vga none -device VGA,edid=on,xres=800,yres=600

ISOFILE = iso/nullos.iso

undefine ACCEL
undefine AUDIODEV
undefine ARCH
undefine KERNEL
