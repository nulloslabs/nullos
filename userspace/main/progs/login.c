#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

/* A simple unix-like login program. */

typedef struct {
    uid_t uid;
    gid_t gid;
    char shell[256];
} passwd_user_t;

#define ROTR(x, n) ((x >> n) | (x << (32 - n)))
#define SHFR(x, n) (x >> n)
#define CH(x, y, z) ((x & y) ^ (~x & z))
#define MAJ(x, y, z) ((x & y) ^ (x & z) ^ (y & z))
#define F1(x) (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define F2(x) (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define F3(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ SHFR(x, 3))
#define F4(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ SHFR(x, 10))

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static void sha256_hash(const char *src, char out[65]) {
    uint32_t h[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };

    size_t len = strlen(src);
    const uint8_t *data = (const uint8_t *)src;
    uint64_t bitlen = (uint64_t)len * 8;

    size_t offset = 0;
    int end = 0;
    int extra = 0;

    while (!end) {
        uint8_t chunk[64];
        uint32_t w[64];
        uint32_t v[8];

        memset(chunk, 0, sizeof(chunk));
        size_t rem = len - offset;

        if (rem >= 64) {
            memcpy(chunk, data + offset, 64);
            offset += 64;
        } else {
            if (!extra) {
                if (rem > 0)
                    memcpy(chunk, data + offset, rem);
                
                chunk[rem] = 0x80;

                if (rem < 56) {
                    for (int i = 0; i < 8; i++)
                        chunk[63 - i] = (uint8_t)(bitlen >> (i * 8));
                    end = 1;
                } else {
                    extra = 1;
                }
                offset = len;
            } else {
                for (int i = 0; i < 8; i++)
                    chunk[63 - i] = (uint8_t)(bitlen >> (i * 8));
                    end = 1;
            }
        }

        for (int i = 0; i < 16; i++) {
            w[i] = ((uint32_t)chunk[i * 4 + 0] << 24) |
                   ((uint32_t)chunk[i * 4 + 1] << 16) |
                   ((uint32_t)chunk[i * 4 + 2] << 8)  |
                   ((uint32_t)chunk[i * 4 + 3]);
        }

        for (int i = 16; i < 64; i++) {
            w[i] = F4(w[i - 2]) + w[i - 7] + F3(w[i - 15]) + w[i - 16];
        }

        memcpy(v, h, sizeof(h));

        for (int i = 0; i < 64; i++) {
            uint32_t t1 = v[7] + F2(v[4]) + CH(v[4], v[5], v[6]) + K[i] + w[i];
            uint32_t t2 = F1(v[0]) + MAJ(v[0], v[1], v[2]);

            v[7] = v[6];
            v[6] = v[5];
            v[5] = v[4];
            v[4] = v[3] + t1;
            v[3] = v[2];
            v[2] = v[1];
            v[1] = v[0];
            v[0] = t1 + t2;
        }

        for (int i = 0; i < 8; i++)
            h[i] += v[i];
    }

    static const char hex[] = "0123456789abcdef";

    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 4; j++) {
            uint8_t byte = (uint8_t)(h[i] >> ((3 - j) * 8));
            out[(i * 8) + (j * 2) + 0] = hex[(byte >> 4) & 0xF];
            out[(i * 8) + (j * 2) + 1] = hex[byte & 0xF];
        }
    }

    out[64] = '\0';
}

static int read_file(const char *path, char *buf, int size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    int n = read(fd, buf, size - 1);
    close(fd);

    if (n < 0) return -1;

    buf[n] = '\0';
    return n;
}

