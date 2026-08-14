#pragma once

#define DEFAULT_DOMAINNAME "(none)"
#define MAX_DOMAINNAME_LEN 65

int get_domainname(char *name, size_t len);
int set_domainname(const char *name, size_t len);
