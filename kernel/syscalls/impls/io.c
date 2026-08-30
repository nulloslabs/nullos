#include <stdbool.h>
#include <signal.h>
#include <flock.h>
#include <time.h>
#include <wait.h>
#include <limits.h>
#include <poll.h>
#include <errno.h>
#include <asm/unistd.h>
#include <linux/rseq.h>
#include <linux/types.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/kd.h>
#include <sys/time.h>
#include <sys/mount.h>
#include <sys/fb.h>
#include <sys/statx.h>
#include <sys/resource.h>
#include <sys/reboot.h>
#include <sys/epoll.h>
#include <sys/sysmacros.h>
#include <main/limine_req.h>
#include <main/elf.h>
#include <main/hostname.h>
#include <main/timekeeping.h>
#include <main/mp.h>
#include <main/fd.h>
#include <main/signal.h>
#include <main/string.h>
#include <io/fb.h>
#include <io/fonts.h>
#include <io/devices.h>
#include <io/devpts.h>
#include <io/initrd.h>
#include <io/keyboard.h>
#include <io/pty.h>
#include <io/time.h>
#include <io/sockets.h>
#include <io/net.h>
#include <io/unix_sockets.h>
#include <io/procfs.h>
#include <io/ext4.h>
#include <io/vfat.h>
#include <io/mbr.h>
#include <io/gpt.h>
#include <mm/mm.h>
#include <mm/pmm.h>
#include <syscalls/syscalls.h>
#include <syscalls/impls/helpers.h>
#include <syscalls/impls/io.h>

void sys_read(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    void *buf = (void *)frame->rsi;
    size_t count = frame->rdx;

    frame->rax = do_read(fd, buf, count);
}

void sys_write(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    const void *buf = (const void *)frame->rsi;
    size_t count = (size_t)frame->rdx;

    frame->rax = do_write(fd, buf, count);
}

void sys_poll(syscall_frame_t *frame) {
    struct pollfd *user_fds = (struct pollfd *)frame->rdi;
    uint64_t nfds = (uint64_t)frame->rsi;
    int timeout = (int)frame->rdx;
    if (nfds > 1024) { frame->rax = (uint64_t)-EINVAL; return; }
    if (nfds > 0 && !user_write_range_ok(current_task_ptr->ctx, (uint64_t)user_fds, nfds * sizeof(struct pollfd))) {
        frame->rax = (uint64_t)-EFAULT; return;
    }
    struct pollfd *k_fds = NULL;
    if (nfds > 0) {
        k_fds = malloc(nfds * sizeof(struct pollfd));
        if (!k_fds) { frame->rax = (uint64_t)-ENOMEM; return; }
        if (copy_from_user(k_fds, user_fds, nfds * sizeof(struct pollfd)) < 0) {
            free(k_fds);
            frame->rax = (uint64_t)-EFAULT; return;
        }
    }

    #define EVAL_FDS(events) do { \
        (events) = 0; \
        for (uint64_t i = 0; i < nfds; i++) { \
            k_fds[i].revents = 0; \
            int fd = k_fds[i].fd; \
            if (fd < 0) continue; \
            fd_entry_t *entry = get_current_fd(fd); \
            if (!entry || !entry->open) { k_fds[i].revents |= POLLNVAL; (events)++; continue; } \
            if (k_fds[i].events & POLLIN) { \
                if (entry->type == FD_STREAM) { \
                    int tty_idx = current_task_ptr->ctty_idx >= 0 ? current_task_ptr->ctty_idx : 1; \
                    tty_t *t = get_tty(tty_idx); \
                    if (t && get_tty_ring_count(&t->input) > 0) k_fds[i].revents |= POLLIN; \
                } else if (entry->type == FD_DEV) { \
                    char rel[256]; \
                    int tty_idx = -1; \
                    if (is_devtmpfs_path(entry->path, rel)) { \
                        tty_idx = tty_rel_to_idx(rel); \
                    } else if (is_devpts_path(entry->path, rel)) { \
                        tty_idx = current_task_ptr->ctty_idx; \
                    } \
                    if (tty_idx >= 0) { \
                        tty_t *t = get_tty(tty_idx); \
                        if (t && get_tty_ring_count(&t->input) > 0) k_fds[i].revents |= POLLIN; \
                    } else { \
                        k_fds[i].revents |= POLLIN; \
                    } \
                } else if (entry->type == FD_PIPE) { \
                    unix_handle_t *h = (unix_handle_t *)entry->handle; \
                    if (h && h->in && (h->in->len > 0 || h->in->writers == 0)) k_fds[i].revents |= POLLIN; \
                } else if (entry->type == FD_SOCKET) { \
                    poll_net_device(); \
                    if (is_socket_readable((socket_t *)entry->handle)) k_fds[i].revents |= POLLIN; \
                } else { \
                    k_fds[i].revents |= POLLIN; \
                } \
            } \
            if (k_fds[i].events & POLLOUT) k_fds[i].revents |= POLLOUT; \
            if (k_fds[i].revents) (events)++; \
        } \
    } while (0)

    int events = 0;
    EVAL_FDS(events);

    if (events == 0 && timeout != 0) {
        uint64_t start_time = get_monotonic_time_us();
        uint64_t total_us = (timeout > 0) ? (uint64_t)timeout * 1000ULL : UINT64_MAX;
        while (1) {
            if (signal_pending()) {
                if (nfds > 0) free(k_fds);
                frame->rax = (uint64_t)-EINTR;
                return;
            }
            EVAL_FDS(events);
            if (events > 0) break;
            if (timeout > 0 && get_monotonic_time_us() - start_time >= total_us) break;
            yield_sched();
        }
    }

    #undef EVAL_FDS

    if (nfds > 0) {
        if (copy_to_user((void*)user_fds, k_fds, nfds * sizeof(struct pollfd)) < 0) {
            frame->rax = (uint64_t)-EFAULT;
            free(k_fds);
            return;
        }
        free(k_fds);
    }
    frame->rax = (uint64_t)events;
}

