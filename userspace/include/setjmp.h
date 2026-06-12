#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned long long jmp_buf[8];

__attribute__((naked)) int setjmp(jmp_buf env);
__attribute__((naked, noreturn)) void longjmp(jmp_buf env, int val);

#ifdef __cplusplus
}
#endif