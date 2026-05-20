#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "unity.h"
#include "uring_engine.h"

#define TMP_PATH   "/tmp/dios_uring_test.bin"
#define NBLK       8        /* sectors in the test file */
#define QD         4

/* Open an engine on a fresh temp file (no O_DIRECT so the test is hermetic and
 * works on any fs). Returns NULL if io_uring is unavailable in this env. */
static UringEngine* open_tmp_engine(void)
{
    int fd = open(TMP_PATH, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return NULL;
    /* size the file to NBLK * 512 */
    if (ftruncate(fd, (off_t)NBLK * 512) != 0) { close(fd); return NULL; }
    close(fd);
    return uring_open(TMP_PATH, QD, URING_WRITE);
}

static void test_uring_open_reports_geometry(void)
{
    UringEngine* e = open_tmp_engine();
    if (!e) { TEST_IGNORE_MESSAGE("io_uring unavailable in this environment"); return; }

    TEST_ASSERT_EQUAL_UINT(512, uring_block_size(e));
    TEST_ASSERT_EQUAL_UINT64(NBLK, uring_capacity_blocks(e));
    TEST_ASSERT_EQUAL_UINT(QD, uring_queue_depth(e));

    uring_close(e);
    remove(TMP_PATH);
}

static void test_uring_write_then_read_roundtrip(void)
{
    UringEngine* e = open_tmp_engine();
    if (!e) { TEST_IGNORE_MESSAGE("io_uring unavailable in this environment"); return; }

    U32 blk = uring_block_size(e);
    unsigned char* wbuf = uring_alloc_buffer(e, (size_t)NBLK * blk);
    unsigned char* rbuf = uring_alloc_buffer(e, (size_t)NBLK * blk);
    UringCqe cqes[QD];
    U32 i;
    int queued, reaped, total;

    TEST_ASSERT_NOT_NULL(wbuf);
    TEST_ASSERT_NOT_NULL(rbuf);

    /* distinct content per sector */
    for (i = 0; i < NBLK; i++) memset(wbuf + (size_t)i * blk, (int)(0x10 + i), blk);

    /* submit NBLK single-sector writes, bounded by QD */
    total = 0;
    for (i = 0; i < NBLK; i++)
    {
        while (uring_queue_write(e, wbuf + (size_t)i * blk, i, 1, 0x100 + i, 0) < 0)
        {
            uring_submit(e);
            reaped = uring_reap(e, cqes, QD, 1);
            TEST_ASSERT_GREATER_THAN_INT(0, reaped);
            total += reaped;
        }
    }
    uring_submit(e);
    while (total < (int)NBLK)
    {
        reaped = uring_reap(e, cqes, QD, 1);
        TEST_ASSERT_GREATER_THAN_INT(0, reaped);
        total += reaped;
    }
    (void)queued;

    TEST_ASSERT_EQUAL_INT(0, uring_flush(e));

    /* read it all back */
    total = 0;
    for (i = 0; i < NBLK; i++)
    {
        while (uring_queue_read(e, rbuf + (size_t)i * blk, i, 1, 0x200 + i) < 0)
        {
            uring_submit(e);
            reaped = uring_reap(e, cqes, QD, 1);
            TEST_ASSERT_GREATER_THAN_INT(0, reaped);
            total += reaped;
        }
    }
    uring_submit(e);
    while (total < (int)NBLK)
    {
        reaped = uring_reap(e, cqes, QD, 1);
        TEST_ASSERT_GREATER_THAN_INT(0, reaped);
        total += reaped;
    }

    TEST_ASSERT_EQUAL_MEMORY(wbuf, rbuf, (size_t)NBLK * blk);

    uring_free_buffer(wbuf);
    uring_free_buffer(rbuf);
    uring_close(e);
    remove(TMP_PATH);
}

static void test_uring_completion_result_is_byte_count(void)
{
    UringEngine* e = open_tmp_engine();
    if (!e) { TEST_IGNORE_MESSAGE("io_uring unavailable in this environment"); return; }

    U32 blk = uring_block_size(e);
    unsigned char* buf = uring_alloc_buffer(e, (size_t)2 * blk);
    UringCqe cqes[QD];
    int reaped;

    TEST_ASSERT_NOT_NULL(buf);
    memset(buf, 0x5A, (size_t)2 * blk);

    TEST_ASSERT_EQUAL_INT(0, uring_queue_write(e, buf, 0, 2, 0xABCD, 0));
    TEST_ASSERT_GREATER_THAN_INT(0, uring_submit(e));
    reaped = uring_reap(e, cqes, QD, 1);

    TEST_ASSERT_EQUAL_INT(1, reaped);
    TEST_ASSERT_EQUAL_UINT64(0xABCD, cqes[0].user_data);
    TEST_ASSERT_EQUAL_INT((int)(2 * blk), cqes[0].result);  /* bytes transferred */

    uring_free_buffer(buf);
    uring_close(e);
    remove(TMP_PATH);
}

void register_uring_tests(void)
{
    RUN_TEST(test_uring_open_reports_geometry);
    RUN_TEST(test_uring_write_then_read_roundtrip);
    RUN_TEST(test_uring_completion_result_is_byte_count);
}
