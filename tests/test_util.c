#include <string.h>
#include "unity.h"
#include "util.h"

/* ---------- hex2dec ---------- */
static void test_hex2dec_simple(void)
{
    char b[] = "100";
    TEST_ASSERT_EQUAL_UINT64(0x100, hex2dec(b));
}

static void test_hex2dec_uppercase(void)
{
    char b[] = "DEADBEEF";
    TEST_ASSERT_EQUAL_UINT64(0xDEADBEEFULL, hex2dec(b));
}

static void test_hex2dec_mixed_case(void)
{
    char b[] = "AbCdEf";
    TEST_ASSERT_EQUAL_UINT64(0xABCDEFULL, hex2dec(b));
}

static void test_hex2dec_zero(void)
{
    char b[] = "0";
    TEST_ASSERT_EQUAL_UINT64(0, hex2dec(b));
}

static void test_hex2dec_lowercases_input(void)
{
    /* hex2dec mutates the buffer to lowercase. */
    char b[] = "ABCD";
    hex2dec(b);
    TEST_ASSERT_EQUAL_STRING("abcd", b);
}

/* ---------- toLowerCase ---------- */
static void test_to_lower_case_basic(void)
{
    char dst[16] = {0};
    char src[]   = "Hello123";
    toLowerCase(src, dst, (int)strlen(src));
    TEST_ASSERT_EQUAL_STRING("hello123", dst);
}

/* ---------- str_trim ---------- */
static void test_str_trim_both_sides(void)
{
    char s[] = "   hello   ";
    TEST_ASSERT_EQUAL_STRING("hello", str_trim(s));
}

static void test_str_trim_tabs_and_newlines(void)
{
    char s[] = "\t\n  key  \r\n";
    TEST_ASSERT_EQUAL_STRING("key", str_trim(s));
}

static void test_str_trim_empty(void)
{
    char s[] = "    ";
    TEST_ASSERT_EQUAL_STRING("", str_trim(s));
}

static void test_str_trim_no_whitespace(void)
{
    char s[] = "intact";
    TEST_ASSERT_EQUAL_STRING("intact", str_trim(s));
}

/* ---------- parse_amount (size vs duration auto-detect) ---------- */
static void test_parse_amount_bytes(void)
{
    U64 v; int t;
    TEST_ASSERT_EQUAL_INT(0, parse_amount("64M", &v, &t));
    TEST_ASSERT_EQUAL_UINT64(64ULL * 1024 * 1024, v); TEST_ASSERT_EQUAL_INT(0, t);
    TEST_ASSERT_EQUAL_INT(0, parse_amount("2G", &v, &t));
    TEST_ASSERT_EQUAL_UINT64(2ULL * 1024 * 1024 * 1024, v); TEST_ASSERT_EQUAL_INT(0, t);
    TEST_ASSERT_EQUAL_INT(0, parse_amount("4096", &v, &t));   /* bare number = bytes */
    TEST_ASSERT_EQUAL_UINT64(4096, v); TEST_ASSERT_EQUAL_INT(0, t);
}

static void test_parse_amount_duration(void)
{
    U64 v; int t;
    TEST_ASSERT_EQUAL_INT(0, parse_amount("30s", &v, &t));
    TEST_ASSERT_EQUAL_UINT64(30, v); TEST_ASSERT_EQUAL_INT(1, t);
    TEST_ASSERT_EQUAL_INT(0, parse_amount("2h", &v, &t));
    TEST_ASSERT_EQUAL_UINT64(7200, v); TEST_ASSERT_EQUAL_INT(1, t);
}

static void test_parse_amount_m_vs_M(void)
{
    U64 v; int t;
    /* lowercase m = minutes (time), uppercase M = megabytes (size) */
    TEST_ASSERT_EQUAL_INT(0, parse_amount("5m", &v, &t));
    TEST_ASSERT_EQUAL_UINT64(300, v); TEST_ASSERT_EQUAL_INT(1, t);
    TEST_ASSERT_EQUAL_INT(0, parse_amount("5M", &v, &t));
    TEST_ASSERT_EQUAL_UINT64(5ULL * 1024 * 1024, v); TEST_ASSERT_EQUAL_INT(0, t);
}

void register_util_tests(void)
{
    RUN_TEST(test_parse_amount_bytes);
    RUN_TEST(test_parse_amount_duration);
    RUN_TEST(test_parse_amount_m_vs_M);
    RUN_TEST(test_hex2dec_simple);
    RUN_TEST(test_hex2dec_uppercase);
    RUN_TEST(test_hex2dec_mixed_case);
    RUN_TEST(test_hex2dec_zero);
    RUN_TEST(test_hex2dec_lowercases_input);
    RUN_TEST(test_to_lower_case_basic);
    RUN_TEST(test_str_trim_both_sides);
    RUN_TEST(test_str_trim_tabs_and_newlines);
    RUN_TEST(test_str_trim_empty);
    RUN_TEST(test_str_trim_no_whitespace);
}
