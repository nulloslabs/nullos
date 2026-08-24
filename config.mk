KERNEL := $(shell uname -s)
ARCH := $(shell uname -m)

ifeq ($(ACCEL),)
	ifeq ($(KERNEL)-$(ARCH),Linux-x86_64)
		ACCEL := kvm
	else ifeq ($(KERNEL)-$(ARCH),Darwin-x86_64)
		ACCEL := hvf
	else
		ACCEL := tcg
	endif
endif

ifeq ($(AUDIODEV),)
	ifeq ($(KERNEL)-$(ARCH),Linux-x86_64)
		AUDIODEV := alsa
	else ifeq ($(KERNEL)-$(ARCH),Darwin-x86_64)
		AUDIODEV := coreaudio
	else
		AUDIODEV := none
	endif
endif

QEMU := qemu-system-x86_64
QEMUFLAGS := -M q35,accel=$(ACCEL) -smp 1 -m 1024 -serial stdio -audiodev $(AUDIODEV),id=audio0 -device ac97,audiodev=audio0 -netdev user,id=net0 -device rtl8139,netdev=net0 -vga none -device VGA,edid=on,xres=800,yres=600

ISOFILE = iso/nullos.iso

undefine ACCEL
undefine AUDIODEV
undefine ARCH
undefine KERNEL