static const char *get_shadow_password(const char *shadow, const char *user) {
    static char hash[128];
    const char *p = shadow;
    int ulen = strlen(user);

    while (*p) {
        if (strncmp(p, user, ulen) == 0 && p[ulen] == ':') {
            const char *start = p + ulen + 1;
            const char *end = start;
            
            while (*end && *end != ':' && *end != '\n') {
                end++;
            }
            
            int len = end - start;
            if (len >= (int)sizeof(hash)) len = sizeof(hash) - 1;
            
            memcpy(hash, start, len);
            hash[len] = '\0';
            return hash;
        }
        
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    return NULL;
}

static int is_shadow_locked(const char *password_field) {
    return !password_field || password_field[0] == '!' || password_field[0] == '*';
}

static unsigned int parse_uint_field(const char *start, const char *end, int *ok) {
    unsigned int value = 0;
    if (start == end) {
        *ok = 0;
        return 0;
    }

    while (start < end) {
        if (*start < '0' || *start > '9') {
            *ok = 0;
            return 0;
        }
        value = (value * 10) + (unsigned int)(*start - '0');
        start++;
    }

    *ok = 1;
    return value;
}

static int get_passwd_user(const char *passwd, const char *user, passwd_user_t *out) {
    const char *p = passwd;
    int ulen = strlen(user);

    while (*p) {
        if (strncmp(p, user, ulen) == 0 && p[ulen] == ':') {
            const char *fields[7] = {0};
            const char *q = p;
            fields[0] = p;

            for (int i = 1; i < 7; i++) {
                q = strchr(q, ':');
                if (!q) break;
                fields[i] = ++q;
            }

            if (fields[6]) {
                const char *uid_end = strchr(fields[2], ':');
                const char *gid_end = strchr(fields[3], ':');
                int uid_ok = 0;
                int gid_ok = 0;

                uid_t uid = uid_end ? parse_uint_field(fields[2], uid_end, &uid_ok) : 0;
                gid_t gid = gid_end ? parse_uint_field(fields[3], gid_end, &gid_ok) : 0;

                if (uid_ok && gid_ok) {
                    int i = 0;
                    q = fields[6];
                    while (*q && *q != '\n' && *q != '\0' && i < 255) {
                        out->shell[i++] = *q++;
                    }
                    out->shell[i] = '\0';
                    out->uid = uid;
                    out->gid = gid;
                    return 1;
                }
            }
        }

        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    return 0;
}

static int is_passwd_blank(const char *passwd, const char *user) {
    const char *p = passwd;
    int ulen = strlen(user);

    while (*p) {
        if (strncmp(p, user, ulen) == 0 && p[ulen] == ':') {
            if (p[ulen + 1] == ':') {
                return 1; 
            }
            return 0; 
        }
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    return 0;
}

static void print_issue(void) {
    char buf[1024];
    struct utsname uts;

    if (uname(&uts) < 0) {
        strcpy(uts.sysname, "NullOS");
        strcpy(uts.nodename, "(none)");
        strcpy(uts.release, "unknown");
    }

    if (read_file("/etc/issue", buf, sizeof(buf)) < 0) return;

    for (const char *p = buf; *p; p++) {
        if (*p == '\\' && *(p+1)) {
            p++;
            switch (*p) {
                case 'n': printf("%s", uts.nodename); break;
                case 'r': printf("%s", uts.release);  break;
                case 'l': printf("tty0");             break;
                case 's': printf("%s", uts.sysname);  break;
                case '\\': printf("\\");              break;
                default:  printf("\\%c", *p);         break;
            }
        } else {
            putchar(*p);
        }
    }
}

static void print_motd(void) {
    char buf[4096];
    if (read_file("/etc/motd", buf, sizeof(buf)) < 0) return;
    printf("%s", buf);
}

int main(int argc, char **argv, char **envp) {
    char passwd_buf[4096]; 
    char shadow_buf[4096];
    char hostname[64];

    gethostname(hostname, sizeof(hostname));

    if (read_file("/etc/passwd", passwd_buf, sizeof(passwd_buf)) < 0) {
        fprintf(stderr, "Login: Cannot read /etc/passwd\n");
        return 1;
    }

    if (read_file("/etc/shadow", shadow_buf, sizeof(shadow_buf)) < 0) {
        fprintf(stderr, "Login: Cannot read /etc/shadow\n");
        return 1;
    }

    int show_issue = 1;

    for (;;) {
        char username[64];
        char password[256];
        char hashed[65];

        if (show_issue) {
            printf("\033[2J\033[H");
            printf("\n");
            print_issue();
            printf("\n");
            show_issue = 0;
        }

        printf("%s login: ", hostname);
        fflush(stdout);

        int n = read(0, username, sizeof(username) - 1);
        if (n <= 0) break;

        if (username[n - 1] == '\n') username[n - 1] = '\0';
        else username[n] = '\0';

        // 1. Traditional Unix behavior: instantly authenticate if /etc/passwd field is empty.
        int authenticated = 0;
        if (is_passwd_blank(passwd_buf, username)) {
            authenticated = 1;
        }

        // 2. Query password and verify the second field of the shadow entry. Aging fields
        // are parsed by skipping at ':' in get_shadow_password(), but not enforced yet.
        if (!authenticated) {
            printf("Password: ");
            fflush(stdout);

            n = read(0, password, sizeof(password) - 1);
            if (n <= 0) break;

            if (password[n - 1] == '\n') password[n - 1] = '\0';
            else password[n] = '\0';

            const char *stored_entry = get_shadow_password(shadow_buf, username);
            
            if (!is_shadow_locked(stored_entry) && strncmp(stored_entry, "$5$", 3) == 0) {
                const char *salt_start = stored_entry + 3;
                const char *salt_end = strchr(salt_start, '$');
                
                if (salt_end) {
                    char salt[64] = {0};
                    size_t salt_len = salt_end - salt_start;
                    
                    if (salt_len < sizeof(salt)) {
                        memcpy(salt, salt_start, salt_len);
                        salt[salt_len] = '\0';
                        
                        // Point straight to the beginning of the hash data token
                        const char *stored_hash = salt_end + 1;

                        // Recompose payload: salt + plaintext password string
                        char salt_plus_password[512];
                        snprintf(salt_plus_password, sizeof(salt_plus_password), "%s%s", salt, password);

                        // Process composite block through crypto stage
                        sha256_hash(salt_plus_password, hashed);

                        // Validate match matrix
                        if (strcmp(hashed, stored_hash) == 0) {
                            authenticated = 1;
                        }
                    }
                }
            }
        }

        // 3. Verify authentication status
        if (!authenticated) {
            printf("\nLogin incorrect.\n\n");
            continue;
        }

        // 4. Resolve target user shell configuration mapping
        passwd_user_t user_info;
        if (!get_passwd_user(passwd_buf, username, &user_info) || !user_info.shell[0]) {
            fprintf(stderr, "\nLogin: no shell configured for %s\n\n", username);
            continue;
        }

        printf("\n");
        print_motd();
        printf("\n");

        pid_t pid = fork();
        if (pid == 0) {
            if (setgid(user_info.gid) < 0 || setuid(user_info.uid) < 0) {
                perror("\nLogin: set credentials failed");
                _exit(126);
            }

            char *sh_argv[] = { user_info.shell, NULL };
            execve(user_info.shell, sh_argv, envp);
            perror("\nLogin: execve() failed");
            _exit(127);
        }

        if (pid < 0) {
            perror("\nLogin: fork() failed");
            continue;
        }

        int status;
        waitpid(pid, &status, 0);
        printf("\n");
        show_issue = 1;
    }

    return 1;
}
