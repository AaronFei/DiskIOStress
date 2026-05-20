#include <string.h>
#include <stdlib.h>
#include "unity.h"
#include "pattern.h"
#include "util.h"
#include "globals.h"

#define BUF_SZ (32 * SIZE_1K)   /* size large enough for all pattern paths */

/* The sector tag depends on gDiskIOInfo fields; set a 512B sector + seed. */
static void prepare_disk_info_for_pattern(void)
{
    gDiskIOInfo.sz_block = 512;
    gDiskIOInfo.seed     = 0xABCD1234;
}

/* Build a ThreadInfo_t with the metadata the tag scheme reads. */
static void make_thread_info(ThreadInfo_t* p, U32 block_count)
{
    memset(p, 0, sizeof(*p));
    p->id           = 7;
    p->cr_trunk     = 3;
    p->block_count  = block_count;
    p->pattern_type = PATTERN_RANDOM;
    p->workload     = WORKLOAD_RAND_WRC;
}

/* Deterministic payload fill. (generate_pattern with PATTERN_RANDOM always
 * writes a fixed 32 KB, so it can't be used on the small buffers below.) */
static void fill_payload(unsigned char* b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) b[i] = (unsigned char)(i * 7 + 1);
}

/* ---------- generate_pattern variants ---------- */

static void test_pattern_all_zero(void)
{
    unsigned char buf[BUF_SZ];
    memset(buf, 0xAA, sizeof(buf));
    generate_pattern(buf, sizeof(buf), PATTERN_ALLZERO);
    for (size_t i = 0; i < sizeof(buf); i++) TEST_ASSERT_EQUAL_HEX8(0x00, buf[i]);
}

static void test_pattern_all_one(void)
{
    unsigned char buf[BUF_SZ];
    memset(buf, 0x00, sizeof(buf));
    generate_pattern(buf, sizeof(buf), PATTERN_ALLONE);
    for (size_t i = 0; i < sizeof(buf); i++) TEST_ASSERT_EQUAL_HEX8(0xFF, buf[i]);
}

static void test_pattern_inc_byte(void)
{
    unsigned char buf[BUF_SZ];
    generate_pattern(buf, sizeof(buf), PATTERN_SEQU_INC_BYTE);
    /* element [i] should be i & 0xFF */
    for (size_t i = 0; i < 1024; i++) TEST_ASSERT_EQUAL_HEX8((unsigned char)i, buf[i]);
}

static void test_pattern_inc_dword(void)
{
    unsigned char buf[BUF_SZ];
    generate_pattern(buf, sizeof(buf), PATTERN_SEQU_INC_DWORD);
    U32* p = (U32*)buf;
    for (U32 i = 0; i < 32; i++) TEST_ASSERT_EQUAL_UINT32(i, p[i]);
}

static void test_pattern_working_one_walks_bits(void)
{
    unsigned char buf[BUF_SZ];
    generate_pattern(buf, sizeof(buf), PATTERN_WORKING_ONE);
    U32* p = (U32*)buf;
    /* first 32 dwords each have exactly one bit set, at position i */
    for (U32 i = 0; i < 32; i++) TEST_ASSERT_EQUAL_UINT32(1U << i, p[i]);
}

static void test_pattern_working_zero_walks_bits(void)
{
    unsigned char buf[BUF_SZ];
    generate_pattern(buf, sizeof(buf), PATTERN_WORKING_ZERO);
    U32* p = (U32*)buf;
    for (U32 i = 0; i < 32; i++) TEST_ASSERT_EQUAL_UINT32(~(1U << i), p[i]);
}

static void test_pattern_random_is_repeatable_with_same_seed(void)
{
    unsigned char a[BUF_SZ];
    unsigned char b[BUF_SZ];

    srand(0xCAFEBABE);
    generate_pattern(a, sizeof(a), PATTERN_RANDOM);

    srand(0xCAFEBABE);
    generate_pattern(b, sizeof(b), PATTERN_RANDOM);

    TEST_ASSERT_EQUAL_MEMORY(a, b, sizeof(a));
}

