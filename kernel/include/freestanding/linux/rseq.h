#pragma once

#include <freestanding/stdint.h>

enum rseq_cpu_id_state {
    RSEQ_CPU_ID_UNITIALIZED         = -1,
    RSEQ_CPU_ID_REGISTRATION_FAILED = -2,
};

enum rseq_flags {
    RSEQ_FLAG_UNREGISTER = (1 << 0),
};

enum rseq_cs_flags_bit {
    RSEQ_CS_FLAG_NO_RESTART_ON_PREEMPT_BIT = 0,
    RSEQ_CS_FLAG_NO_RESTART_ON_SIGNAL_BIT  = 1,
    RSEQ_CS_FLAG_NO_RESTART_ON_MIGRATE_BIT = 2,
};

enum rseq_cs_flags {
    RSEQ_CS_FLAG_NO_RESTART_ON_PREEMPT = (1U << RSEQ_CS_FLAG_NO_RESTART_ON_PREEMPT_BIT),
    RSEQ_CS_FLAG_NO_RESTART_ON_SIGNAL  = (1U << RSEQ_CS_FLAG_NO_RESTART_ON_SIGNAL_BIT),
    RSEQ_CS_FLAG_NO_RESTART_ON_MIGRATE = (1U << RSEQ_CS_FLAG_NO_RESTART_ON_MIGRATE_BIT),
};

struct rseq_cs {
    uint32_t cpu_id_start;
    uint32_t cpu_id;
    union {
        uint64_t ptr64;
        uint64_t ptr;
    } rseq_cs;
    uint32_t flags;
} __attribute__((aligned(4 * sizeof(uint64_t))));
