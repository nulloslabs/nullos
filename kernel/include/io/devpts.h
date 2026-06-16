#pragma once

#include <freestanding/stdint.h>
#include <freestanding/stdbool.h>

bool devpts_device_exists(const char* name);
const char *devpts_get_device_name(int index);
