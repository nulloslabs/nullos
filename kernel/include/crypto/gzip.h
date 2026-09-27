#pragma once

// --- Configuration ---
#define TGZ_MAX_BITS       15
#define TGZ_OK             0
#define TGZ_ERR_FORMAT    -1

// --- Internal Context ---
typedef struct {
    const uint8_t *in;  // Current input pointer
    uint8_t *out;       // Current output pointer
    uint8_t *out_start; // To calculate total written
    
    uint32_t bit_buf;
    int bit_cnt;
} tgz_stream;

typedef struct {
    uint16_t counts[TGZ_MAX_BITS + 1];
    uint16_t symbols[TGZ_MAX_BITS + 1][288]; 
} tgz_huffman;

int ungzip(const void *src, void *dst);