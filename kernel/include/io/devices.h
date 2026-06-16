#pragma once

#include <freestanding/stdint.h>

uint64_t read_device(const char* name, void* buf, uint64_t count, uint64_t offset);
uint64_t write_device(const char* name, const void* buf, uint64_t count, uint64_t offset);
void init_devices(void);
int register_device(const char* name, uint64_t (*read_fn)(void*, uint64_t, uint64_t), uint64_t (*write_fn)(const void*, uint64_t, uint64_t));
int unregister_device(const char* name);
