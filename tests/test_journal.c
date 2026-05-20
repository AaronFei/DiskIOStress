#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "journal.h"

#define JPATH "/tmp/dios_test.journal"

static void test_journal_save_load_roundtrip(void)
{
    Journal a, b;
    memset(&a, 0, sizeof(a));
    strcpy(a.device, "/dev/sdb");
    a.seed = 0xDEADBEEF;
    a.blk = 512;
    a.unit_blocks = 128;
    strcpy(a.ranges, "0-64K,128K-192K");
    strcpy(a.access, "random");
    strcpy(a.durability, "plp");
    a.gen = 42;
    a.total_units = 100000;
    a.durable_units = 73210;
    a.submitted_units = 73242;
    a.cycle = 7;

    TEST_ASSERT_EQUAL_INT(0, journal_save(JPATH, &a));
    TEST_ASSERT_EQUAL_INT(0, journal_load(JPATH, &b));

    TEST_ASSERT_EQUAL_STRING("/dev/sdb", b.device);
    TEST_ASSERT_EQUAL_HEX32(0xDEADBEEF, b.seed);
    TEST_ASSERT_EQUAL_UINT(512, b.blk);
    TEST_ASSERT_EQUAL_UINT(128, b.unit_blocks);
    TEST_ASSERT_EQUAL_STRING("0-64K,128K-192K", b.ranges);
    TEST_ASSERT_EQUAL_STRING("random", b.access);
    TEST_ASSERT_EQUAL_STRING("plp", b.durability);
    TEST_ASSERT_EQUAL_UINT(42, b.gen);
    TEST_ASSERT_EQUAL_UINT64(100000, b.total_units);
    TEST_ASSERT_EQUAL_UINT64(73210, b.durable_units);
    TEST_ASSERT_EQUAL_UINT64(73242, b.submitted_units);
    TEST_ASSERT_EQUAL_UINT(7, b.cycle);

    remove(JPATH);
}

static void test_journal_load_missing(void)
{
    Journal j;
    TEST_ASSERT_EQUAL_INT(-1, journal_load("/tmp/__no_such_dios_journal__", &j));
}

static void test_journal_save_is_atomic_no_tmp_left(void)
{
    Journal a;
    char tmp[64];
    FILE* fp;
    memset(&a, 0, sizeof(a));
    strcpy(a.device, "/dev/sdz");
    strcpy(a.access, "seq");
    strcpy(a.durability, "volatile");

    TEST_ASSERT_EQUAL_INT(0, journal_save(JPATH, &a));

    /* the .tmp staging file must have been renamed away */
    snprintf(tmp, sizeof(tmp), "%s.tmp", JPATH);
    fp = fopen(tmp, "r");
    TEST_ASSERT_NULL(fp);
    if (fp) fclose(fp);

    remove(JPATH);
}

void register_journal_tests(void)
{
    RUN_TEST(test_journal_save_load_roundtrip);
    RUN_TEST(test_journal_load_missing);
    RUN_TEST(test_journal_save_is_atomic_no_tmp_left);
}
