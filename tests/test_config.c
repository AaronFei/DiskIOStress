#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "config.h"

static void test_set_defaults(void)
{
    Config_t cfg;
    config_set_defaults(&cfg);

    TEST_ASSERT_EQUAL_UINT(MAX_THREAD_NUM, cfg.nr_thread);
    TEST_ASSERT_EQUAL_UINT(MAX_LOOP_NUM,   cfg.nr_loop);
    TEST_ASSERT_EQUAL_UINT64(MAX_TRUNK_SIZE, cfg.sz_trunk);
    TEST_ASSERT_EQUAL_UINT(MAX_TEST_TIME,  cfg.test_time);
    TEST_ASSERT_EQUAL_INT(DEFAULT_PATTERN, (int)cfg.pattern);
    TEST_ASSERT_EQUAL_INT(DEFAULT_WORKLOAD, (int)cfg.workload);
    TEST_ASSERT_EQUAL_INT(0, cfg.seed_set);
    TEST_ASSERT_EQUAL_STRING("", cfg.config_path);
}

static void test_parse_enum_pattern_known(void)
{
    TEST_ASSERT_EQUAL_INT(PATTERN_RANDOM,        parse_enum("random",       gPatternMap));
    TEST_ASSERT_EQUAL_INT(PATTERN_ALLZERO,       parse_enum("zero",         gPatternMap));
    TEST_ASSERT_EQUAL_INT(PATTERN_ALLONE,        parse_enum("one",          gPatternMap));
    TEST_ASSERT_EQUAL_INT(PATTERN_SEQU_INC_DWORD,parse_enum("inc_dword",    gPatternMap));
    TEST_ASSERT_EQUAL_INT(PATTERN_WORKING_ZERO,  parse_enum("working_zero", gPatternMap));
}

static void test_parse_enum_pattern_case_insensitive(void)
{
    TEST_ASSERT_EQUAL_INT(PATTERN_RANDOM, parse_enum("RANDOM", gPatternMap));
    TEST_ASSERT_EQUAL_INT(PATTERN_RANDOM, parse_enum("Random", gPatternMap));
}

static void test_parse_enum_pattern_unknown(void)
{
    TEST_ASSERT_EQUAL_INT(-1, parse_enum("not_a_pattern", gPatternMap));
    TEST_ASSERT_EQUAL_INT(-1, parse_enum("",              gPatternMap));
}

static void test_parse_enum_workload(void)
{
    /* canonical names */
    TEST_ASSERT_EQUAL_INT(WORKLOAD_RAND_WRC,  parse_enum("rand-verify",   gWorkloadMap));
    TEST_ASSERT_EQUAL_INT(WORKLOAD_SEQ_WRC,   parse_enum("seq-verify",    gWorkloadMap));
    TEST_ASSERT_EQUAL_INT(WORKLOAD_SEQ_WRRC,  parse_enum("seq-verify-2x", gWorkloadMap));
    TEST_ASSERT_EQUAL_INT(WORKLOAD_SEQ_W1RCN, parse_enum("retention",     gWorkloadMap));
    TEST_ASSERT_EQUAL_INT(WORKLOAD_MIX_RW,    parse_enum("concurrent-rw", gWorkloadMap));
    /* legacy aliases must still parse */
    TEST_ASSERT_EQUAL_INT(WORKLOAD_RAND_WRC,  parse_enum("rand_wrc",  gWorkloadMap));
    TEST_ASSERT_EQUAL_INT(WORKLOAD_SEQ_WRC,   parse_enum("seq_wrc",   gWorkloadMap));
    TEST_ASSERT_EQUAL_INT(WORKLOAD_SEQ_WRRC,  parse_enum("seq_wrrc",  gWorkloadMap));
    TEST_ASSERT_EQUAL_INT(WORKLOAD_SEQ_W1RCN, parse_enum("seq_w1rcn", gWorkloadMap));
    TEST_ASSERT_EQUAL_INT(WORKLOAD_MIX_RW,    parse_enum("mix_rw",    gWorkloadMap));
    TEST_ASSERT_EQUAL_INT(-1,                 parse_enum("nope",      gWorkloadMap));
}

static void test_enum_name_roundtrip(void)
{
    TEST_ASSERT_EQUAL_STRING("random",       enum_name(PATTERN_RANDOM,    gPatternMap));
    TEST_ASSERT_EQUAL_STRING("zero",         enum_name(PATTERN_ALLZERO,   gPatternMap));
    /* enum_name returns the canonical (first) name, not a legacy alias */
    TEST_ASSERT_EQUAL_STRING("rand-verify",  enum_name(WORKLOAD_RAND_WRC, gWorkloadMap));
    TEST_ASSERT_EQUAL_STRING("concurrent-rw",enum_name(WORKLOAD_MIX_RW,   gWorkloadMap));
    TEST_ASSERT_EQUAL_STRING("?",            enum_name(9999,              gPatternMap));
}

/* ---------- config_apply_kv ---------- */

static void test_apply_kv_threads(void)
{
    Config_t c;
    config_set_defaults(&c);
    TEST_ASSERT_EQUAL_INT(0, config_apply_kv(&c, "threads", "32", "test", 1));
    TEST_ASSERT_EQUAL_UINT(32, c.nr_thread);
}

