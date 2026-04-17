#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <dirent.h>
#include <string.h>

#define ROTR(x, n) ((x >> n) | (x << (32 - n)))
#define SHFR(x, n) (x >> n)
#define CH(x, y, z)  ((x & y) ^ (~x & z))
#define MAJ(x, y, z) ((x & y) ^ (x & z) ^ (y & z))
#define F1(x) (ROTR(x, 2)  ^ ROTR(x, 13) ^ ROTR(x, 22))
#define F2(x) (ROTR(x, 6)  ^ ROTR(x, 11) ^ ROTR(x, 25))
#define F3(x) (ROTR(x, 7)  ^ ROTR(x, 18) ^ SHFR(x, 3))
#define F4(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ SHFR(x, 10))

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static void hash_sha256(const char *src, char out[65]) {
    uint32_t h[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };

    size_t len = strlen(src);
    const uint8_t *data = (const uint8_t *)src;
    uint64_t bitlen = (uint64_t)len * 8;
    size_t offset = 0;
    int end = 0, extra = 0;

    while (!end) {
        uint8_t chunk[64];
        memset(chunk, 0, 64);
        size_t rem = len - offset;

        if (rem >= 64) {
            memcpy(chunk, data + offset, 64);
            offset += 64;
        } else {
            if (!extra) {
                if (rem > 0) memcpy(chunk, data + offset, rem);
                chunk[rem] = 0x80;
                if (rem < 56) {
                    for (int i = 0; i < 8; i++)
                        chunk[63 - i] = (uint8_t)(bitlen >> (i * 8));
                    end = 1;
                } else {
                    extra = 1;
                }
                offset = len;
            } else {
                for (int i = 0; i < 8; i++)
                    chunk[63 - i] = (uint8_t)(bitlen >> (i * 8));
                end = 1;
            }
        }

        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = ((uint32_t)chunk[i*4]   << 24) |
                   ((uint32_t)chunk[i*4+1] << 16) |
                   ((uint32_t)chunk[i*4+2] <<  8) |
                   ((uint32_t)chunk[i*4+3]);
        for (int i = 16; i < 64; i++)
            w[i] = F4(w[i-2]) + w[i-7] + F3(w[i-15]) + w[i-16];

        uint32_t v[8];
        memcpy(v, h, sizeof(h));

        for (int i = 0; i < 64; i++) {
            uint32_t t1 = v[7] + F2(v[4]) + CH(v[4], v[5], v[6]) + K[i] + w[i];
            uint32_t t2 = F1(v[0]) + MAJ(v[0], v[1], v[2]);
            v[7] = v[6]; v[6] = v[5]; v[5] = v[4]; v[4] = v[3] + t1;
            v[3] = v[2]; v[2] = v[1]; v[1] = v[0]; v[0] = t1 + t2;
        }

        for (int i = 0; i < 8; i++) h[i] += v[i];
    }

    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 4; j++) {
            uint8_t byte = (uint8_t)(h[i] >> ((3 - j) * 8));
            out[(i * 8) + (j * 2)]     = hex[(byte >> 4) & 0xF];
            out[(i * 8) + (j * 2) + 1] = hex[byte & 0xF];
        }
    }
    out[64] = '\0';
}

int main(int argc, char **argv, char **envp) {
    for (;;) {
        char buf[256];
        char hashed_buf[65];
        printf("Please enter a password to hash to SHA256.\n");
        printf("> ");
        int n = read(0, buf, sizeof(buf) - 1);
        if (n > 0) buf[n] = '\0';
        hash_sha256(buf, hashed_buf);
        printf("Hashed password is: '%s'\n", hashed_buf);
    }
    return 0;
}