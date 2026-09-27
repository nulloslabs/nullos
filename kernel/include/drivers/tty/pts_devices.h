#pragma once

#include <freestanding/stdint.h>

uint64_t read_pts_device(const char* name, void* buf, uint64_t count, uint64_t offset);
uint64_t write_pts_device(const char* name, const void* buf, uint64_t count, uint64_t offset);