/* ---------- crc32 ---------- */

static void test_crc32_known_vector(void)
{
    /* CRC32/IEEE of "123456789" is 0xCBF43926 */
    crc32_init();
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926, crc32("123456789", 9));
}

/* ---------- stamp_sector_tags + verify_sector_tags ---------- */

static void test_stamp_then_verify_roundtrip(void)
{
    prepare_disk_info_for_pattern();
    ThreadInfo_t p; make_thread_info(&p, 4);

    unsigned char buf[4 * 512];
    fill_payload(buf, sizeof(buf));
    stamp_sector_tags(buf, &p, 0x2000, 5);

    U32 errsec = 0xFFFF;
    TEST_ASSERT_EQUAL_INT(TAG_OK, verify_sector_tags(buf, &p, 0x2000, 5, &errsec));
}

static void test_stamp_writes_expected_header_fields(void)
{
    prepare_disk_info_for_pattern();
    ThreadInfo_t p; make_thread_info(&p, 2);

    unsigned char buf[2 * 512];
    fill_payload(buf, sizeof(buf));
    stamp_sector_tags(buf, &p, 0x9000, 11);

    SectorTag_t* t0 = (SectorTag_t*)buf;
    SectorTag_t* t1 = (SectorTag_t*)(buf + 512);

    TEST_ASSERT_EQUAL_HEX32(DIOS_TAG_MAGIC, t0->magic);
    TEST_ASSERT_EQUAL_HEX32(0xABCD1234,     t0->seed);
    TEST_ASSERT_EQUAL_UINT64(0x9000,        t0->lba);
    TEST_ASSERT_EQUAL_UINT64(0x9001,        t1->lba);   /* per-sector LBA */
    TEST_ASSERT_EQUAL_UINT(11,              t0->write_loop);
    TEST_ASSERT_EQUAL_UINT(3,               t0->trunk_index);
    TEST_ASSERT_EQUAL_UINT(7,               t0->thread_id);
    TEST_ASSERT_EQUAL_UINT(PATTERN_RANDOM,  t0->pattern_type);
    TEST_ASSERT_EQUAL_UINT(WORKLOAD_RAND_WRC, t0->workload);
}

static void test_verify_detects_lba_misdirection(void)
{
    prepare_disk_info_for_pattern();
    ThreadInfo_t p; make_thread_info(&p, 2);

    unsigned char buf[2 * 512];
    fill_payload(buf, sizeof(buf));
    stamp_sector_tags(buf, &p, 0x2000, 1);

    /* verify expecting a different start LBA -> first sector mismatches */
    U32 errsec = 0xFFFF;
    TEST_ASSERT_EQUAL_INT(TAG_ERR_LBA, verify_sector_tags(buf, &p, 0x9999, 1, &errsec));
    TEST_ASSERT_EQUAL_UINT(0, errsec);
}

static void test_verify_detects_stale_generation(void)
{
    prepare_disk_info_for_pattern();
    ThreadInfo_t p; make_thread_info(&p, 1);

    unsigned char buf[512];
    fill_payload(buf, sizeof(buf));
    stamp_sector_tags(buf, &p, 0x100, 4);   /* written in loop 4 */

    /* reader expects loop 5 -> the data is one generation stale */
    TEST_ASSERT_EQUAL_INT(TAG_ERR_WRITE_LOOP, verify_sector_tags(buf, &p, 0x100, 5, NULL));
}

static void test_verify_detects_payload_bitrot(void)
{
    prepare_disk_info_for_pattern();
    ThreadInfo_t p; make_thread_info(&p, 1);

    unsigned char buf[512];
    fill_payload(buf, sizeof(buf));
    stamp_sector_tags(buf, &p, 0x100, 1);

    buf[200] ^= 0xFF;   /* flip a byte in the payload region */

    TEST_ASSERT_EQUAL_INT(TAG_ERR_PAYLOAD_CRC, verify_sector_tags(buf, &p, 0x100, 1, NULL));
}

