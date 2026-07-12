#include <freestanding/stdint.h>
#include <main/rng.h>
#include <main/string.h>
#include <main/spinlocks.h>
#include <main/log.h>
#include <io/hpet.h>
#include <io/tsc.h>
#include <io/pit.h>
#include <mm/mm.h>

static uint32_t rng_state[16];
static spinlock_t rng_lock = SPINLOCK_INIT;
static bool rng_seeded = false;

void regen_rng(void) {
    // Get entropy from timers
    uint64_t hpet_entropy = read_hpet_counter();
    uint64_t tsc_entropy = read_tsc();
    uint64_t pit_entropy = (uint64_t)read_pit_counter();

    // Read it again to make completely random numbers with jitters
    hpet_entropy = read_hpet_counter() - hpet_entropy;
    tsc_entropy = read_tsc() - tsc_entropy;
    pit_entropy = (uint64_t)read_pit_counter() - pit_entropy;

    // Get entropy from registers
    uint64_t rax, rbx, rcx, rdx, rip, rsp, rbp;

    // RIP is special, it's static so we will use __builtin_return_address(0)
    rip = (uint64_t)__builtin_return_address(0);

    // Other registers we can get from mov X, Y
    __asm__ volatile (
        "mov %%rax, %0\n"
        "mov %%rbx, %1\n"
        "mov %%rcx, %2\n"
        "mov %%rdx, %3\n"
        "mov %%rsp, %4\n"
        "mov %%rbp, %5\n"
        : "=r"(rax), "=r"(rbx), "=r"(rcx), "=r"(rdx), "=r"(rsp), "=r"(rbp)
    );

    #define GOLDEN_RATIO 0x9E3779B97F4A7C15ULL

    uint64_t timer_entropy = hpet_entropy;
    timer_entropy = timer_entropy * GOLDEN_RATIO ^ tsc_entropy;
    timer_entropy = timer_entropy * GOLDEN_RATIO ^ pit_entropy;

    uint64_t reg_entropy = rax;
    reg_entropy = reg_entropy * GOLDEN_RATIO ^ rbx;
    reg_entropy = reg_entropy * GOLDEN_RATIO ^ rcx;
    reg_entropy = reg_entropy * GOLDEN_RATIO ^ rdx;
    reg_entropy = reg_entropy * GOLDEN_RATIO ^ rip;
    reg_entropy = reg_entropy * GOLDEN_RATIO ^ rsp;
    reg_entropy = reg_entropy * GOLDEN_RATIO ^ rbp;

    rng_state[0] = 0x61707865;
    rng_state[1] = 0x3320646e;
    rng_state[2] = 0x79622d32;
    rng_state[3] = 0x6b206574;

    for (int i = 4; i < 16; i++) {
        uint64_t entropy = timer_entropy ^ (reg_entropy * (GOLDEN_RATIO * i));
        rng_state[i] = (uint32_t)(entropy ^ (entropy >> 32));
    }

    #undef GOLDEN_RATIO
}

void add_entropy_bytes(const void *buf, size_t len) {
    uint64_t flags;
    spin_lock_irqsave(&rng_lock, &flags);

    const uint8_t *in = (const uint8_t *)buf;
    
    #define GOLDEN_RATIO 0x9E3779B97F4A7C15ULL

    for (size_t i = 0; i < len; i++) {
        int slot = 4 + (i % 12);
        uint64_t mix = ((uint64_t)rng_state[slot] ^ in[i]) * GOLDEN_RATIO;
        rng_state[slot] ^= (uint32_t)(mix ^ (mix >> 32));
    }

    rng_seeded = true;

    #undef GOLDEN_RATIO

    spin_unlock_irqrestore(&rng_lock, flags);
}

void get_random_bytes(void *buf, size_t len) {
    uint64_t flags;
    spin_lock_irqsave(&rng_lock, &flags);

    uint8_t *out = (uint8_t *)buf;
    size_t pos = 0;

    #define ROTL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
    #define QR(a, b, c, d) \
        a += b; d ^= a; d = ROTL32(d, 16); \
        c += d; b ^= c; b = ROTL32(b, 12); \
        a += b; d ^= a; d = ROTL32(d, 8); \
        c += d; b ^= c; b = ROTL32(b, 7);

    while (pos < len) {
        uint32_t x[16];
        memcpy(x, rng_state, 64);

        for (int i = 0; i < 10; i++) {
            QR(x[0], x[4], x[8], x[12]);
            QR(x[1], x[5], x[9], x[13]);
            QR(x[2], x[6], x[10], x[14]);
            QR(x[3], x[7], x[11], x[15]);
            QR(x[0], x[5], x[10], x[15]);
            QR(x[1], x[6], x[11], x[12]);
            QR(x[2], x[7], x[8], x[13]);
            QR(x[3], x[4], x[9], x[14]);
        }

        for (int i = 0; i < 16; i++) x[i] += rng_state[i];

        if (++rng_state[12] == 0) regen_rng();

        size_t chunk = len - pos < 64 ? len - pos : 64;
        memcpy(out + pos, x, chunk);
        pos += chunk;
    }

    #undef QR
    #undef ROTL32

    spin_unlock_irqrestore(&rng_lock, flags);
}

bool is_rng_seeded(void) {
    uint64_t flags;
    spin_lock_irqsave(&rng_lock, &flags);
    bool seeded = rng_seeded;
    spin_unlock_irqrestore(&rng_lock, flags);
    return seeded;
}

void init_rng(void) {
    regen_rng();
    rng_seeded = true;
    log("initialized rng");
}
