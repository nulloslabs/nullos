.intel_syntax noprefix

.section .init
.global _init
.type _init, @function
_init:
    push rbp
    mov rbp, rsp

.section .fini
.global _fini
.type _fini, @function
_fini:
    push rbp
    mov rbp, rsp