static void test_verify_detects_missing_magic(void)
{
    prepare_disk_info_for_pattern();
    ThreadInfo_t p; make_thread_info(&p, 1);

    unsigned char buf[512];
    memset(buf, 0x00, sizeof(buf));   /* never stamped: looks like blank media */

    TEST_ASSERT_EQUAL_INT(TAG_ERR_MAGIC, verify_sector_tags(buf, &p, 0x100, 0, NULL));
}

static void test_verify_detects_cross_run_seed(void)
{
    prepare_disk_info_for_pattern();
    ThreadInfo_t p; make_thread_info(&p, 1);

    unsigned char buf[512];
    fill_payload(buf, sizeof(buf));
    stamp_sector_tags(buf, &p, 0x100, 1);

    gDiskIOInfo.seed = 0xDEADBEEF;   /* simulate verifying under a different run */
    TEST_ASSERT_EQUAL_INT(TAG_ERR_SEED, verify_sector_tags(buf, &p, 0x100, 1, NULL));
}

/* ---------- addr (self-describing) pattern ---------- */

static void test_fill_addr_records(void)
{
    prepare_disk_info_for_pattern();   /* sz_block = 512 */
    unsigned char buf[2 * 512];
    U32 tagsz = (U32)sizeof(SectorTag_t);
    U64 lba; U32 off, gen;

    memset(buf, 0, sizeof(buf));
    fill_unit_payload(buf, 2, 512, 0x1000, 7, PATTERN_ADDR);

    /* sector 0: first record at payload start carries lba=0x1000, byte_off=tagsz, gen=7 */
    memcpy(&lba, buf + tagsz,      8);
    memcpy(&off, buf + tagsz + 8,  4);
    memcpy(&gen, buf + tagsz + 12, 4);
    TEST_ASSERT_EQUAL_UINT64(0x1000, lba);
    TEST_ASSERT_EQUAL_UINT(tagsz, off);
    TEST_ASSERT_EQUAL_UINT(7, gen);

    /* sector 1: lba advances to 0x1001 */
    memcpy(&lba, buf + 512 + tagsz, 8);
    TEST_ASSERT_EQUAL_UINT64(0x1001, lba);
}

static void test_fill_addr_then_verify_roundtrip(void)
{
    prepare_disk_info_for_pattern();
    ThreadInfo_t p; make_thread_info(&p, 2);
    p.pattern_type = PATTERN_ADDR;

    unsigned char buf[2 * 512];
    fill_unit_payload(buf, 2, 512, 0x4000, 9, PATTERN_ADDR);
    stamp_sector_tags(buf, &p, 0x4000, 9);

    TEST_ASSERT_EQUAL_INT(TAG_OK, verify_sector_tags(buf, &p, 0x4000, 9, NULL));

    buf[200] ^= 0xFF;   /* corrupt payload -> CRC catches it */
    TEST_ASSERT_EQUAL_INT(TAG_ERR_PAYLOAD_CRC, verify_sector_tags(buf, &p, 0x4000, 9, NULL));
}

void register_pattern_tests(void)
{
    RUN_TEST(test_fill_addr_records);
    RUN_TEST(test_fill_addr_then_verify_roundtrip);
    RUN_TEST(test_pattern_all_zero);
    RUN_TEST(test_pattern_all_one);
    RUN_TEST(test_pattern_inc_byte);
    RUN_TEST(test_pattern_inc_dword);
    RUN_TEST(test_pattern_working_one_walks_bits);
    RUN_TEST(test_pattern_working_zero_walks_bits);
    RUN_TEST(test_pattern_random_is_repeatable_with_same_seed);
    RUN_TEST(test_crc32_known_vector);
    RUN_TEST(test_stamp_then_verify_roundtrip);
    RUN_TEST(test_stamp_writes_expected_header_fields);
    RUN_TEST(test_verify_detects_lba_misdirection);
    RUN_TEST(test_verify_detects_stale_generation);
    RUN_TEST(test_verify_detects_payload_bitrot);
    RUN_TEST(test_verify_detects_missing_magic);
    RUN_TEST(test_verify_detects_cross_run_seed);
}
