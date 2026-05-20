#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "range.h"
#include "util.h"

static void recompute_total(RangeSet* rs)
{
    U32 i;
    rs->total_units = 0;
    for (i = 0; i < rs->count; i++)
    {
        U64 blocks = rs->ranges[i].end_lba - rs->ranges[i].start_lba;
        rs->total_units += blocks / rs->unit_blocks;
    }
}

static int add_range(RangeSet* rs, U64 start_lba, U64 end_lba, U64 cap_blocks)
{
    U64 span;

    if (rs->count >= MAX_RANGES)        return -1;
    if (end_lba > cap_blocks) end_lba = cap_blocks;
    if (start_lba >= end_lba)           return -1;

    /* truncate to a whole number of units */
    span = (end_lba - start_lba) / rs->unit_blocks * rs->unit_blocks;
    if (span == 0)                      return -1;

    rs->ranges[rs->count].start_lba = start_lba;
    rs->ranges[rs->count].end_lba   = start_lba + span;
    rs->count++;
    return 0;
}

int range_parse(RangeSet* rs, const char* spec, U32 blk, U64 cap_blocks, U32 unit_blocks)
{
    char buf[1024];
    char* save = NULL;
    char* tok;

    if (!spec || !*spec || unit_blocks == 0 || blk == 0) return -1;

    memset(rs, 0, sizeof(*rs));
    rs->unit_blocks = unit_blocks;

    if (strcmp(spec, "whole") == 0)
    {
        if (add_range(rs, 0, cap_blocks, cap_blocks) != 0) return -1;
        recompute_total(rs);
        return rs->total_units ? 0 : -1;
    }

    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    for (tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save))
    {
        char* dash = strchr(tok, '-');
        U64 sb, eb;
        char* a;
        char* b;

        if (!dash) return -1;
        *dash = '\0';
        a = tok;
        b = dash + 1;

        if (parse_size(a, &sb) != 0) return -1;
        if (parse_size(b, &eb) != 0) return -1;

        /* byte offsets -> sectors */
        if (add_range(rs, sb / blk, eb / blk, cap_blocks) != 0) return -1;
    }

    recompute_total(rs);
    return rs->total_units ? 0 : -1;
}

int range_make_regions(RangeSet* rs, U32 nregions, U64 region_bytes,
                       U32 blk, U64 cap_blocks, U32 unit_blocks)
{
    U64 region_blocks;
    U64 stride;
    U32 i;

    if (nregions == 0 || nregions > MAX_RANGES || region_bytes == 0 ||
        blk == 0 || unit_blocks == 0 || cap_blocks == 0)
        return -1;

    memset(rs, 0, sizeof(*rs));
    rs->unit_blocks = unit_blocks;

    region_blocks = region_bytes / blk;
    if (region_blocks == 0) return -1;
    if (region_blocks * nregions > cap_blocks) return -1;

    /* spread the regions evenly across the device */
    stride = cap_blocks / nregions;

    for (i = 0; i < nregions; i++)
    {
        U64 start = (U64)i * stride;
        if (add_range(rs, start, start + region_blocks, cap_blocks) != 0) return -1;
    }

    recompute_total(rs);
    return rs->total_units ? 0 : -1;
}

U64 range_unit_lba(const RangeSet* rs, U64 unit_index)
{
    U32 i;
    for (i = 0; i < rs->count; i++)
    {
        U64 units = (rs->ranges[i].end_lba - rs->ranges[i].start_lba) / rs->unit_blocks;
        if (unit_index < units)
            return rs->ranges[i].start_lba + unit_index * rs->unit_blocks;
        unit_index -= units;
    }
    return 0;   /* out of range (caller must keep unit_index < total_units) */
}

/* ---- Full-coverage random permutation via a small Feistel network ---- */

static U32 feistel_mix(U32 x, U32 round, U32 key)
{
    /* cheap reversible-network round function (need not be invertible itself) */
    x ^= key + round * 0x9E3779B9u;
    x *= 0x85EBCA6Bu;
    x ^= x >> 13;
    x *= 0xC2B2AE35u;
    x ^= x >> 16;
    return x;
}

U64 range_permute(U64 total_units, U64 step, U32 round)
{
    U32 bits, half, mask;
    U64 n = total_units;

    if (n <= 1) return 0;

    /* domain = next power of two >= n, split into two equal halves */
    bits = 0;
    while (((U64)1 << bits) < n) bits++;
    if (bits & 1) bits++;            /* make it even so halves are equal */
    half = bits / 2;
    mask = (half >= 32) ? 0xFFFFFFFFu : ((1u << half) - 1);

    /* cycle-walk: re-apply the permutation until the result lands in [0,n) */
    do
    {
        U32 l = (U32)(step & mask);
        U32 r = (U32)((step >> half) & mask);
        int i;
        for (i = 0; i < 4; i++)
        {
            U32 nl = r;
            U32 nr = l ^ (feistel_mix(r, (U32)i, round + 0x1234u) & mask);
            l = nl;
            r = nr;
        }
        step = ((U64)r << half) | l;
    } while (step >= n);

    return step;
}
