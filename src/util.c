#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/resource.h>
#include "util.h"

U32 get_timeval_sec(struct timeval base_timeval)
{
    struct timeval curr_timeval;

    gettimeofday(&curr_timeval, NULL);

    return curr_timeval.tv_sec - base_timeval.tv_sec;
}

double get_timeval_sec_usec(struct timeval base_timeval)
{
    struct timeval curr_timeval;

    gettimeofday(&curr_timeval, NULL);

    return (curr_timeval.tv_sec + curr_timeval.tv_usec / 1000000.0) - (base_timeval.tv_sec + base_timeval.tv_usec / 1000000.0);
}

U32 get_core_number(void)
{
    FILE* fp = popen("cat /proc/cpuinfo | grep processor | wc -l", "r");
    U32 nr_core;

    fscanf(fp, "%d", &nr_core);

    pclose(fp);

    return nr_core;
}

void set_process_priority(S32 priority)
{
    int which = PRIO_PROCESS;
    id_t pid;
    int ret;

    pid = getpid();
    ret = setpriority(which, pid, priority);
    (void)ret;
}

void toLowerCase(char* src, char* dest, int len)
{
    int i;

    for (i = 0; i < len ; i++)
    {
        if (src[i] >= 'A' && src[i] <= 'Z') dest[i] = 'a' + (src[i] - 'A');
        else                                dest[i] = src[i];
    }

    dest[i] = '\0';
}

U64 hex2dec(char* buf)
{
    U32 len;
    S32 i;
    U64 base = 1;
    U64 num = 0;

    len = strlen(buf);

    // to lower case
    for (i = 0; i < (S32)len; i++)
    {
        if (buf[i] >= 'A' && buf[i] <= 'Z')
        {
            buf[i] = 'a' + buf[i] - 'A';
        }
    }

    for (i = len - 1; i >= 0; i--)
    {
        if (buf[i] >= '0' && buf[i] <= '9')
        {
            num += (buf[i] - '0') * base;
        }
        else
        {
            num += (buf[i] - 'a' + 10) * base;
        }

        base *= 16;
    }

    return num;
}

void dump_buffer(char* buf, U32 len)
{
    U32 i, j;

    for (i = 0; i < len / 16; i++)
    {
        for (j = 0; j < 16; j++)
        {
            printf(" %02X", buf[i * 16 + j] & 0xFF);
        }

        printf (" | ");

        for (j = 0; j < 16; j++)
        {
            if (buf[i * 16 + j] > 0x1F && buf[i * 16 + j] < 0x7F)
            {
                printf("%c", buf[i * 16 + j] & 0xFF);
            }
            else
            {
                printf(".");
            }
        }

        printf("\n");
    }

    printf("===========================================================\n");
}

char* str_trim(char* s)
{
    char* end;
    while (*s && isspace((unsigned char)*s)) s++;
    end = s + strlen(s);
    while (end > s && isspace((unsigned char)*(end - 1))) end--;
    *end = '\0';
    return s;
}

/* CRC32 (IEEE 802.3, reflected, poly 0xEDB88320). Table is built once at
 * startup (single-threaded) and only read afterwards, so it is thread-safe. */
static U32 gCrc32Table[256];

void crc32_init(void)
{
    U32 i, j, c;
    for (i = 0; i < 256; i++)
    {
        c = i;
        for (j = 0; j < 8; j++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        gCrc32Table[i] = c;
    }
}

U32 crc32(const void* data, U32 len)
{
    const U8* p = (const U8*)data;
    U32 crc = 0xFFFFFFFFu;
    U32 i;
    for (i = 0; i < len; i++)
        crc = gCrc32Table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

int parse_size(const char* s, U64* out)
{
    char* end;
    unsigned long long v;
    U64 mult = 1;

    if (!s || !*s) return -1;

    errno = 0;
    v = strtoull(s, &end, 0);   /* base 0: 0x.. hex or decimal */
    if (end == s || errno != 0) return -1;

    if (*end)
    {
        switch (*end)
        {
            case 'k': case 'K': mult = 1024ULL; break;
            case 'm': case 'M': mult = 1024ULL * 1024; break;
            case 'g': case 'G': mult = 1024ULL * 1024 * 1024; break;
            case 't': case 'T': mult = 1024ULL * 1024 * 1024 * 1024; break;
            default: return -1;
        }
        end++;
        if (*end) return -1;   /* trailing junk */
    }

    *out = (U64)v * mult;
    return 0;
}

int parse_duration(const char* s, U32* out)
{
    char* end;
    unsigned long long v;
    U64 mult = 1;

    if (!s || !*s) return -1;
    errno = 0;
    v = strtoull(s, &end, 10);
    if (end == s || errno != 0) return -1;

    if (*end)
    {
        switch (*end)
        {
            case 's': mult = 1;     break;
            case 'm': mult = 60;    break;
            case 'h': mult = 3600;  break;
            case 'd': mult = 86400; break;
            default: return -1;
        }
        end++;
        if (*end) return -1;
    }

    *out = (U32)((U64)v * mult);
    return 0;
}
