# NullOS

A secure, fast and privacy-focused hobby OS.

[![Discord](https://img.shields.io/discord/1512751094583791726?color=5865F2&label=NullOS%20Labs&logo=discord&logoColor=white)](https://discord.gg/TMrw9rzfvx)

## ❤️ Credits

| Project | Purpose |
| :--- | :--- |
| [uACPI](https://github.com/uACPI/uACPI) | ACPI library for kernel |

## ✨ Features

| Status | Feature |
| :--- | :--- |
| Done | AC'97 support |
| Done | ACPI support |
| Done | ATAPI, PATA, SATA and NVMe support |
| Done | ext2/3/4, FAT16/32 and ISO9660 support |
| Done | E1000, RTL8139 and networking stack support |
| Done | ELF executable support |
| Done | Framebuffer, BGA, SVGA II, virtio-gpu support |
| Done | gzip decompression support |
| Done | IDE and AHCI support |
| Done | Initrd support |
| Done | Modular font system |
| Done | MP support |
| Done | OHCI and UHCI USB support |
| Done | PCI(e) support |
| Done | Serial port support |
| Done | Syscall support |

## 🖥️ System Requirements

| Category | Minimum | Recommended |
| :--- | :--- | :--- |
| CPU | Any x86_64-v1 CPU | Any x86_64-v2 CPU |
| RAM | 256MiB | 1024MiB |
| Display | 640x480x15 | 800x600x32 |

## 🛠️ Build Requirements

| Linux (x86_64) | macOS (any) | Other (any) | Notes |
| :--- | :--- | :--- | :--- |
| cc | cc | cc | Host C compiler |
| gcc | x86_64-elf-gcc | x86_64-elf-gcc | Target C compiler |
| ld | x86_64-elf-ld | x86_64-elf-ld | |
| strip | x86_64-elf-strip | x86_64-elf-strip | |
| objcopy | objcopy | objcopy | Host objcopy |
| objcopy | x86_64-elf-objcopy | x86_64-elf-objcopy | Target objcopy |
| make | gmake | gmake | |
| xorriso | xorriso | xorriso | |
| qemu-system-x86_64 | qemu-system-x86_64 | qemu-system-x86_64 | |
| curl | curl | curl | |
| tar | tar | tar | |
| cpio | cpio | cpio | |
| gzip | gzip | gzip | |
| zstd | zstd | zstd | |
| xz | xz | xz | |

## 🚀 Getting Started

**1. Clone the repository**

```bash
# You can remove --depth=1 to clone the entire commit history (may take a while)
git clone --depth=1 https://github.com/nulloslabs/nullos.git
cd nullos
```

**2. Build the kernel and ISO**

```bash
make
```

> [!WARNING]
> Use `make` if you are using Linux, else use `gmake`.

**3. Run in QEMU**

```bash
make qemu
```

> [!NOTE]
> The root password is `nullos`.

## 📜 License

This project is licensed under the **MIT License**. See the [LICENSE](LICENSE) file for details.

## 🤝 Contributing

NullOS is maintained by NullOS Labs, a small hobby development team. Technical discussions, contributions, and bug reports are always welcome!
