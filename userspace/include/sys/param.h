#pragma once

#ifndef NBBY
#define NBBY            8
#endif
#define NBPW            8

#define EXEC_PAGESIZE   4096

#define LITTLE_ENDIAN   1234
#define BIG_ENDIAN      4321
#define PDP_ENDIAN      3412

#ifndef BYTE_ORDER
#define BYTE_ORDER      LITTLE_ENDIAN
#endif

#ifndef MAXPATHLEN
#define MAXPATHLEN      4096
#endif
#ifndef MAXNAMLEN
#define MAXNAMLEN       255
#endif

#define MAXCOMLEN       16
#define MAXHOSTNAMELEN  64

#ifndef MIN
#define MIN(a,b)        (((a)<(b))?(a):(b))
#endif
#ifndef MAX
#define MAX(a,b)        (((a)>(b))?(a):(b))
#endif

#ifndef howmany
#define howmany(x, y)   (((x) + ((y) - 1)) / (y))
#endif

#ifndef roundup
#define roundup(x, y)   ((((x)+((y)-1))/(y))*(y))
#endif

#ifndef powerof2
#define powerof2(x)     ((((x)-1)&(x))==0)
#endif