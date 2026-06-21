.intel_syntax noprefix
# ============================================================================
# FPU / XSAVE state save and restore primitives.
#
# The kernel uses an eager FPU model: on every context switch we save the
# outgoing task's FPU state into its per-task buffer and restore the incoming
# task's.  This file provides the raw instructions; the C layer in scheduler.c
# picks XSAVE vs FXSAVE based on cpu_has_feature(CPU_FEATURE_XSAVE).
#
# Calling convention (System V AMD64):
#   save_fpu_xsave(rdi=area):           xsave  [rdi]
#   save_fpu_fxsave(rdi=area):          fxsave [rdi]
#   restore_fpu_xsave(rdi=area):        xrstor [rdi]
#   restore_fpu_fxsave(rdi=area):       fxrstor[rdi]
#   fpu_init_xsave(rdi=area):           zero area, fninit, xsave (clean state)
#   fpu_init_fxsave(rdi=area):          zero area, fninit, fxsave
#
# XSAVE/XRSTOR use EDX:EAX as the feature mask.  We pass 0xFFFFFFFFFFFFFFFF
# in both so all supported components are saved/restored (the CPU silently
# ignores bits for unsupported components).  For the signal-frame path the
# caller passes a specific mask via save_fpu_xsave_mask / restore_fpu_xsave_mask.
# ============================================================================

.global save_fpu_xsave
save_fpu_xsave:
        mov     rdx, 0xFFFFFFFFFFFFFFFF
        mov     rax, 0xFFFFFFFFFFFFFFFF
        xsave64 [rdi]
        ret

.global save_fpu_xsave_mask
# rdi = area, rsi = mask (low 64 in rax, high 64 in rdx)
save_fpu_xsave_mask:
        mov     rax, rsi
        mov     rdx, 0xFFFFFFFFFFFFFFFF
        xsave64 [rdi]
        ret

.global save_fpu_fxsave
save_fpu_fxsave:
        fxsave64 [rdi]
        ret

.global restore_fpu_xsave
restore_fpu_xsave:
        mov     rdx, 0xFFFFFFFFFFFFFFFF
        mov     rax, 0xFFFFFFFFFFFFFFFF
        xrstor64 [rdi]
        ret

.global restore_fpu_xsave_mask
restore_fpu_xsave_mask:
        mov     rax, rsi
        mov     rdx, 0xFFFFFFFFFFFFFFFF
        xrstor64 [rdi]
        ret

.global restore_fpu_fxsave
restore_fpu_fxsave:
        fxrstor64 [rdi]
        ret

.global init_fpu_x87
# Just initialize the x87 FPU state.  Used before a clean save so the saved
# area reflects a sane initial state rather than whatever the previous owner
# left in the FPU.
init_fpu_x87:
        fninit
        ret

.global clear_fpu_ts
# Clear CR0.TS (task-switched bit).  Defensive: with eager save/restore TS
# is never set, but the kernel entry stub may set it; ensure FPU instructions
# don't #NM when we touch the registers here.
clear_fpu_ts:
        clts
        ret
