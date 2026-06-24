#include <main/sse.h>
#include <main/string.h>
#include <main/machine_info.h>
#include <io/terminal.h>
#include <main/panic.h>

extern void save_fpu_xsave(void *area);
extern void save_fpu_fxsave(void *area);
extern void restore_fpu_xsave(const void *area);
extern void restore_fpu_fxsave(const void *area);
extern void clear_fpu_ts(void);
extern void init_fpu_x87(void);

static bool use_xsave = false;
static size_t cached_state_size = 0;

size_t get_fpu_state_size(void) {
    return cached_state_size ? cached_state_size : xsave_area_size();
}

void save_fpu_state(void *area) {
    if (use_xsave) {
        save_fpu_xsave(area);
    } else {
        save_fpu_fxsave(area);
    }
}

void restore_fpu_state(const void *area) {
    if (use_xsave) {
        restore_fpu_xsave(area);
    } else {
        restore_fpu_fxsave(area);
    }
}

void init_fpu_area(void *area) {
    uint32_t mxcsr_default = 0x1F80;  // all exceptions masked, round-to-nearest
    memset(area, 0, get_fpu_state_size());
    clear_fpu_ts();                         // ensure no #NM
    init_fpu_x87();                       // x87 to reset state
    __asm__ volatile("ldmxcsr %0" :: "m"(mxcsr_default));
    save_fpu_state(area);
}

void init_sse(void) {
    if (!cpu_has_feature(CPU_FEATURE_SSE)) panic("cpu dosen't support sse");
    if (!cpu_has_feature(CPU_FEATURE_SSE2)) panic("cpu dosen't support sse2");

    // Clear EM bit and set MP bit in CR0
    __asm__ volatile(
        "mov %%cr0, %%rax\n"
        "and $~(1<<2), %%rax\n"  // clear EM
        "or  $(1<<1), %%rax\n"   // set MP
        "mov %%rax, %%cr0\n"
        ::: "rax"
    );
    // Set OSFXSR and OSXMMEXCPT bits in CR4
    __asm__ volatile(
        "mov %%cr4, %%rax\n"
        "or $(1<<9), %%rax\n"    // OSFXSR
        "or $(1<<10), %%rax\n"   // OSXMMEXCPT
        "mov %%rax, %%cr4\n"
        ::: "rax"
    );

    if (cpu_has_feature(CPU_FEATURE_XSAVE)) {
        __asm__ volatile(
            "mov %%cr4, %%rax\n"
            "or $(1<<18), %%rax\n"   // OSXSAVE
            "mov %%rax, %%cr4\n"
            ::: "rax"
        );
        use_xsave = true;
    } else {
        use_xsave = false;
    }

    cached_state_size = xsave_area_size();
    clear_fpu_ts();
    printf("sse: enabled sse\n");
}
