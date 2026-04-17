#pragma once

const char *get_boot_args(void);
const char* get_arg_value(const char* args, const char* key);
void parse_boot_args(void);