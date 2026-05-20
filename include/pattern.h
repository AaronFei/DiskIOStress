#ifndef DISKIOSTRESS_PATTERN_H
#define DISKIOSTRESS_PATTERN_H

#include "types.h"

extern const char* pattern_str[];

/*===================================================
| Per-sector data tag (written into the head of every sector)
|
| Layout is naturally aligned and exactly 32 bytes:
|   +0  magic        +4  seed       +8  lba (8B)
|   +16 write_loop   +20 trunk_index
|   +24 thread_id(2) +26 pattern(1) +27 workload(1)
|   +28 payload_crc
| The CRC covers [sizeof(SectorTag_t) .. sz_block), i.e. the payload only.
===================================================*/
#define DIOS_TAG_MAGIC  0xD15C10AAu   /* "DISC IO" */

typedef struct
{
    U32 magic;         /* DIOS_TAG_MAGIC                                   */
    U32 seed;          /* run seed (gDiskIOInfo.seed)                      */
    U64 lba;           /* this sector's logical block address              */
    U32 write_loop;    /* loop in which this LBA was last written          */
    U32 trunk_index;   /* trunk index within the thread (informational)    */
    U16 thread_id;     /* owning thread id                                 */
    U8  pattern_type;  /* PATTERN_*                                        */
    U8  workload;      /* WORKLOAD_*                                       */
    U32 payload_crc;   /* CRC32 over [sizeof(SectorTag_t) .. sz_block)     */
} SectorTag_t;

typedef enum
{
    TAG_OK = 0,
    TAG_ERR_MAGIC,
    TAG_ERR_LBA,
    TAG_ERR_SEED,
    TAG_ERR_WRITE_LOOP,   /* stale data: correct LBA but older generation */
    TAG_ERR_THREAD,
    TAG_ERR_PATTERN,
    TAG_ERR_WORKLOAD,
    TAG_ERR_PAYLOAD_CRC   /* payload bit-rot relative to its own header   */
} TagError_t;

void generate_pattern(unsigned char* bufW, U32 size, U32 pattern_type);

/* Fill the payload region [sizeof(SectorTag_t) .. blk) of every sector in a unit.
 * PATTERN_ADDR writes repeating self-describing records [lba(8) | byte-offset(4) |
 * gen(4)] so any fragment locates itself (great for torn-write / shift debugging).
 * Other patterns get a size-safe byte fill. The header + CRC are written afterwards
 * by stamp_sector_tags(). */
void fill_unit_payload(unsigned char* buf, U32 unit_blocks, U32 blk,
                       U64 start_lba, U32 gen, U32 pattern);

/* Stamp a structured tag into the first pThrInfo->block_count sectors. */
void stamp_sector_tags(unsigned char* buf, ThreadInfo_t* pThrInfo, U64 start_lba, U32 write_loop);

/* Parse + validate the tags of a read-back buffer. Returns the first error and
 * (via err_sector) the sector index where it occurred. */
TagError_t verify_sector_tags(unsigned char* buf, ThreadInfo_t* pThrInfo, U64 start_lba, U32 expected_write_loop, U32* err_sector);

/* Self-contained integrity scrub: checks only magic + LBA + payload CRC (no
 * generation/seed/pattern), so it works without knowing how the data was written. */
TagError_t scrub_sector_tags(unsigned char* buf, ThreadInfo_t* pThrInfo, U64 start_lba, U32* err_sector);

const char* tag_error_str(TagError_t e);
void        dump_sector_error(ThreadInfo_t* pThrInfo, unsigned char* bufR, U64 lba, U32 err_sector, U32 expected_write_loop);

#endif /* DISKIOSTRESS_PATTERN_H */