static void test_apply_kv_loops_and_test_time(void)
{
    Config_t c;
    config_set_defaults(&c);
    TEST_ASSERT_EQUAL_INT(0, config_apply_kv(&c, "loops",     "500", "t", 1));
    TEST_ASSERT_EQUAL_INT(0, config_apply_kv(&c, "test_time", "60",  "t", 2));
    TEST_ASSERT_EQUAL_UINT(500, c.nr_loop);
    TEST_ASSERT_EQUAL_UINT(60,  c.test_time);
}

static void test_apply_kv_trunk_size_is_mb(void)
{
    Config_t c;
    config_set_defaults(&c);
    TEST_ASSERT_EQUAL_INT(0, config_apply_kv(&c, "trunk_size", "5", "t", 1));
    TEST_ASSERT_EQUAL_UINT64(5ULL * SIZE_1M, c.sz_trunk);
}

static void test_apply_kv_pattern_and_workload(void)
{
    Config_t c;
    config_set_defaults(&c);
    TEST_ASSERT_EQUAL_INT(0, config_apply_kv(&c, "pattern",  "inc_byte", "t", 1));
    TEST_ASSERT_EQUAL_INT(0, config_apply_kv(&c, "workload", "seq_wrc",  "t", 2));
    TEST_ASSERT_EQUAL_INT(PATTERN_SEQU_INC_BYTE, (int)c.pattern);
    TEST_ASSERT_EQUAL_INT(WORKLOAD_SEQ_WRC,      (int)c.workload);
}

static void test_apply_kv_seed_nonzero_sets_flag(void)
{
    Config_t c;
    config_set_defaults(&c);
    TEST_ASSERT_EQUAL_INT(0, config_apply_kv(&c, "seed", "0xDEADBEEF", "t", 1));
    TEST_ASSERT_EQUAL_UINT(0xDEADBEEF, c.seed);
    TEST_ASSERT_EQUAL_INT(1, c.seed_set);
}

static void test_apply_kv_seed_zero_means_auto(void)
{
    Config_t c;
    config_set_defaults(&c);
    TEST_ASSERT_EQUAL_INT(0, config_apply_kv(&c, "seed", "0", "t", 1));
    TEST_ASSERT_EQUAL_INT(0, c.seed_set);
}

static void test_apply_kv_unknown_key(void)
{
    Config_t c;
    config_set_defaults(&c);
    fprintf(stderr, "[expected stderr message follows] ");
    TEST_ASSERT_EQUAL_INT(-1, config_apply_kv(&c, "nope", "1", "t", 1));
}

static void test_apply_kv_unknown_pattern_value(void)
{
    Config_t c;
    config_set_defaults(&c);
    fprintf(stderr, "[expected stderr message follows] ");
    TEST_ASSERT_EQUAL_INT(-1, config_apply_kv(&c, "pattern", "moo", "t", 1));
}

/* ---------- config_load_file ---------- */

static void test_load_file_valid(void)
{
    const char* path = "/tmp/diskiostress_test_ok.ini";
    FILE* fp = fopen(path, "w");
    TEST_ASSERT_NOT_NULL(fp);
    fputs("# comment line\n"
          "threads = 16\n"
          "loops=5\n"
          "  pattern   =   inc_dword  \n"
          "workload=seq_wrrc ; trailing comment\n"
          "seed = 0x1234\n",
          fp);
    fclose(fp);

    Config_t c;
    config_set_defaults(&c);
    TEST_ASSERT_EQUAL_INT(0, config_load_file(path, &c));
    TEST_ASSERT_EQUAL_UINT(16, c.nr_thread);
    TEST_ASSERT_EQUAL_UINT(5,  c.nr_loop);
    TEST_ASSERT_EQUAL_INT(PATTERN_SEQU_INC_DWORD, (int)c.pattern);
    TEST_ASSERT_EQUAL_INT(WORKLOAD_SEQ_WRRC,      (int)c.workload);
    TEST_ASSERT_EQUAL_UINT(0x1234, c.seed);
    TEST_ASSERT_EQUAL_INT(1, c.seed_set);
    TEST_ASSERT_EQUAL_STRING(path, c.config_path);
    remove(path);
}

static void test_load_file_missing(void)
{
    Config_t c;
    config_set_defaults(&c);
    fprintf(stderr, "[expected stderr message follows] ");
    TEST_ASSERT_EQUAL_INT(-1, config_load_file("/tmp/__no_such_diskiostress_cfg__", &c));
}

void register_config_tests(void)
{
    RUN_TEST(test_set_defaults);
    RUN_TEST(test_parse_enum_pattern_known);
    RUN_TEST(test_parse_enum_pattern_case_insensitive);
    RUN_TEST(test_parse_enum_pattern_unknown);
    RUN_TEST(test_parse_enum_workload);
    RUN_TEST(test_enum_name_roundtrip);
    RUN_TEST(test_apply_kv_threads);
    RUN_TEST(test_apply_kv_loops_and_test_time);
    RUN_TEST(test_apply_kv_trunk_size_is_mb);
    RUN_TEST(test_apply_kv_pattern_and_workload);
    RUN_TEST(test_apply_kv_seed_nonzero_sets_flag);
    RUN_TEST(test_apply_kv_seed_zero_means_auto);
    RUN_TEST(test_apply_kv_unknown_key);
    RUN_TEST(test_apply_kv_unknown_pattern_value);
    RUN_TEST(test_load_file_valid);
    RUN_TEST(test_load_file_missing);
}
