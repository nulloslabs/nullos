#include <main/sse.h>
#include <io/terminal.h>
#include <main/machine_info.h>
#include <main/panic.h>

void init_sse(void) {
    // Check if SSE is available
    if (!cpu_has_feature(CPU_FEATURE_SSE)) { panic("cpu dosen't support sse"); }

    // Check if SSE2 is available
    if (!cpu_has_feature(CPU_FEATURE_SSE2)) { panic("cpu dosen't support sse2"); }

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
    printf("sse: enabled sse\n");
}