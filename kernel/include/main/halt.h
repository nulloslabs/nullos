#pragma once

extern volatile int system_halted;

void cli(void);
void sti(void);
void halt_other_cpus(void);
__attribute__((noreturn)) void halt(void);
__attribute__((noreturn)) void idle(void);
void pause(void);
void wfi(void);
