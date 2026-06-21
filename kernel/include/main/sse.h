#pragma once

#include <freestanding/stdint.h>
#include <freestanding/stddef.h>

void init_sse(void);

// ---- Per-task FPU / XSAVE state primitives ----
//
// These wrap the raw assembly in main/fpu.S, choosing XSAVE vs FXSAVE based
// on CPU capability.  Callers pass a buffer at least fpu_state_size() bytes
// large, page-aligned for XSAVE (the CPU requires 64-byte alignment).

// Number of bytes required to hold a task's FPU state.  Equals the FXSAVE
// 512-byte area when XSAVE is unavailable, or the CPU's reported XSAVE area
// size otherwise.  Stable after init_sse().
size_t get_fpu_state_size(void);

// Save the current FPU/SSE state into `area` (kernel pointer).  Uses XSAVE
// when available, FXSAVE otherwise.
void save_fpu_state(void *area);

// Restore FPU/SSE state from `area` into the CPU.  Uses XRSTOR / FXRSTOR.
void restore_fpu_state(const void *area);

// Initialize `area` to a clean (fninit'd, all-zero) saved state, so a newly
// created task starts with a sane FPU rather than the previous owner's.
void init_fpu_area(void *area);