void sys_ioctl(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    unsigned long req = (unsigned long)frame->rsi;
    uint64_t argp = frame->rdx;

    fd_entry_t *entry = get_current_fd(fd);

    if (entry && entry->type == FD_SOCKET) {
        static short interface_flags = IFF_UP | IFF_BROADCAST | IFF_RUNNING | IFF_MULTICAST;
        static int interface_mtu = 1500;
        static int interface_tx_queue_length = 1000;

        bool network_admin_request = req == SIOCADDRT || req == SIOCDELRT || req == SIOCSIFFLAGS || req == SIOCSIFADDR || req == SIOCSIFNETMASK || req == SIOCSIFBRDADDR || req == SIOCSIFMTU || req == SIOCSIFTXQLEN;
        if (network_admin_request && (!current_task_ptr || current_task_ptr->euid != 0)) {
            frame->rax = (uint64_t)-EPERM;
            return;
        }

        if (req == SIOCADDRT || req == SIOCDELRT) {
            struct rtentry route;
            if (copy_from_user(&route, (const void *)argp, sizeof(route)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
            if (req == SIOCADDRT && (route.rt_flags & RTF_GATEWAY)) memcpy(&net_gateway_ip, route.rt_gateway.sa_data + 2, sizeof(net_gateway_ip));
            if (req == SIOCDELRT) net_gateway_ip = 0;
            frame->rax = 0;
            return;
        }

        bool interface_query = req == SIOCGIFNAME || req == SIOCGIFINDEX || req == SIOCGIFHWADDR || req == SIOCGIFFLAGS || req == SIOCSIFFLAGS || req == SIOCGIFADDR || req == SIOCSIFADDR || req == SIOCGIFNETMASK || req == SIOCSIFNETMASK || req == SIOCGIFBRDADDR || req == SIOCSIFBRDADDR || req == SIOCGIFMTU || req == SIOCSIFMTU || req == SIOCGIFTXQLEN || req == SIOCSIFTXQLEN;
        if (interface_query) {
            struct ifreq ifr;
            if (copy_from_user(&ifr, (const void *)argp, sizeof(ifr)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
            ifr.ifr_name[IFNAMSIZ - 1] = '\0';
            if (req == SIOCGIFNAME) {
                if (ifr.ifr_ifindex == NET_LOOPBACK_INTERFACE_INDEX) strncpy(ifr.ifr_name, "lo", IFNAMSIZ);
                else if (ifr.ifr_ifindex == NET_ETHERNET_INTERFACE_INDEX && net_current_device) strncpy(ifr.ifr_name, "eth0", IFNAMSIZ);
                else { frame->rax = (uint64_t)-ENODEV; return; }
            } else if (strcmp(ifr.ifr_name, "lo") != 0 && (strcmp(ifr.ifr_name, "eth0") != 0 || !net_current_device)) {
                frame->rax = (uint64_t)-ENODEV;
                return;
            } else if (req == SIOCGIFINDEX) {
                ifr.ifr_ifindex = strcmp(ifr.ifr_name, "lo") == 0 ? NET_LOOPBACK_INTERFACE_INDEX : NET_ETHERNET_INTERFACE_INDEX;
            } else if (req == SIOCGIFHWADDR) {
                if (!net_current_device) { frame->rax = (uint64_t)-ENODEV; return; }
                ifr.ifr_hwaddr.sa_family = ARPHRD_ETHER;
                memcpy(ifr.ifr_hwaddr.sa_data, net_current_device->mac, 6);
            } else if (req == SIOCGIFFLAGS) {
                ifr.ifr_flags = interface_flags;
            } else if (req == SIOCSIFFLAGS) {
                interface_flags = ifr.ifr_flags;
            } else if (req == SIOCGIFADDR) {
                ifr.ifr_addr.sa_family = AF_INET;
                memcpy(ifr.ifr_addr.sa_data + 2, &net_local_ip, sizeof(net_local_ip));
            } else if (req == SIOCSIFADDR) {
                memcpy(&net_local_ip, ifr.ifr_addr.sa_data + 2, sizeof(net_local_ip));
            } else if (req == SIOCGIFNETMASK) {
                ifr.ifr_netmask.sa_family = AF_INET;
                memcpy(ifr.ifr_netmask.sa_data + 2, &net_subnet_mask, sizeof(net_subnet_mask));
            } else if (req == SIOCSIFNETMASK) {
                memcpy(&net_subnet_mask, ifr.ifr_netmask.sa_data + 2, sizeof(net_subnet_mask));
            } else if (req == SIOCGIFBRDADDR) {
                uint32_t broadcast = net_local_ip | ~net_subnet_mask;
                ifr.ifr_broadaddr.sa_family = AF_INET;
                memcpy(ifr.ifr_broadaddr.sa_data + 2, &broadcast, sizeof(broadcast));
            } else if (req == SIOCSIFBRDADDR) {
            } else if (req == SIOCGIFMTU) {
                ifr.ifr_mtu = interface_mtu;
            } else if (req == SIOCSIFMTU) {
                if (ifr.ifr_mtu < 68 || ifr.ifr_mtu > 1500) { frame->rax = (uint64_t)-EINVAL; return; }
                interface_mtu = ifr.ifr_mtu;
            } else if (req == SIOCGIFTXQLEN) {
                ifr.ifr_qlen = interface_tx_queue_length;
            } else if (req == SIOCSIFTXQLEN) {
                if (ifr.ifr_qlen < 0) { frame->rax = (uint64_t)-EINVAL; return; }
                interface_tx_queue_length = ifr.ifr_qlen;
            } else {
                frame->rax = (uint64_t)-EINVAL;
                return;
            }
            if (copy_to_user((void *)argp, &ifr, sizeof(ifr)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
            frame->rax = 0;
            return;
        }
    }

    // Handle framebuffer ioctl requests
    if (entry && entry->type == FD_DEV) {
        char rel[256];
        if (is_devtmpfs_path(entry->path, rel)) {
            if (strncmp(rel, "fb", 2) == 0) {
                int idx = rel[2] - '0';
                if (fb_req.response && idx >= 0 && idx < (int)fb_req.response->framebuffer_count) {
                    struct limine_framebuffer *fb = fb_req.response->framebuffers[idx];
                    if (req == FBIOGET_VSCREENINFO) {
                        struct fb_var_screeninfo vinfo;
                        memset(&vinfo, 0, sizeof(vinfo));
                        vinfo.xres = fb->width;
                        vinfo.yres = fb->height;
                        vinfo.xres_virtual = idx == 0 && fb_xres_virtual ? fb_xres_virtual : fb->width;
                        vinfo.yres_virtual = idx == 0 && fb_yres_virtual ? fb_yres_virtual : fb->height;
                        vinfo.xoffset = idx == 0 ? fb_xoffset : 0;
                        vinfo.yoffset = idx == 0 ? fb_yoffset : 0;
                        vinfo.bits_per_pixel = fb->bpp;
                        // Set RGB bitfield layout.  Prefer limine's mask sizes
                        // but fall back to safe defaults when they are zero
                        // (e.g. some firmware framebuffers don't fill them in).
                        if (fb->bpp == 32) {
                            if (fb->red_mask_size) {
                                vinfo.red.offset   = fb->red_mask_shift;
                                vinfo.red.length   = fb->red_mask_size;
                                vinfo.green.offset = fb->green_mask_shift;
                                vinfo.green.length = fb->green_mask_size;
                                vinfo.blue.offset  = fb->blue_mask_shift;
                                vinfo.blue.length  = fb->blue_mask_size;
                            } else {
                                // BGRA8 by default (QEMU/Bochs layout)
                                vinfo.blue.offset  = 0;  vinfo.blue.length  = 8;
                                vinfo.green.offset = 8;  vinfo.green.length = 8;
                                vinfo.red.offset   = 16; vinfo.red.length   = 8;
                            }
                        } else if (fb->bpp == 24) {
                            vinfo.red.offset   = 16; vinfo.red.length   = 8;
                            vinfo.green.offset = 8;  vinfo.green.length = 8;
                            vinfo.blue.offset  = 0;  vinfo.blue.length  = 8;
                        } else if (fb->bpp == 16) {
                            vinfo.red.offset   = 11; vinfo.red.length   = 5;
                            vinfo.green.offset = 5;  vinfo.green.length = 6;
                            vinfo.blue.offset  = 0;  vinfo.blue.length  = 5;
                        }
                        vinfo.activate = 0; // FB_ACTIVATE_NOW
                        if (copy_to_user((void *)argp, &vinfo, sizeof(vinfo)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
                        frame->rax = 0;
                        return;
                    } else if (req == FBIOGET_FSCREENINFO) {
                        struct fb_fix_screeninfo finfo;
                        memset(&finfo, 0, sizeof(finfo));
                        strncpy(finfo.id, "limine-fb", 15);
                        finfo.smem_start = virt_to_phys((void *)fb->address);
                        finfo.smem_len = (idx == 0 && fb_yres_virtual ? fb_yres_virtual : fb->height) * fb->pitch;
                        finfo.type = FB_TYPE_PACKED_PIXELS;
                        finfo.visual = FB_VISUAL_TRUECOLOR;
                        finfo.line_length = fb->pitch;
                        if (copy_to_user((void *)argp, &finfo, sizeof(finfo)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
                        frame->rax = 0;
                        return;
                    } else if (req == FBIOPUT_VSCREENINFO) {
                        if (idx != 0) { frame->rax = (uint64_t)-ENOTSUP; return; }
                        struct fb_var_screeninfo vinfo;
                        if (copy_from_user(&vinfo, (const void *)argp, sizeof(vinfo)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
                        uint64_t xres_virtual = vinfo.xres_virtual ? vinfo.xres_virtual : vinfo.xres;
                        uint64_t yres_virtual = vinfo.yres_virtual ? vinfo.yres_virtual : vinfo.yres;
                        int status = set_fb_resolution(vinfo.xres, vinfo.yres, xres_virtual, yres_virtual, vinfo.xoffset, vinfo.yoffset, (uint16_t)vinfo.bits_per_pixel);
                        if (status < 0) { frame->rax = (uint64_t)status; return; }
                        vinfo.xres = fb->width;
                        vinfo.yres = fb->height;
                        vinfo.xres_virtual = fb_xres_virtual;
                        vinfo.yres_virtual = fb_yres_virtual;
                        vinfo.xoffset = fb_xoffset;
                        vinfo.yoffset = fb_yoffset;
                        vinfo.bits_per_pixel = fb->bpp;
                        vinfo.red.offset = fb->red_mask_shift;
                        vinfo.red.length = fb->red_mask_size;
                        vinfo.green.offset = fb->green_mask_shift;
                        vinfo.green.length = fb->green_mask_size;
                        vinfo.blue.offset = fb->blue_mask_shift;
                        vinfo.blue.length = fb->blue_mask_size;
                        if (copy_to_user((void *)argp, &vinfo, sizeof(vinfo)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
                        frame->rax = 0;
                        return;
                    } else if (req == FBIOPAN_DISPLAY) {
                        // Pan/offset — accept as no-op (no virtual screen pan).
                        frame->rax = 0;
                        return;
                    }
                }
            }
        }
    }

    int is_tty = (fd == 0 || fd == 1 || fd == 2);

    // Also treat devtmpfs tty devices as ttys
    if (!is_tty) {
        if (entry && entry->type == FD_DEV) {
            char rel[256];
            if (is_devtmpfs_path(entry->path, rel)) {
                if (strncmp(rel, "tty", 3) == 0 || strncmp(rel, "pts/", 4) == 0 || strcmp(rel, "console") == 0) is_tty = 1;
            } else if (is_devpts_path(entry->path, rel)) {
                is_tty = 1;
            }
        }
    }

    switch (req) {
        case HDIO_GETGEO: {
            if (!entry || entry->type != FD_DEV) { frame->rax = (uint64_t)-ENOTTY; return; }
            uint64_t size;
            if (get_block_device_size(entry->path, &size) < 0) { frame->rax = (uint64_t)-EINVAL; return; }
            struct hd_geometry geo;
            geo.heads = 255;
            geo.sectors = 63;
            uint64_t sectors = size / 512;
            uint64_t cyl = sectors / (255 * 63);
            if (cyl > 65535) cyl = 65535;
            geo.cylinders = (unsigned short)cyl;
            geo.start = 0;
            if (copy_to_user((void *)argp, &geo, sizeof(geo)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
            frame->rax = 0;
            return;
        }

        case BLKRRPART: {
            if (!current_task_ptr || current_task_ptr->euid != 0) { frame->rax = (uint64_t)-EPERM; return; }
            if (!entry || entry->type != FD_DEV) { frame->rax = (uint64_t)-ENOTTY; return; }
            char rel[256];
            if (!is_devtmpfs_path(entry->path, rel)) { frame->rax = (uint64_t)-ENOTTY; return; }
            uint64_t blk_size;
            if (get_block_device_size(rel, &blk_size) < 0) { frame->rax = (uint64_t)-EINVAL; return; }
            disk_device_bus_t bus;
            int disk_index;
            if (get_block_device_bus(rel, &bus, &disk_index) < 0) { frame->rax = (uint64_t)-EINVAL; return; }
            if (bus == DISK_BUS_PATA) {
                remove_mbr_partitions(disk_index, DISK_BUS_PATA);
                remove_gpt_partitions(disk_index, DISK_BUS_PATA);
                if (!probe_gpt_for_pata_disk(disk_index, rel, blk_size)) probe_mbr_for_pata_disk(disk_index, rel, blk_size);
            } else if (bus == DISK_BUS_SATA) {
                remove_mbr_partitions(disk_index, DISK_BUS_SATA);
                remove_gpt_partitions(disk_index, DISK_BUS_SATA);
                if (!probe_gpt_for_sata_disk(disk_index, rel, blk_size)) probe_mbr_for_sata_disk(disk_index, rel, blk_size);
            } else if (bus == DISK_BUS_NVME) {
                remove_mbr_partitions(disk_index, DISK_BUS_NVME);
                remove_gpt_partitions(disk_index, DISK_BUS_NVME);
                if (!probe_gpt_for_nvme_disk(disk_index, rel, blk_size)) probe_mbr_for_nvme_disk(disk_index, rel, blk_size);
            } else {
                frame->rax = (uint64_t)-EINVAL;
                return;
            }
            frame->rax = 0;
            return;
        }

        case BLKGETSIZE: {
            if (!entry || entry->type != FD_DEV) { frame->rax = (uint64_t)-ENOTTY; return; }
            uint64_t size;
            if (get_block_device_size(entry->path, &size) < 0) { frame->rax = (uint64_t)-EINVAL; return; }
            unsigned long sectors = (unsigned long)(size / 512);
            if (copy_to_user((void *)argp, &sectors, sizeof(sectors)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
            frame->rax = 0;
            return;
        }

        case BLKFLSBUF: {
            frame->rax = 0;
            return;
        }

        case BLKSSZGET: {
            if (!entry || entry->type != FD_DEV) { frame->rax = (uint64_t)-ENOTTY; return; }
            int sector_size = 512;
            if (copy_to_user((void *)argp, &sector_size, sizeof(sector_size)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
            frame->rax = 0;
            return;
        }

        case KDGKBENT: {
            struct kbentry entry_map;
            if (!is_tty) { frame->rax = (uint64_t)-ENOTTY; return; }
            if (copy_from_user(&entry_map, (const void *)argp, sizeof(entry_map)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
            entry_map.kb_value = get_tty_keymap(entry_map.kb_table, entry_map.kb_index);
            if (copy_to_user((void *)argp, &entry_map, sizeof(entry_map)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
            frame->rax = 0;
            return;
        }

        case KDSKBENT: {
            struct kbentry entry_map;
            if (!is_tty) { frame->rax = (uint64_t)-ENOTTY; return; }
            if (!current_task_ptr || current_task_ptr->euid != 0) { frame->rax = (uint64_t)-EPERM; return; }
            if (copy_from_user(&entry_map, (const void *)argp, sizeof(entry_map)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
            if (set_tty_keymap(entry_map.kb_table, entry_map.kb_index, entry_map.kb_value) < 0) { frame->rax = (uint64_t)-EINVAL; return; }
            frame->rax = 0;
            return;
        }

        case KDSKBMODE: {
            int idx = ioctl_tty_idx(entry);
            if (!is_tty || idx < 0 || idx >= NUM_TTYS) { frame->rax = (uint64_t)-ENOTTY; return; }
            if (argp > K_OFF) { frame->rax = (uint64_t)-EINVAL; return; }
            ttys[idx].kb_mode = (int)argp;
            frame->rax = 0;
            return;
        }

        case KDGKBMODE: {
            int idx = ioctl_tty_idx(entry);
            if (!is_tty || idx < 0 || idx >= NUM_TTYS) { frame->rax = (uint64_t)-ENOTTY; return; }
            if (copy_to_user((void *)argp, &ttys[idx].kb_mode, sizeof(int)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
            frame->rax = 0;
            return;
        }

        case KDFONTOP: {
            int idx = ioctl_tty_idx(entry);
            if (!is_tty || idx < 0 || idx >= 100) { frame->rax = (uint64_t)-ENOTTY; return; }
            if (!current_task_ptr || current_task_ptr->euid != 0) { frame->rax = (uint64_t)-EPERM; return; }
            struct console_font_op font_op;
            if (copy_from_user(&font_op, (const void *)argp, sizeof(font_op)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
            if (font_op.op != KD_FONT_OP_SET || (font_op.flags & ~KD_FONT_FLAG_DONT_RECALC) != 0 || font_op.width != 8 || font_op.height == 0 || font_op.height > 32 || font_op.charcount != 256 || !font_op.data) { frame->rax = (uint64_t)-EINVAL; return; }
            // Validate entire user buffer (256 glyphs * 32 bytes stride) is accessible
            if (!user_range_ok(current_task_ptr->ctx, (uint64_t)font_op.data, 256ULL * 32ULL)) { frame->rax = (uint64_t)-EFAULT; return; }
            uint64_t font_size = 256ULL * font_op.height;
            unsigned char *font_data = malloc(font_size);
            if (!font_data) { frame->rax = (uint64_t)-ENOMEM; return; }
            for (uint64_t glyph = 0; glyph < 256; glyph++) {
                uint64_t user_glyph = (uint64_t)font_op.data + glyph * 32ULL;
                if (copy_from_user(font_data + glyph * font_op.height, (const void *)user_glyph, font_op.height) < 0) { free(font_data); frame->rax = (uint64_t)-EFAULT; return; }
            }
            int status = change_font_data(font_data, 8, (uint8_t)font_op.height);
            free(font_data);
            frame->rax = (uint64_t)status;
            return;
        }

        case TCGETS: {
            struct termios t = {0};
            int idx = ioctl_tty_idx(entry);
            if (idx < 0) { frame->rax = (uint64_t)-ENOTTY; return; }
            if (idx >= 100) {
                pty_t *pty_tcgets = get_pty(idx - 100);
                if (!pty_tcgets || !pty_tcgets->allocated) { frame->rax = (uint64_t)-ENOTTY; return; }
                t = pty_tcgets->termios;
            } else {
                tty_t *tty_tcgets = get_tty(idx);
                if (!tty_tcgets) { frame->rax = (uint64_t)-ENOTTY; return; }
                t = tty_tcgets->termios;
            }
            if (copy_to_user((void *)argp, &t, sizeof(t)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
            frame->rax = 0;
            return;
        }

        case TCSETS:
        case TCSETSW:
        case TCSETSF: {
            int idx = ioctl_tty_idx(entry);
            if (idx < 0) { frame->rax = (uint64_t)-ENOTTY; return; }
            if (idx >= 100) {
                pty_t *pty_tcsets = get_pty(idx - 100);
                if (!pty_tcsets || !pty_tcsets->allocated) { frame->rax = (uint64_t)-ENOTTY; return; }
                bool pty_was_icanon = !!(pty_tcsets->termios.c_lflag & ICANON);
                if (copy_from_user(&pty_tcsets->termios, (const void *)argp, sizeof(struct termios)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
                bool pty_now_icanon = !!(pty_tcsets->termios.c_lflag & ICANON);
                if ((req == TCSETSF) || (pty_was_icanon && !pty_now_icanon)) { pty_tcsets->m2s.head = pty_tcsets->m2s.tail = 0; }
            } else {
                tty_t *tty_tcsets = get_tty(idx);
                if (!tty_tcsets) { frame->rax = (uint64_t)-ENOTTY; return; }
                bool tty_was_icanon = !!(tty_tcsets->termios.c_lflag & ICANON);
                if (copy_from_user(&tty_tcsets->termios, (const void *)argp, sizeof(struct termios)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
                bool tty_now_icanon = !!(tty_tcsets->termios.c_lflag & ICANON);
                if ((req == TCSETSF) || (tty_was_icanon && !tty_now_icanon)) {
                    uint64_t irq;
                    spin_lock_irqsave(&tty_lock, &irq);
                    tty_tcsets->input.head = tty_tcsets->input.tail = 0;
                    spin_unlock_irqrestore(&tty_lock, irq);
                    spin_lock_irqsave(&stdin_lock, &irq);
                    current_task_ptr->stdin_buf_len = 0;
                    current_task_ptr->stdin_buf_pos = 0;
                    spin_unlock_irqrestore(&stdin_lock, irq);
                }
            }
            frame->rax = 0;
            return;
        }

        case TIOCGWINSZ: {
            winsize_t ws = { .ws_row = 25, .ws_col = 80, .ws_xpixel = 0, .ws_ypixel = 0 };
            if (fb_req.response && fb_req.response->framebuffer_count > 0) {
                struct limine_framebuffer *fb = fb_req.response->framebuffers[0];
                ws.ws_xpixel = (uint16_t)fb->width;
                ws.ws_ypixel = (uint16_t)fb->height;
                if (current_font_w > 0 && current_font_h > 0) {
                    ws.ws_col = (uint16_t)(fb->width / current_font_w);
                    ws.ws_row = (uint16_t)(fb->height / current_font_h);
                }
            }
            if (copy_to_user((void *)argp, &ws, sizeof(ws)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
            frame->rax = 0;
            return;
        }

        case TIOCSWINSZ:
            frame->rax = 0;
            return;

        case TCSBRK:
        case TCXONC:
            frame->rax = 0;
            return;

        case TCFLSH:
            if (entry && entry->type == FD_DEV) {
                char rel[256];
                if (is_devtmpfs_path(entry->path, rel)) {
                    int idx = tty_rel_to_idx(rel);
                    if (idx >= 0 && idx < NUM_TTYS) {
                        if (argp == 0 || argp == 2) { get_tty(idx)->input.head = get_tty(idx)->input.tail = 0; }
                    }
                } else if (is_devpts_path(entry->path, rel)) {
                    int idx = 0;
                    const char *p = rel;
                    while (*p >= '0' && *p <= '9') { idx = idx * 10 + (*p - '0'); p++; }
                    if (idx >= 0 && idx < NUM_PTYS) {
                        if (argp == 0 || argp == 2) { }
                    }
                }
            }
            frame->rax = 0;
            return;

        case TIOCGPGRP: {
            int idx = ioctl_tty_idx(entry);
            if (idx < 0) { frame->rax = (uint64_t)-ENOTTY; return; }
            pid_t pgrp = current_task_ptr->pgid;
            if (idx >= 100) {
                pty_t *p = get_pty(idx - 100);
                if (p && p->fg_pgrp > 0) pgrp = p->fg_pgrp;
            } else {
                tty_t *tty_fg = get_tty(idx);
                if (tty_fg && tty_fg->fg_pgrp > 0) pgrp = tty_fg->fg_pgrp;
            }
            if (argp) {
                pid_t pgrp_val = pgrp;
                if (copy_to_user((void *)argp, &pgrp_val, sizeof(pid_t)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
            }
            frame->rax = 0;
            return;
        }

        case TIOCEXCL:
        case TIOCNXCL:
            frame->rax = 0;
            return;

        case TIOCSCTTY: {
            int tidx = -1;
            if (entry) {
                if (entry->type == FD_STREAM) {
                    tidx = 0;
                } else if (entry->type == FD_DEV) {
                    char rel[256];
                    if (is_devtmpfs_path(entry->path, rel)) {
                        tidx = tty_rel_to_idx(rel);
                        if (tidx < 0 && strncmp(rel, "pts/", 4) == 0) {
                            int pidx = pty_rel_to_idx(rel + 4);
                            if (pidx >= 0) tidx = 100 + pidx;
                        }
                    } else if (is_devpts_path(entry->path, rel)) {
                        int pidx = pty_rel_to_idx(rel);
                        if (pidx >= 0) tidx = 100 + pidx;
                    }
                }
            }
            if (tidx < 0) { frame->rax = (uint64_t)-ENOTTY; return; }
            if (tidx >= 100) {
                pty_t *pty_ct = get_pty(tidx - 100);
                if (!pty_ct || !pty_ct->allocated) { frame->rax = (uint64_t)-ENOTTY; return; }
                current_task_ptr->ctty_idx = tidx;
                pty_ct->fg_pgrp = current_task_ptr->pgid;
            } else {
                tty_t *tty_ct = get_tty(tidx);
                if (!tty_ct) { frame->rax = (uint64_t)-ENOTTY; return; }
                current_task_ptr->ctty_idx = tidx;
                tty_ct->fg_pgrp = current_task_ptr->pgid;
                uint64_t irq;
                spin_lock_irqsave(&tty_lock, &irq);
                tty_ct->input.head = tty_ct->input.tail = 0;
                spin_unlock_irqrestore(&tty_lock, irq);
            }
            frame->rax = 0;
            return;
        }

        case TIOCSPGRP: {
            pid_t new_pgrp = 0;
            if (copy_from_user(&new_pgrp, (const void *)argp, sizeof(pid_t)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
            int idx = ioctl_tty_idx(entry);
            if (idx < 0) { frame->rax = (uint64_t)-ENOTTY; return; }
            if (idx >= 100) {
                pty_t *p = get_pty(idx - 100);
                if (!p) { frame->rax = (uint64_t)-ENOTTY; return; }
                p->fg_pgrp = new_pgrp;
            } else {
                tty_t *tty_sp = get_tty(idx);
                if (!tty_sp) { frame->rax = (uint64_t)-ENOTTY; return; }
                tty_sp->fg_pgrp = new_pgrp;
            }
            frame->rax = 0;
            return;
        }

        case FIONREAD: {
            uint64_t irq;
            spin_lock_irqsave(&stdin_lock, &irq);
            int avail = (fd == 0) ? (current_task_ptr->stdin_buf_len - current_task_ptr->stdin_buf_pos) : 0;
            spin_unlock_irqrestore(&stdin_lock, irq);
            if (copy_to_user((void *)argp, &avail, sizeof(int)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
            frame->rax = 0;
            return;
        }

        case TIOCNOTTY:
            current_task_ptr->ctty_idx = -1;
            frame->rax = 0;
            return;

        case TIOCGSID: {
            pid_t sid = current_task_ptr->sid;
            if (copy_to_user((void *)argp, &sid, sizeof(sid)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
            frame->rax = 0;
            return;
        }

        case TIOCSPTLCK: {
            if (!entry || entry->type != FD_PTY_MASTER) { frame->rax = (uint64_t)-ENOTTY; return; }
            int idx = ptm_path_idx(entry->path);
            pty_t *p = get_pty(idx);
            if (!p) { frame->rax = (uint64_t)-EBADF; return; }
            int val = 0;
            if (copy_from_user(&val, (const void *)argp, sizeof(int)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
            p->locked = (val != 0);
            frame->rax = 0;
            return;
        }

        case TCSETS2:
        case TCSETSW2:
        case TCSETSF2: {
            struct termios2 t2;
            int idx = ioctl_tty_idx(entry);
            if (idx < 0) { frame->rax = (uint64_t)-ENOTTY; return; }
            if (copy_from_user(&t2, (const void *)argp, sizeof(t2)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
            if (idx >= 100) {
                pty_t *pty_tcsets = get_pty(idx - 100);
                if (!pty_tcsets || !pty_tcsets->allocated) { frame->rax = (uint64_t)-ENOTTY; return; }
                pty_tcsets->termios.c_iflag = t2.c_iflag;
                pty_tcsets->termios.c_oflag = t2.c_oflag;
                pty_tcsets->termios.c_cflag = t2.c_cflag;
                pty_tcsets->termios.c_lflag = t2.c_lflag;
                pty_tcsets->termios.c_line = t2.c_line;
                for (int i = 0; i < NCCS2; i++) pty_tcsets->termios.c_cc[i] = t2.c_cc[i];
                pty_tcsets->termios.c_ispeed = t2.c_ispeed;
                pty_tcsets->termios.c_ospeed = t2.c_ospeed;
            } else {
                tty_t *tty_tcsets = get_tty(idx);
                if (!tty_tcsets) { frame->rax = (uint64_t)-ENOTTY; return; }
                tty_tcsets->termios.c_iflag = t2.c_iflag;
                tty_tcsets->termios.c_oflag = t2.c_oflag;
                tty_tcsets->termios.c_cflag = t2.c_cflag;
                tty_tcsets->termios.c_lflag = t2.c_lflag;
                tty_tcsets->termios.c_line = t2.c_line;
                for (int i = 0; i < NCCS2; i++) tty_tcsets->termios.c_cc[i] = t2.c_cc[i];
                tty_tcsets->termios.c_ispeed = t2.c_ispeed;
                tty_tcsets->termios.c_ospeed = t2.c_ospeed;
            }
            frame->rax = 0;
            return;
        }

        case TIOCGPTN: {
            if (!entry || entry->type != FD_PTY_MASTER) { frame->rax = (uint64_t)-ENOTTY; return; }
            int idx = ptm_path_idx(entry->path);
            unsigned int uidx = (unsigned int)idx;
            if (copy_to_user((void *)argp, &uidx, sizeof(unsigned int)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
            frame->rax = 0;
            return;
        }

        case BLKGETSIZE64: {
            if (!entry || entry->type != FD_DEV) { frame->rax = (uint64_t)-ENOTTY; return; }
            uint64_t size;
            if (get_block_device_size(entry->path, &size) < 0) { frame->rax = (uint64_t)-EINVAL; return; }
            if (copy_to_user((void *)argp, &size, sizeof(size)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
            frame->rax = 0;
            return;
        }

        case TCGETS2: {
            struct termios t = {0};
            struct termios2 t2 = {0};
            int idx = ioctl_tty_idx(entry);
            if (idx < 0) { frame->rax = (uint64_t)-ENOTTY; return; }
            if (idx >= 100) {
                pty_t *pty_tcgets = get_pty(idx - 100);
                if (!pty_tcgets || !pty_tcgets->allocated) { frame->rax = (uint64_t)-ENOTTY; return; }
                t = pty_tcgets->termios;
            } else {
                tty_t *tty_tcgets = get_tty(idx);
                if (!tty_tcgets) { frame->rax = (uint64_t)-ENOTTY; return; }
                t = tty_tcgets->termios;
            }
            t2.c_iflag = t.c_iflag;
            t2.c_oflag = t.c_oflag;
            t2.c_cflag = t.c_cflag;
            t2.c_lflag = t.c_lflag;
            t2.c_line = t.c_line;
            for (int i = 0; i < NCCS2; i++) t2.c_cc[i] = t.c_cc[i];
            t2.c_ispeed = t.c_ispeed;
            t2.c_ospeed = t.c_ospeed;
            if (copy_to_user((void *)argp, &t2, sizeof(t2)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
            frame->rax = 0;
            return;
        }

        default:
            frame->rax = (uint64_t)-EINVAL;
            return;
    }
}

void sys_select(syscall_frame_t *frame) {
    int nfds = (int)frame->rdi;
    uint64_t *readfds   = (uint64_t *)frame->rsi;
    uint64_t *writefds  = (uint64_t *)frame->rdx;
    uint64_t *exceptfds = (uint64_t *)frame->r10;
    struct timeval *timeout_ptr = (struct timeval *)frame->r8;

    if (nfds < 0 || nfds > FD_MAX) { frame->rax = (uint64_t)-EINVAL; return; }

    if (timeout_ptr) {
        if (!user_range_ok(current_task_ptr->ctx, (uint64_t)timeout_ptr, sizeof(struct timeval))) { frame->rax = (uint64_t)-EFAULT; return; }
    }
    if (nfds > 0) {
        int bytes = ((nfds + 63) / 64) * 8;
        if (readfds   && !user_write_range_ok(current_task_ptr->ctx, (uint64_t)readfds,   bytes)) { frame->rax = (uint64_t)-EFAULT; return; }
        if (writefds  && !user_write_range_ok(current_task_ptr->ctx, (uint64_t)writefds,  bytes)) { frame->rax = (uint64_t)-EFAULT; return; }
        if (exceptfds && !user_write_range_ok(current_task_ptr->ctx, (uint64_t)exceptfds, bytes)) { frame->rax = (uint64_t)-EFAULT; return; }
    }

    int64_t timeout_us = -1;
    if (timeout_ptr) {
        struct timeval tv;
        uint64_t converted;
        if (copy_from_user(&tv, timeout_ptr, sizeof(tv)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
        if (timeval_to_us(&tv, &converted) < 0 || converted > INT64_MAX) { frame->rax = (uint64_t)-EINVAL; return; }
        timeout_us = (int64_t)converted;
    } else {
        timeout_us = -1; // infinite
    }

    int set_bytes = nfds > 0 ? ((nfds + 7) / 8) : 0;
    int qword_bytes = nfds > 0 ? ((nfds + 63) / 64) * 8 : 0;

    uint8_t *k_read = NULL, *k_write = NULL, *k_except = NULL;
    uint8_t *o_read = NULL, *o_write = NULL, *o_except = NULL;

    if (set_bytes > 0) {
        k_read   = malloc(qword_bytes);
        k_write  = malloc(qword_bytes);
        k_except = malloc(qword_bytes);
        o_read   = malloc(qword_bytes);
        o_write  = malloc(qword_bytes);
        o_except = malloc(qword_bytes);
        if (!k_read || !k_write || !k_except || !o_read || !o_write || !o_except) {
            free(k_read); free(k_write); free(k_except);
            free(o_read); free(o_write); free(o_except);
            frame->rax = (uint64_t)-ENOMEM; return;
        }
        memset(k_read,   0, qword_bytes);
        memset(k_write,  0, qword_bytes);
        memset(k_except, 0, qword_bytes);
        memset(o_read,   0, qword_bytes);
        memset(o_write,  0, qword_bytes);
        memset(o_except, 0, qword_bytes);
        if (readfds)   copy_from_user(k_read,   readfds,   set_bytes);
        if (writefds)  copy_from_user(k_write,  writefds,  set_bytes);
        if (exceptfds) copy_from_user(k_except, exceptfds, set_bytes);
    }

    int64_t ret = do_select(nfds, k_read, k_write, k_except, o_read, o_write, o_except, qword_bytes, timeout_us);

    if (ret >= 0) {
        if (readfds   && set_bytes > 0 && write_vmm(current_task_ptr->ctx, (uint64_t)readfds,   o_read,   set_bytes) < 0) ret = -EFAULT;
        if (ret >= 0 && writefds  && set_bytes > 0 && write_vmm(current_task_ptr->ctx, (uint64_t)writefds,  o_write,  set_bytes) < 0) ret = -EFAULT;
        if (ret >= 0 && exceptfds && set_bytes > 0 && write_vmm(current_task_ptr->ctx, (uint64_t)exceptfds, o_except, set_bytes) < 0) ret = -EFAULT;
    }

    free(k_read); free(k_write); free(k_except);
    free(o_read); free(o_write); free(o_except);

    frame->rax = (uint64_t)ret;
}

void sys_dup(syscall_frame_t *frame) {
    int oldfd = (int)frame->rdi;

    fd_entry_t *src = get_current_fd(oldfd);
    if (!src) { frame->rax = (uint64_t)-EBADF; return; }

    // Find the lowest free fd
    fd_table_t *table = &current_task_ptr->fd_table;
    for (int i = 0; i < FD_MAX; i++) {
        if (!table->entries[i]) {
            fd_entry_t *e = malloc(sizeof(*e));
            if (!e) { frame->rax = (uint64_t)-ENOMEM; return; }
            *e = *src;
            e->open = true;
            table->entries[i] = e;
            retain_fd_entry(e);
            frame->rax = (uint64_t)i;
            return;
        }
    }
    frame->rax = (uint64_t)-EMFILE;
}

void sys_dup2(syscall_frame_t *frame) {
    int oldfd = (int)frame->rdi;
    int newfd = (int)frame->rsi;

    if (newfd < 0 || newfd >= FD_MAX) { frame->rax = (uint64_t)-EBADF; return; }

    fd_entry_t *src = get_current_fd(oldfd);
    if (!src) { frame->rax = (uint64_t)-EBADF; return; }

    if (oldfd == newfd) { frame->rax = (uint64_t)newfd; return; }

    fd_table_t *table = &current_task_ptr->fd_table;

    // Close newfd if it's already open
    if (table->entries[newfd]) free_fd(table, newfd);

    fd_entry_t *e = malloc(sizeof(*e));
    if (!e) { frame->rax = (uint64_t)-ENOMEM; return; }
    *e = *src;
    e->open = true;
    table->entries[newfd] = e;
    retain_fd_entry(e);
    frame->rax = (uint64_t)newfd;
}

void sys_pselect6(syscall_frame_t *frame) {
    int nfds = (int)frame->rdi;
    uint64_t *readfds   = (uint64_t *)frame->rsi;
    uint64_t *writefds  = (uint64_t *)frame->rdx;
    uint64_t *exceptfds = (uint64_t *)frame->r10;
    struct timespec *timeout_ptr = (struct timespec *)frame->r8;
    // r9 points to {sigset_t *ss, size_t ss_len} — two pointers packed
    uint64_t sigmask_arg = frame->r9;

    if (nfds < 0 || nfds > FD_MAX) { frame->rax = (uint64_t)-EINVAL; return; }

    if (timeout_ptr) {
        if (!user_range_ok(current_task_ptr->ctx, (uint64_t)timeout_ptr, sizeof(struct timespec))) { frame->rax = (uint64_t)-EFAULT; return; }
    }
    if (nfds > 0) {
        int bytes = ((nfds + 63) / 64) * 8;
        if (readfds   && !user_write_range_ok(current_task_ptr->ctx, (uint64_t)readfds,   bytes)) { frame->rax = (uint64_t)-EFAULT; return; }
        if (writefds  && !user_write_range_ok(current_task_ptr->ctx, (uint64_t)writefds,  bytes)) { frame->rax = (uint64_t)-EFAULT; return; }
        if (exceptfds && !user_write_range_ok(current_task_ptr->ctx, (uint64_t)exceptfds, bytes)) { frame->rax = (uint64_t)-EFAULT; return; }
    }

    int64_t timeout_us = -1;
    if (timeout_ptr) {
        struct timespec ts;
        if (copy_from_user(&ts, timeout_ptr, sizeof(ts)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
        if (timespec_to_us(&ts, &timeout_us) < 0) { frame->rax = (uint64_t)-EINVAL; return; }
    }

    // Atomically swap signal mask
    uint64_t old_blocked = current_task_ptr->blocked_signals;
    if (sigmask_arg) {
        // pselect6 arg is a struct {sigset_t *ss; size_t ss_len} passed by pointer
        uint64_t ss_ptr = 0;
        size_t ss_len = 0;
        if (!user_range_ok(current_task_ptr->ctx, sigmask_arg, 16)) { frame->rax = (uint64_t)-EFAULT; return; }
        if (read_vmm(current_task_ptr->ctx, &ss_ptr, sigmask_arg, 8) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
        if (read_vmm(current_task_ptr->ctx, &ss_len, sigmask_arg + 8, 8) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
        if (ss_len != 8) { frame->rax = (uint64_t)-EINVAL; return; }
        if (ss_ptr) {
            uint64_t new_mask = 0;
            if (!user_range_ok(current_task_ptr->ctx, ss_ptr, 8)) { frame->rax = (uint64_t)-EFAULT; return; }
            if (read_vmm(current_task_ptr->ctx, &new_mask, ss_ptr, 8) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
            new_mask &= ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
            current_task_ptr->blocked_signals = new_mask;
        }
    }

    int set_bytes = nfds > 0 ? ((nfds + 7) / 8) : 0;
    int qword_bytes = nfds > 0 ? ((nfds + 63) / 64) * 8 : 0;

    uint8_t *k_read = NULL, *k_write = NULL, *k_except = NULL;
    uint8_t *o_read = NULL, *o_write = NULL, *o_except = NULL;

    if (set_bytes > 0) {
        k_read   = malloc(qword_bytes);
        k_write  = malloc(qword_bytes);
        k_except = malloc(qword_bytes);
        o_read   = malloc(qword_bytes);
        o_write  = malloc(qword_bytes);
        o_except = malloc(qword_bytes);
        if (!k_read || !k_write || !k_except || !o_read || !o_write || !o_except) {
            free(k_read); free(k_write); free(k_except);
            free(o_read); free(o_write); free(o_except);
            current_task_ptr->blocked_signals = old_blocked;
            frame->rax = (uint64_t)-ENOMEM; return;
        }
        memset(k_read,   0, qword_bytes);
        memset(k_write,  0, qword_bytes);
        memset(k_except, 0, qword_bytes);
        memset(o_read,   0, qword_bytes);
        memset(o_write,  0, qword_bytes);
        memset(o_except, 0, qword_bytes);
        if (readfds)   copy_from_user(k_read,   readfds,   set_bytes);
        if (writefds)  copy_from_user(k_write,  writefds,  set_bytes);
        if (exceptfds) copy_from_user(k_except, exceptfds, set_bytes);
    }

    int64_t ret = do_select(nfds, k_read, k_write, k_except, o_read, o_write, o_except, qword_bytes, timeout_us);

    // Restore signal mask BEFORE check_signals sees any delivered signal
    current_task_ptr->blocked_signals = old_blocked;

    if (ret >= 0) {
        if (readfds   && set_bytes > 0 && write_vmm(current_task_ptr->ctx, (uint64_t)readfds,   o_read,   set_bytes) < 0) ret = -EFAULT;
        if (ret >= 0 && writefds  && set_bytes > 0 && write_vmm(current_task_ptr->ctx, (uint64_t)writefds,  o_write,  set_bytes) < 0) ret = -EFAULT;
        if (ret >= 0 && exceptfds && set_bytes > 0 && write_vmm(current_task_ptr->ctx, (uint64_t)exceptfds, o_except, set_bytes) < 0) ret = -EFAULT;
    }

    free(k_read); free(k_write); free(k_except);
    free(o_read); free(o_write); free(o_except);

    frame->rax = (uint64_t)ret;
}
