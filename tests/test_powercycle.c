#include <string.h>
#include "unity.h"
#include "powercycle.h"
#include "pattern.h"
#include "util.h"
#include "globals.h"

#define BLK 512
#define UB  2                 /* unit_blocks */
#define UNIT_BYTES (UB * BLK)

static ThreadInfo_t TI;

static void pc_test_setup(void)
{
    crc32_init();
    gDiskIOInfo.sz_block = BLK;
    gDiskIOInfo.seed     = 0x5EED5EED;
    memset(&TI, 0, sizeof(TI));
    TI.id = 0;
    TI.pattern_type = PATTERN_RANDOM;
    TI.workload = WORKLOAD_RAND_WRC;
    TI.block_count = UB;
}

/* fill payload + stamp a unit at generation `gen` for the given lba */
static void make_unit(unsigned char* buf, U64 lba, U32 gen)
{
    U32 i;
    for (i = 0; i < UNIT_BYTES; i++) buf[i] = (unsigned char)(i * 7 + gen);
    stamp_sector_tags(buf, &TI, lba, gen);
}

/* ---- durable region (pos < D): must be generation G ---- */

static void test_durable_current_gen_is_newest(void)
{
    unsigned char buf[UNIT_BYTES];
    PcReport r; memset(&r, 0, sizeof(r));
    pc_test_setup();

    make_unit(buf, 0x1000, 5);
    /* pos 0 < D=10 ; G=5 */
    pc_classify(buf, &TI, 0x1000, 0, 5, 10, 10, UB, &r);

    TEST_ASSERT_EQUAL_UINT64(UB, r.newest);
    TEST_ASSERT_EQUAL_UINT64(0, r.stale);
    TEST_ASSERT_EQUAL_UINT64(0, r.corrupt);
}

static void test_durable_old_gen_is_stale_failure(void)
{
    unsigned char buf[UNIT_BYTES];
    PcReport r; memset(&r, 0, sizeof(r));
    pc_test_setup();

    /* data is G-1 but it sits in the durable region where G is required */
    make_unit(buf, 0x1000, 4);
    pc_classify(buf, &TI, 0x1000, 0, 5, 10, 10, UB, &r);

    TEST_ASSERT_EQUAL_UINT64(UB, r.stale);       /* durability failure */
    TEST_ASSERT_EQUAL_UINT64(0, r.newest);
}

static void test_durable_payload_bitrot_is_corrupt(void)
{
    unsigned char buf[UNIT_BYTES];
    PcReport r; memset(&r, 0, sizeof(r));
    pc_test_setup();

    make_unit(buf, 0x1000, 5);
    buf[100] ^= 0xFF;                            /* flip a payload byte */
    pc_classify(buf, &TI, 0x1000, 0, 5, 10, 10, UB, &r);

    TEST_ASSERT_EQUAL_UINT64(UB, r.corrupt);
    TEST_ASSERT_EQUAL_UINT64(0, r.newest);
}

static void test_durable_misdirected_lba_is_corrupt(void)
{
    unsigned char buf[UNIT_BYTES];
    PcReport r; memset(&r, 0, sizeof(r));
    pc_test_setup();

    make_unit(buf, 0x2000, 5);                   /* stamped for a different lba */
    pc_classify(buf, &TI, 0x1000, 0, 5, 10, 10, UB, &r);

    TEST_ASSERT_EQUAL_UINT64(UB, r.corrupt);
}

/* ---- at-risk window (D <= pos < S): G or G-1 acceptable ---- */

static void test_atrisk_current_gen_is_newest(void)
{
    unsigned char buf[UNIT_BYTES];
    PcReport r; memset(&r, 0, sizeof(r));
    pc_test_setup();

    make_unit(buf, 0x1000, 5);
    pc_classify(buf, &TI, 0x1000, 12, 5, 10, 16, UB, &r);  /* pos in [D=10,S=16) */
    TEST_ASSERT_EQUAL_UINT64(UB, r.newest);
}

static void test_atrisk_prev_gen_is_acceptable(void)
{
    unsigned char buf[UNIT_BYTES];
    PcReport r; memset(&r, 0, sizeof(r));
    pc_test_setup();

    make_unit(buf, 0x1000, 4);                   /* in-flight write didn't land: still G-1 */
    pc_classify(buf, &TI, 0x1000, 12, 5, 10, 16, UB, &r);
    TEST_ASSERT_EQUAL_UINT64(UB, r.acceptable);
    TEST_ASSERT_EQUAL_UINT64(0, r.corrupt);
    TEST_ASSERT_EQUAL_UINT64(0, r.stale);
}

static void test_atrisk_blank_first_pass_is_acceptable(void)
{
    unsigned char buf[UNIT_BYTES];
    PcReport r; memset(&r, 0, sizeof(r));
    pc_test_setup();

    memset(buf, 0, sizeof(buf));                 /* never written, G==1 first pass */
    pc_classify(buf, &TI, 0x1000, 2, 1, 0, 8, UB, &r);
    TEST_ASSERT_EQUAL_UINT64(UB, r.acceptable);
}

static void test_atrisk_two_gens_old_is_stale(void)
{
    unsigned char buf[UNIT_BYTES];
    PcReport r; memset(&r, 0, sizeof(r));
    pc_test_setup();

    make_unit(buf, 0x1000, 3);                   /* G-2 in an at-risk slot expecting G or G-1 */
    pc_classify(buf, &TI, 0x1000, 12, 5, 10, 16, UB, &r);
    TEST_ASSERT_EQUAL_UINT64(UB, r.stale);
}

/* ---- not-yet-written this pass (pos >= S): expect G-1 ---- */

static void test_notyet_prev_gen_ok(void)
{
    unsigned char buf[UNIT_BYTES];
    PcReport r; memset(&r, 0, sizeof(r));
    pc_test_setup();

    make_unit(buf, 0x1000, 4);                   /* previous pass value */
    pc_classify(buf, &TI, 0x1000, 20, 5, 10, 16, UB, &r);  /* pos >= S */
    TEST_ASSERT_EQUAL_UINT64(UB, r.prev_ok);
}

static void test_notyet_current_gen_is_stale(void)
{
    unsigned char buf[UNIT_BYTES];
    PcReport r; memset(&r, 0, sizeof(r));
    pc_test_setup();

    /* a unit we did NOT submit this pass reads as G -> unexpected */
    make_unit(buf, 0x1000, 5);
    pc_classify(buf, &TI, 0x1000, 20, 5, 10, 16, UB, &r);
    TEST_ASSERT_EQUAL_UINT64(UB, r.stale);
}

void register_powercycle_tests(void)
{
    RUN_TEST(test_durable_current_gen_is_newest);
    RUN_TEST(test_durable_old_gen_is_stale_failure);
    RUN_TEST(test_durable_payload_bitrot_is_corrupt);
    RUN_TEST(test_durable_misdirected_lba_is_corrupt);
    RUN_TEST(test_atrisk_current_gen_is_newest);
    RUN_TEST(test_atrisk_prev_gen_is_acceptable);
    RUN_TEST(test_atrisk_blank_first_pass_is_acceptable);
    RUN_TEST(test_atrisk_two_gens_old_is_stale);
    RUN_TEST(test_notyet_prev_gen_ok);
    RUN_TEST(test_notyet_current_gen_is_stale);
}
