QEMU = qemu-system-x86_64
QEMUFLAGS = -enable-kvm -smp 1 -m 512 -serial stdio -audiodev alsa,id=audio0 -device ac97,audiodev=audio0 -netdev user,id=net0 -device rtl8139,netdev=net0
ISOFILE = iso/system.iso
