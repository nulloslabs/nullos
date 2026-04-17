#pragma once

#define DEFAULT_HOSTNAME "(none)"
#define HOSTNAME_MAX_LEN 64

int get_hostname(char *name, size_t len);
int set_hostname(const char *name, size_t len);
