#pragma once

extern volatile int system_halted;

void halt_other_cpus(void);

void cli(void);
void sti(void);
__attribute__((noreturn)) void idle(void);
__attribute__((noreturn)) void halt(void);
