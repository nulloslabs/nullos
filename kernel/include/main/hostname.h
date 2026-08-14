#pragma once

#define DEFAULT_HOSTNAME "(none)"
#define MAX_HOSTNAME_LEN 65

int get_hostname(char *name, size_t len);
int set_hostname(const char *name, size_t len);
