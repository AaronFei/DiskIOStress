#include <string.h>
#include <stdlib.h>
#include "unity.h"
#include "range.h"
#include "util.h"

/* ---------- parse_size ---------- */
static void test_parse_size_units(void)
{
    U64 v;
    TEST_ASSERT_EQUAL_INT(0, parse_size("4096", &v));     TEST_ASSERT_EQUAL_UINT64(4096, v);
    TEST_ASSERT_EQUAL_INT(0, parse_size("0x1000", &v));   TEST_ASSERT_EQUAL_UINT64(0x1000, v);
    TEST_ASSERT_EQUAL_INT(0, parse_size("64K", &v));      TEST_ASSERT_EQUAL_UINT64(64ULL*1024, v);
    TEST_ASSERT_EQUAL_INT(0, parse_size("1M", &v));       TEST_ASSERT_EQUAL_UINT64(1024ULL*1024, v);
    TEST_ASSERT_EQUAL_INT(0, parse_size("2G", &v));       TEST_ASSERT_EQUAL_UINT64(2ULL*1024*1024*1024, v);
    TEST_ASSERT_EQUAL_INT(0, parse_size("1T", &v));       TEST_ASSERT_EQUAL_UINT64(1024ULL*1024*1024*1024, v);
}

static void test_parse_size_bad(void)
{
    U64 v;
    TEST_ASSERT_EQUAL_INT(-1, parse_size("", &v));
    TEST_ASSERT_EQUAL_INT(-1, parse_size("12X", &v));
    TEST_ASSERT_EQUAL_INT(-1, parse_size("1Mb", &v));
    TEST_ASSERT_EQUAL_INT(-1, parse_size("abc", &v));
}

/* ---------- range_parse ---------- */
static void test_range_parse_whole(void)
{
    RangeSet rs;
    /* 512B sectors, cap = 2048 sectors (1 MiB), unit = 8 sectors (4 KiB) */
    TEST_ASSERT_EQUAL_INT(0, range_parse(&rs, "whole", 512, 2048, 8));
    TEST_ASSERT_EQUAL_UINT(1, rs.count);
    TEST_ASSERT_EQUAL_UINT64(0, rs.ranges[0].start_lba);
    TEST_ASSERT_EQUAL_UINT64(2048, rs.ranges[0].end_lba);
    TEST_ASSERT_EQUAL_UINT64(2048 / 8, rs.total_units);
}

static void test_range_parse_explicit_bytes(void)
{
    RangeSet rs;
    /* "0-64K,128K-192K" with 512B sectors, unit 8 sectors */
    TEST_ASSERT_EQUAL_INT(0, range_parse(&rs, "0-64K,128K-192K", 512, 1024*1024/512, 8));
    TEST_ASSERT_EQUAL_UINT(2, rs.count);
    TEST_ASSERT_EQUAL_UINT64(0,        rs.ranges[0].start_lba);
    TEST_ASSERT_EQUAL_UINT64(64*1024/512,  rs.ranges[0].end_lba);   /* 128 sectors */
    TEST_ASSERT_EQUAL_UINT64(128*1024/512, rs.ranges[1].start_lba); /* 256 */
    TEST_ASSERT_EQUAL_UINT64(192*1024/512, rs.ranges[1].end_lba);   /* 384 */
    /* each 64K range = 128 sectors / 8 = 16 units; two ranges = 32 */
    TEST_ASSERT_EQUAL_UINT64(32, rs.total_units);
}

static void test_range_parse_bad(void)
{
    RangeSet rs;
    TEST_ASSERT_EQUAL_INT(-1, range_parse(&rs, "", 512, 2048, 8));
    TEST_ASSERT_EQUAL_INT(-1, range_parse(&rs, "0_1G", 512, 2048, 8));   /* no dash */
    TEST_ASSERT_EQUAL_INT(-1, range_parse(&rs, "1G-2G", 512, 2048, 8));  /* beyond cap */
}

