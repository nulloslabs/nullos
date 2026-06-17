#include <io/pts_devices.h>
#include <io/ptys.h>
#include <freestanding/errno.h>

extern int get_pts_idx(const char *name);

uint64_t read_pts_device(const char* name, void* buf, uint64_t count, uint64_t offset) {
    int idx = get_pts_idx(name);
    if (idx < 0) return (uint64_t)-ENOENT;
    return read_pts(idx, buf, count, offset);
}

uint64_t write_pts_device(const char* name, const void* buf, uint64_t count, uint64_t offset) {
    int idx = get_pts_idx(name);
    if (idx < 0) return (uint64_t)-ENOENT;
    return write_pts(idx, buf, count, offset);
}
