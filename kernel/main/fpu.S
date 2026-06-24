.intel_syntax noprefix
.global save_fpu_xsave
.global save_fpu_xsave_mask
.global save_fpu_fxsave
.global restore_fpu_xsave
.global restore_fpu_xsave_mask
.global restore_fpu_fxsave
.global init_fpu_x87
.global clear_fpu_ts

save_fpu_xsave:
    mov rdx, 0xFFFFFFFFFFFFFFFF
    mov rax, 0xFFFFFFFFFFFFFFFF
    xsave64 [rdi]
    ret

save_fpu_xsave_mask:
    mov rax, rsi
    mov rdx, 0xFFFFFFFFFFFFFFFF
    xsave64 [rdi]
    ret

save_fpu_fxsave:
    fxsave64 [rdi]
    ret

restore_fpu_xsave:
    mov rdx, 0xFFFFFFFFFFFFFFFF
    mov rax, 0xFFFFFFFFFFFFFFFF
    xrstor64 [rdi]
    ret

restore_fpu_xsave_mask:
    mov rax, rsi
    mov rdx, 0xFFFFFFFFFFFFFFFF
    xrstor64 [rdi]
    ret

restore_fpu_fxsave:
    fxrstor64 [rdi]
    ret

init_fpu_x87:
    fninit
    ret

clear_fpu_ts:
    clts
    ret