static void test_range_make_regions(void)
{
    RangeSet rs;
    /* 4 regions of 4 KiB each over a 1 MiB device, 512B sectors, unit 8 */
    TEST_ASSERT_EQUAL_INT(0, range_make_regions(&rs, 4, 4096, 512, 2048, 8));
    TEST_ASSERT_EQUAL_UINT(4, rs.count);
    /* evenly spread: stride = 2048/4 = 512 sectors */
    TEST_ASSERT_EQUAL_UINT64(0,    rs.ranges[0].start_lba);
    TEST_ASSERT_EQUAL_UINT64(512,  rs.ranges[1].start_lba);
    TEST_ASSERT_EQUAL_UINT64(1024, rs.ranges[2].start_lba);
    TEST_ASSERT_EQUAL_UINT64(1536, rs.ranges[3].start_lba);
    /* each region 4KiB = 8 sectors = 1 unit; 4 regions => 4 units */
    TEST_ASSERT_EQUAL_UINT64(4, rs.total_units);
}

/* ---------- unit -> LBA mapping ---------- */
static void test_range_unit_lba_sequential(void)
{
    RangeSet rs;
    range_parse(&rs, "0-64K,128K-192K", 512, 1024*1024/512, 8);
    /* first range: units 0..15 -> lba 0,8,...,120 ; second range: units 16.. -> lba 256,264.. */
    TEST_ASSERT_EQUAL_UINT64(0,   range_unit_lba(&rs, 0));
    TEST_ASSERT_EQUAL_UINT64(8,   range_unit_lba(&rs, 1));
    TEST_ASSERT_EQUAL_UINT64(120, range_unit_lba(&rs, 15));
    TEST_ASSERT_EQUAL_UINT64(256, range_unit_lba(&rs, 16));   /* start of 2nd range */
    TEST_ASSERT_EQUAL_UINT64(264, range_unit_lba(&rs, 17));
}

/* ---------- permutation: full coverage + bijection ---------- */
static void check_permutation_bijection(U64 n, U32 round)
{
    unsigned char* seen = calloc(1, (size_t)n);
    U64 step;
    TEST_ASSERT_NOT_NULL(seen);
    for (step = 0; step < n; step++)
    {
        U64 v = range_permute(n, step, round);
        TEST_ASSERT_TRUE(v < n);          /* in range */
        TEST_ASSERT_EQUAL_UINT8(0, seen[v]); /* not seen before */
        seen[v] = 1;
    }
    /* every index covered exactly once */
    for (step = 0; step < n; step++) TEST_ASSERT_EQUAL_UINT8(1, seen[step]);
    free(seen);
}

static void test_range_permute_is_bijection(void)
{
    check_permutation_bijection(1, 0);
    check_permutation_bijection(2, 0);
    check_permutation_bijection(7, 3);      /* prime, not power of two */
    check_permutation_bijection(100, 1);
    check_permutation_bijection(1000, 5);
    check_permutation_bijection(4096, 2);   /* exact power of two */
}

static void test_range_permute_round_changes_order(void)
{
    /* different rounds should give different orderings (reshuffle between passes) */
    U64 n = 1000;
    U64 same = 0, i;
    for (i = 0; i < n; i++)
        if (range_permute(n, i, 1) == range_permute(n, i, 2)) same++;
    TEST_ASSERT_TRUE(same < n / 2);   /* mostly different */
}

void register_range_tests(void)
{
    RUN_TEST(test_parse_size_units);
    RUN_TEST(test_parse_size_bad);
    RUN_TEST(test_range_parse_whole);
    RUN_TEST(test_range_parse_explicit_bytes);
    RUN_TEST(test_range_parse_bad);
    RUN_TEST(test_range_make_regions);
    RUN_TEST(test_range_unit_lba_sequential);
    RUN_TEST(test_range_permute_is_bijection);
    RUN_TEST(test_range_permute_round_changes_order);
}
