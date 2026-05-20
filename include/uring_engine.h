#ifndef DISKIOSTRESS_URING_ENGINE_H
#define DISKIOSTRESS_URING_ENGINE_H

#include <stddef.h>
#include "types.h"

/*===================================================
| Single-thread, high-queue-depth io_uring block-layer engine.
|
| Works on any block device (NVMe / SATA / USB) and on regular files
| (used by the unit tests). Submission and completion are decoupled so a
| single thread can keep many commands in flight (high QD) without threads.
===================================================*/

typedef struct UringEngine UringEngine;

typedef struct
{
    U64 user_data;   /* tag the caller passed at submit time */
    S32 result;      /* >= 0 bytes transferred, < 0 == -errno */
} UringCqe;

/* uring_open flags */
#define URING_WRITE   (1u << 0)   /* open O_RDWR (default is O_RDONLY)      */
#define URING_DIRECT  (1u << 1)   /* add O_DIRECT (bypass page cache)       */

/* Open `path` and set up a ring of `queue_depth` entries.
 * Returns NULL on failure (ring init, open, or unsupported). */
UringEngine* uring_open(const char* path, U32 queue_depth, U32 flags);
void         uring_close(UringEngine* e);

U32 uring_block_size(const UringEngine* e);      /* logical sector size, bytes */
U64 uring_capacity_blocks(const UringEngine* e); /* device/file size in sectors */
U32 uring_queue_depth(const UringEngine* e);

/* O_DIRECT-aligned scratch buffer (aligned to 4096). Free with uring_free_buffer. */
void* uring_alloc_buffer(const UringEngine* e, size_t len);
void  uring_free_buffer(void* p);

/* Queue one op. `lba`/`nblocks` are in sectors. `user_data` is echoed back on
 * completion. Returns 0 if an SQE was obtained, -1 if the submission queue is
 * full (caller must uring_submit + uring_reap first). For writes, fua != 0 adds
 * RWF_DSYNC so the write is forced to media before completing. */
int uring_queue_write(UringEngine* e, void* buf, U64 lba, U32 nblocks, U64 user_data, int fua);
int uring_queue_read (UringEngine* e, void* buf, U64 lba, U32 nblocks, U64 user_data);

/* Push queued SQEs to the kernel. Returns number submitted, or < 0 (-errno). */
int uring_submit(UringEngine* e);

/* Reap completions into out[0..max). Blocks until at least min_complete are
 * available (min_complete may be 0 for non-blocking drain). Returns the number
 * reaped, or < 0 (-errno). */
int uring_reap(UringEngine* e, UringCqe* out, U32 max, U32 min_complete);

/* Device cache flush (fdatasync on the block-device fd issues FLUSH / SYNCHRONIZE
 * CACHE). Blocks until complete. Returns 0, or < 0 (-errno). */
int uring_flush(UringEngine* e);

/* Discard / TRIM a range via the generic BLKDISCARD ioctl (works on NVMe / SATA /
 * USB). lba/nblocks are in sectors. Returns 0, or < 0 (-errno). */
int uring_discard(UringEngine* e, U64 lba, U64 nblocks);

#endif /* DISKIOSTRESS_URING_ENGINE_H */
