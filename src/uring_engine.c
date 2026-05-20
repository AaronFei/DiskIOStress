#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/fs.h>
#include <liburing.h>
#include "uring_engine.h"

#ifndef RWF_DSYNC
#define RWF_DSYNC 0x00000002
#endif

#define URING_BUF_ALIGN 4096

struct UringEngine
{
    struct io_uring ring;
    int  fd;
    U32  qd;
    U32  blk;          /* logical block size (bytes) */
    U64  cap_blocks;   /* capacity in sectors        */
};

UringEngine* uring_open(const char* path, U32 queue_depth, U32 flags)
{
    UringEngine* e;
    int oflags = (flags & URING_WRITE) ? O_RDWR : O_RDONLY;
    U32 ssz = 0;
    U64 bytes = 0;
    struct stat st;

    if (queue_depth == 0) queue_depth = 1;

    e = (UringEngine*)calloc(1, sizeof(*e));
    if (!e) return NULL;

    if (flags & URING_DIRECT) oflags |= O_DIRECT;

    e->fd = open(path, oflags);
    if (e->fd < 0)
    {
        free(e);
        return NULL;
    }

    /* Block device: query sector size + capacity via ioctl.
     * Regular file (tests): fall back to 512B sectors + st_size. */
    if (ioctl(e->fd, BLKSSZGET, &ssz) == 0 && ssz > 0)
    {
        e->blk = ssz;
        if (ioctl(e->fd, BLKGETSIZE64, &bytes) != 0) bytes = 0;
        e->cap_blocks = bytes / e->blk;
    }
    else
    {
        e->blk = 512;
        if (fstat(e->fd, &st) == 0) e->cap_blocks = (U64)st.st_size / e->blk;
        else                        e->cap_blocks = 0;
    }

    if (io_uring_queue_init(queue_depth, &e->ring, 0) != 0)
    {
        close(e->fd);
        free(e);
        return NULL;
    }

    e->qd = queue_depth;
    return e;
}

void uring_close(UringEngine* e)
{
    if (!e) return;
    io_uring_queue_exit(&e->ring);
    if (e->fd >= 0) close(e->fd);
    free(e);
}

U32 uring_block_size(const UringEngine* e)      { return e ? e->blk : 0; }
U64 uring_capacity_blocks(const UringEngine* e) { return e ? e->cap_blocks : 0; }
U32 uring_queue_depth(const UringEngine* e)     { return e ? e->qd : 0; }

void* uring_alloc_buffer(const UringEngine* e, size_t len)
{
    void* p = NULL;
    (void)e;
    if (posix_memalign(&p, URING_BUF_ALIGN, len) != 0) return NULL;
    return p;
}

void uring_free_buffer(void* p)
{
    free(p);
}

int uring_queue_write(UringEngine* e, void* buf, U64 lba, U32 nblocks, U64 user_data, int fua)
{
    struct io_uring_sqe* sqe = io_uring_get_sqe(&e->ring);
    if (!sqe) return -1;

    io_uring_prep_write(sqe, e->fd, buf, nblocks * e->blk, lba * (U64)e->blk);
    if (fua) sqe->rw_flags |= RWF_DSYNC;
    io_uring_sqe_set_data64(sqe, user_data);
    return 0;
}

int uring_queue_read(UringEngine* e, void* buf, U64 lba, U32 nblocks, U64 user_data)
{
    struct io_uring_sqe* sqe = io_uring_get_sqe(&e->ring);
    if (!sqe) return -1;

    io_uring_prep_read(sqe, e->fd, buf, nblocks * e->blk, lba * (U64)e->blk);
    io_uring_sqe_set_data64(sqe, user_data);
    return 0;
}

int uring_submit(UringEngine* e)
{
    return io_uring_submit(&e->ring);
}

int uring_reap(UringEngine* e, UringCqe* out, U32 max, U32 min_complete)
{
    struct io_uring_cqe* cqe;
    U32 reaped = 0;
    int rc;

    if (max == 0) return 0;

    if (min_complete > 0)
    {
        rc = io_uring_wait_cqe_nr(&e->ring, &cqe, min_complete);
        if (rc < 0) return rc;
    }

    while (reaped < max && io_uring_peek_cqe(&e->ring, &cqe) == 0)
    {
        out[reaped].user_data = cqe->user_data;
        out[reaped].result    = cqe->res;
        io_uring_cqe_seen(&e->ring, cqe);
        reaped++;
    }

    return (int)reaped;
}

int uring_flush(UringEngine* e)
{
    if (fdatasync(e->fd) != 0) return -errno;
    return 0;
}

int uring_discard(UringEngine* e, U64 lba, U64 nblocks)
{
    U64 range[2];
    range[0] = lba * (U64)e->blk;
    range[1] = nblocks * (U64)e->blk;
    if (ioctl(e->fd, BLKDISCARD, range) != 0) return -errno;
    return 0;
}
