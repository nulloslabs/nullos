#pragma once

#include <freestanding/stdint.h>
#include <freestanding/stdbool.h>

uint64_t read_devpts(const char* name, void* buf, uint64_t count, uint64_t offset);
uint64_t write_devpts(const char* name, const void* buf, uint64_t count, uint64_t offset);
bool devpts_device_exists(const char* name);
const char *devpts_get_device_name(int index);
