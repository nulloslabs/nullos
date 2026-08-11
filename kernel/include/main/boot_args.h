#pragma once

#include <stdbool.h>

const char *get_boot_args(void);
bool has_boot_arg(const char *key);
const char *get_arg_value(const char* key);
