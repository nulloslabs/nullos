#pragma once

#include <stdint.h>
#include <stdbool.h>

int get_pts_idx(const char *name);
bool devpts_device_exists(const char* name);
bool is_devpts_path(const char *path, char *rel_out);
const char *devpts_get_device_name(int index);
const char *devpts_get_slave_name(int index);
