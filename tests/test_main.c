#include "unity.h"
#include "util.h"

/* Set-up / tear-down hooks Unity requires (one shared pair across all suites). */
void setUp(void)    { crc32_init(); }
void tearDown(void) {}

/* Per-suite registration functions (defined in test_*.c). */
void register_util_tests(void);
void register_config_tests(void);
void register_pattern_tests(void);
void register_uring_tests(void);
void register_range_tests(void);
void register_journal_tests(void);
void register_powercycle_tests(void);

int main(void)
{
    UNITY_BEGIN();
    register_util_tests();
    register_config_tests();
    register_pattern_tests();
    register_uring_tests();
    register_range_tests();
    register_journal_tests();
    register_powercycle_tests();
    return UNITY_END();
}
