# NullOS

A secure, fast and privacy-focused hobby OS.

## ✨ Features

| Status | Feature | Notes |
| :--- | :--- | :--- |
| Half-done | USB support | Only works on QEMU. |
| Done | ACPI support |
| Done | PCI support |
| Done | Sound card support |
| Done | Rootfs support |
| Done | GZIP decompression support | No compression support. |
| Done | GDT, IDT and SSE support |
| Done | Modular font system |
| Done | Serial port support |
| Done | Syscall support |
| Done | ELF executable support |
| Done | SMP support |
| Done | Networking and networking card support |

## 🛠️ Build Requirements

| Tool | Purpose |
| :--- | :--- |
| `gcc` | Compiling kernel files and userspace files |
| `ld` | Linking kernel and userspace |
| `make` | Build automation |
| `xorriso` | ISO image creation |
| `qemu-system-x86_64` | x86_64 system emulation |

## 🚀 Getting Started

**1. Clone the repository:**

```bash
git clone https://github.com/coolguy-09/nullos.git
cd nullos
```

**2. Build the kernel and ISO:**

```bash
make
```

**3. Run in QEMU:**
    
```bash
make run
```

## 📜 License

This project is licensed under the **GNU General Public License v3.0**. See the [LICENSE](LICENSE) file for details.

## 🤝 Contributing

NullOS is currently a solo hobby project, but technical discussions and bug reports are always welcome!

---
*Developed with ❤️.*
