#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <libgen.h>
#include "journal.h"
#include "util.h"

int journal_load(const char* path, Journal* j)
{
    FILE* fp = fopen(path, "r");
    char line[1280];

    if (!fp) return -1;

    memset(j, 0, sizeof(*j));

    while (fgets(line, sizeof(line), fp))
    {
        char* p = line;
        char* eq;
        char* key;
        char* val;
        char* hash = strchr(p, '#');
        if (hash) *hash = '\0';

        p = str_trim(p);
        if (!*p) continue;

        eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        key = str_trim(p);
        val = str_trim(eq + 1);

        if      (strcmp(key, "device") == 0)          strncpy(j->device, val, sizeof(j->device) - 1);
        else if (strcmp(key, "seed") == 0)            j->seed = (U32)strtoul(val, NULL, 0);
        else if (strcmp(key, "blk") == 0)             j->blk = (U32)strtoul(val, NULL, 0);
        else if (strcmp(key, "unit_blocks") == 0)     j->unit_blocks = (U32)strtoul(val, NULL, 0);
        else if (strcmp(key, "ranges") == 0)          strncpy(j->ranges, val, sizeof(j->ranges) - 1);
        else if (strcmp(key, "access") == 0)          strncpy(j->access, val, sizeof(j->access) - 1);
        else if (strcmp(key, "durability") == 0)      strncpy(j->durability, val, sizeof(j->durability) - 1);
        else if (strcmp(key, "pattern") == 0)         j->pattern = (U32)strtoul(val, NULL, 0);
        else if (strcmp(key, "gen") == 0)             j->gen = (U32)strtoul(val, NULL, 0);
        else if (strcmp(key, "total_units") == 0)     j->total_units = (U64)strtoull(val, NULL, 0);
        else if (strcmp(key, "durable_units") == 0)   j->durable_units = (U64)strtoull(val, NULL, 0);
        else if (strcmp(key, "submitted_units") == 0) j->submitted_units = (U64)strtoull(val, NULL, 0);
        else if (strcmp(key, "cycle") == 0)           j->cycle = (U32)strtoul(val, NULL, 0);
    }

    fclose(fp);
    return 0;
}

int journal_save(const char* path, const Journal* j)
{
    char tmp[1100];
    char dirbuf[1024];
    FILE* fp;
    int fd;
    int dfd;

    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    fp = fopen(tmp, "w");
    if (!fp) return -1;

    fprintf(fp,
        "# DiskIOStress power-cycle journal\n"
        "device=%s\n"
        "seed=0x%08X\n"
        "blk=%u\n"
        "unit_blocks=%u\n"
        "ranges=%s\n"
        "access=%s\n"
        "durability=%s\n"
        "pattern=%u\n"
        "gen=%u\n"
        "total_units=%llu\n"
        "durable_units=%llu\n"
        "submitted_units=%llu\n"
        "cycle=%u\n",
        j->device, j->seed, j->blk, j->unit_blocks, j->ranges,
        j->access, j->durability, j->pattern, j->gen,
        (unsigned long long)j->total_units,
        (unsigned long long)j->durable_units,
        (unsigned long long)j->submitted_units,
        j->cycle);

    fflush(fp);
    fd = fileno(fp);
    if (fd >= 0) fsync(fd);   /* durable file contents before rename */
    fclose(fp);

    if (rename(tmp, path) != 0) return -1;

    /* fsync the directory so the rename itself is durable */
    strncpy(dirbuf, path, sizeof(dirbuf) - 1);
    dirbuf[sizeof(dirbuf) - 1] = '\0';
    dfd = open(dirname(dirbuf), O_RDONLY | O_DIRECTORY);
    if (dfd >= 0)
    {
        fsync(dfd);
        close(dfd);
    }

    return 0;
}
