#pragma once

#include <freestanding/stddef.h>

// Scatter/gather I/O vector (POSIX). Shared ABI definition used by both the
// kernel readv()/writev() syscall handlers and the userspace libc wrappers.
struct iovec {
    void  *iov_base;   // Starting address
    size_t iov_len;    // Number of bytes to transfer
};
