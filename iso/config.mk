XORRISO = xorriso
XORRISOFLAGS = -R -r -J -b boot/limine-bios-cd.bin -no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus -apm-block-size 2048 --efi-boot boot/limine-uefi-cd.bin -efi-boot-part --efi-boot-image --protective-msdos-label
SUBDIR = iso
ISOFILE = system.iso
