#include <errno.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>

int h_errno;

static struct hostent host;
static char *aliases[1];
static char *addr_list[2];
static uint32_t host_addr;
static char host_name[256];

static int parse_ipv4(const char *s, uint32_t *out) {
    unsigned long parts[4];
    char *end;

    for (int i = 0; i < 4; i++) {
        if (*s < '0' || *s > '9') return 0;
        parts[i] = strtoul(s, &end, 10);
        if (parts[i] > 255) return 0;
        if (i < 3) {
            if (*end != '.') return 0;
            s = end + 1;
        } else if (*end) {
            return 0;
        }
    }

    *out = htonl((uint32_t)((parts[0] << 24) | (parts[1] << 16) |
                            (parts[2] << 8) | parts[3]));
    return 1;
}

static struct hostent *make_host(const char *name, uint32_t addr) {
    strncpy(host_name, name, sizeof(host_name) - 1);
    host_name[sizeof(host_name) - 1] = '\0';
    host_addr = addr;
    aliases[0] = NULL;
    addr_list[0] = (char *)&host_addr;
    addr_list[1] = NULL;
    host.h_name = host_name;
    host.h_aliases = aliases;
    host.h_addrtype = AF_INET;
    host.h_length = 4;
    host.h_addr_list = addr_list;
    h_errno = 0;
    return &host;
}

struct hostent *gethostbyname(const char *name) {
    uint32_t addr;
    if (!name) {
        h_errno = HOST_NOT_FOUND;
        return NULL;
    }
    if (strcmp(name, "localhost") == 0)
        return make_host("localhost", htonl(INADDR_LOOPBACK));
    if (parse_ipv4(name, &addr))
        return make_host(name, addr);
    h_errno = HOST_NOT_FOUND;
    return NULL;
}

struct hostent *gethostbyaddr(const void *addr, socklen_t len, int type) {
    if (!addr || len != 4 || type != AF_INET) {
        h_errno = NO_ADDRESS;
        return NULL;
    }
    return make_host("localhost", *(const uint32_t *)addr);
}

int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **res) {
    struct addrinfo *ai;
    struct sockaddr_in *sin;
    uint32_t addr = htonl(INADDR_LOOPBACK);
    int family = hints ? hints->ai_family : AF_UNSPEC;
    int socktype = hints ? hints->ai_socktype : 0;
    int protocol = hints ? hints->ai_protocol : 0;

    if (!res) return EAI_FAIL;
    *res = NULL;
    if (family != AF_UNSPEC && family != AF_INET) return EAI_FAMILY;
    if (node && strcmp(node, "localhost") != 0 && !parse_ipv4(node, &addr))
        return EAI_NONAME;
    if (!node && hints && (hints->ai_flags & AI_PASSIVE))
        addr = htonl(INADDR_ANY);

    ai = calloc(1, sizeof(*ai));
    sin = calloc(1, sizeof(*sin));
    if (!ai || !sin) {
        free(ai);
        free(sin);
        return EAI_MEMORY;
    }

    sin->sin_family = AF_INET;
    sin->sin_addr.s_addr = addr;
    sin->sin_port = service ? htons((uint16_t)strtoul(service, NULL, 10)) : 0;

    ai->ai_family = AF_INET;
    ai->ai_socktype = socktype;
    ai->ai_protocol = protocol;
    ai->ai_addrlen = sizeof(*sin);
    ai->ai_addr = (struct sockaddr *)sin;
    *res = ai;
    return 0;
}

void freeaddrinfo(struct addrinfo *res) {
    while (res) {
        struct addrinfo *next = res->ai_next;
        free(res->ai_addr);
        free(res->ai_canonname);
        free(res);
        res = next;
    }
}

const char *gai_strerror(int errcode) {
    switch (errcode) {
        case 0: return "Success";
        case EAI_BADFLAGS: return "Bad flags";
        case EAI_NONAME: return "Name or service not known";
        case EAI_AGAIN: return "Temporary failure";
        case EAI_FAIL: return "Non-recoverable failure";
        case EAI_FAMILY: return "Address family not supported";
        case EAI_SOCKTYPE: return "Socket type not supported";
        case EAI_SERVICE: return "Service not supported";
        case EAI_MEMORY: return "Out of memory";
        case EAI_SYSTEM: return "System error";
        default: return "Unknown error";
    }
}
