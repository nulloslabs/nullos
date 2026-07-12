# NullOS

A secure, fast and privacy-focused hobby OS.

[![Discord](https://img.shields.io/discord/1512751094583791726?color=5865F2&label=NullOS%20Labs&logo=discord&logoColor=white)](https://discord.gg/TMrw9rzfvx)

## ✨ Features

| Status | Feature | Notes |
| :--- | :--- | :--- |
| Done | USB support | Only UHCI. |
| Done | ACPI support |
| Done | PCI support |
| Done | Sound card support |
| Done | Initrd support |
| Done | GZIP decompression support | No compression support. |
| Done | GDT, IDT and SSE support |
| Done | Modular font system |
| Done | Serial port support |
| Done | Syscall support |
| Done | ELF executable support |
| Done | MP support |
| Done | Networking and networking card support |

## 🛠️ Build Requirements

| Linux (x86_64) | MacOS (any) | Other (any) |
| :--- | :--- | :--- |
| | `docker` | |
| `gcc` | `x86_64-elf-gcc` | `x86_64-linux-gnu-gcc` |
| `ld` | `x86_64-elf-ld` | `x86_64-linux-gnu-ld` |
| `strip` | `x86_64-elf-strip` | `x86_64-linux-gnu-strip` |
| `make` | `gmake` | `gmake` |
| `xorriso` | `xorriso` | `xorriso` |
| `qemu-system-x86_64` | `qemu-system-x86_64` | `qemu-system-x86_64` |
| `curl` | `curl` | `curl` |
| `perl` | `perl` | `perl` |
| `tar` | `tar` | `tar` |
| `cpio` | `cpio` | `cpio` |
| `zstd` | | `zstd` |
| `xz` | `xz` | `xz` |

## 🚀 Getting Started

**1. Clone the repository:**

```bash
# You can remove --depth=1 to clone the entire commit history (may take a while)
git clone --depth=1 https://github.com/asmileyguy/nullos.git
cd nullos
```

**2. Build the kernel and ISO:**

```bash
make
```

**3. Run in QEMU:**
    
```bash
make qemu
```

## 📜 License

This project is licensed under the **MIT License**. See the [LICENSE](LICENSE) file for details.

## 🤝 Contributing

NullOS is currently a solo hobby project, but technical discussions and bug reports are always welcome!
