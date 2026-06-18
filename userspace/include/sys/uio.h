#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Scatter/gather I/O vector.
struct iovec {
    void  *iov_base;   // Starting address
    size_t iov_len;    // Number of bytes to transfer
};

// Maximum number of iovec segments accepted by readv()/writev().
#define IOV_MAX 1024

ssize_t readv(int fd, const struct iovec *iov, int iovcnt);
ssize_t writev(int fd, const struct iovec *iov, int iovcnt);

#ifdef __cplusplus
}
#endif
