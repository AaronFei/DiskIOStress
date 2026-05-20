#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "pattern.h"
#include "util.h"
#include "globals.h"

/* Compile-time guarantee that the on-media tag stays 32 bytes. */
typedef char sector_tag_size_check[(sizeof(SectorTag_t) == 32) ? 1 : -1];

const char* pattern_str[]=
{
    "All Zero",
    "All One",
    "Working Zero",
    "Working One",
    "Sequential Byte Increased",
    "Sequential Byte Decreased",
    "Sequential Word Increased",
    "Sequential Word Decreased",
    "Sequential DWord Increased",
    "Sequential DWord Decreased",
    "Random",
    "Address (self-describing)",
};

void generate_pattern(unsigned char* bufW, U32 size, U32 pattern_type)
{
    U32 i;

    switch (pattern_type)
    {
        case PATTERN_ALLZERO:
            memset(bufW, 0x00, size);
            break;
        case PATTERN_ALLONE:
            memset(bufW, 0xFF, size);
            break;
        case PATTERN_WORKING_ONE:
            for (i = 0; i < 32; i++)
            {
                *(((U32*)bufW) + i) = 1 << i;
            }

            for (i = 1; i < size / 32 / 4; i++)
            {
                memcpy(&bufW[i * 32 * 4], &bufW[0], 32 * 4);
            }

            break;
        case PATTERN_WORKING_ZERO:
            for (i = 0; i < 32; i++)
            {
                *(((U32*)bufW) + i) = ~(1 << i);
            }

            for (i = 1; i < size / 32 / 4; i++)
            {
                memcpy(&bufW[i * 32 * 4], &bufW[0], 32 * 4);
            }

            break;
        case PATTERN_SEQU_INC_DWORD:
            for (i = 0; i < size / 4; i++)
            {
                *(((U32*)bufW) + i) = i;
            }
            break;
        case PATTERN_SEQU_DEC_DWORD:
            for (i = 0; i < size / 4; i++)
            {
                *(((U32*)bufW) + i) = U32_MAX - i;
            }
            break;
        case PATTERN_SEQU_INC_WORD:
            for (i = 0; i < size / 2; i++)
            {
                *(((U16*)bufW) + i) = i;
            }
            break;
        case PATTERN_SEQU_DEC_WORD:
            for (i = 0; i < size / 2; i++)
            {
                *(((U16*)bufW) + i) = U32_MAX - i;
            }
            break;
        case PATTERN_SEQU_INC_BYTE:
            for (i = 0; i < size; i++)
            {
                *(bufW + i) = i;
            }
            break;
        case PATTERN_SEQU_DEC_BYTE:
            for (i = 0; i < size; i++)
            {
                *(bufW + i) = U32_MAX - i;
            }
            break;
        case PATTERN_RANDOM:
        {
            for (i = 0; i < (32 * SIZE_1K); i++)
            {
                bufW[i] = rand() % 0xFF;
            }

            for (i = 1; i < size / 32 / SIZE_1K; i++)
            {
                memcpy(&bufW[i * SIZE_1K * 32], &bufW[0], SIZE_1K * 32);
            }
            break;
        }
        default:
            printf("Error:Invalid Pattern Type\n");
            break;
    }
}

/* Size-safe byte fill of a position-agnostic pattern over [d, d+len). */
static void fill_pattern_bytes(unsigned char* d, U32 len, U32 pattern, U32 seed)
{
    U32 i;
    switch (pattern)
    {
        case PATTERN_ALLZERO: memset(d, 0x00, len); break;
        case PATTERN_ALLONE:  memset(d, 0xFF, len); break;
        case PATTERN_SEQU_INC_BYTE: for (i = 0; i < len; i++) d[i] = (unsigned char)i; break;
        case PATTERN_SEQU_DEC_BYTE: for (i = 0; i < len; i++) d[i] = (unsigned char)(U32_MAX - i); break;
        case PATTERN_SEQU_INC_WORD:
            for (i = 0; i + 2 <= len; i += 2) *(U16*)(d + i) = (U16)(i / 2);
            for (; i < len; i++) d[i] = 0;
            break;
        case PATTERN_SEQU_DEC_WORD:
            for (i = 0; i + 2 <= len; i += 2) *(U16*)(d + i) = (U16)(U32_MAX - i / 2);
            for (; i < len; i++) d[i] = 0;
            break;
        case PATTERN_SEQU_INC_DWORD:
            for (i = 0; i + 4 <= len; i += 4) *(U32*)(d + i) = i / 4;
            for (; i < len; i++) d[i] = 0;
            break;
        case PATTERN_SEQU_DEC_DWORD:
            for (i = 0; i + 4 <= len; i += 4) *(U32*)(d + i) = U32_MAX - i / 4;
            for (; i < len; i++) d[i] = 0;
            break;
        case PATTERN_WORKING_ONE:
            for (i = 0; i + 4 <= len; i += 4) *(U32*)(d + i) = 1u << ((i / 4) & 31);
            for (; i < len; i++) d[i] = 0;
            break;
        case PATTERN_WORKING_ZERO:
            for (i = 0; i + 4 <= len; i += 4) *(U32*)(d + i) = ~(1u << ((i / 4) & 31));
            for (; i < len; i++) d[i] = 0xFF;
            break;
        case PATTERN_RANDOM:
        default:
        {
            U32 x = seed ? seed : 0x9E3779B9u;
            for (i = 0; i < len; i++) { x = x * 1103515245u + 12345u; d[i] = (unsigned char)(x >> 24); }
            break;
        }
    }
}

void fill_unit_payload(unsigned char* buf, U32 unit_blocks, U32 blk,
                       U64 start_lba, U32 gen, U32 pattern)
{
    U32 tagsz = (U32)sizeof(SectorTag_t);
    U32 s;

    for (s = 0; s < unit_blocks; s++)
    {
        unsigned char* sec = buf + (U64)s * blk;
        unsigned char* pl  = sec + tagsz;
        U32 plen = (blk > tagsz) ? (blk - tagsz) : 0;
        U64 lba  = start_lba + s;

        if (pattern == PATTERN_ADDR)
        {
            U32 off;
            for (off = 0; off + 16 <= plen; off += 16)
            {
                U32 byte_off = tagsz + off;          /* offset within the sector */
                memcpy(pl + off,      &lba,      8);
                memcpy(pl + off + 8,  &byte_off, 4);
                memcpy(pl + off + 12, &gen,      4);
            }
            for (; off < plen; off++) pl[off] = (unsigned char)(lba >> ((off & 7) * 8));
        }
        else
        {
            /* fold the run seed in so different runs write different bytes for the
             * same (lba,gen) — reproducible within a run (seed is fixed). */
            fill_pattern_bytes(pl, plen, pattern, (U32)(lba ^ ((U64)gen << 20) ^ gDiskIOInfo.seed));
        }
    }
}

void stamp_sector_tags(unsigned char* buf, ThreadInfo_t* pThrInfo, U64 start_lba, U32 write_loop)
{
#if SUPPORT_DATA_TAG == TRUE
    U32 sz          = gDiskIOInfo.sz_block;
    U32 payload_off = (U32)sizeof(SectorTag_t);
    U32 idx;

    for (idx = 0; idx < pThrInfo->block_count; idx++)
    {
        unsigned char* sec = buf + (U64)sz * idx;
        SectorTag_t*   t   = (SectorTag_t*)sec;

        t->magic        = DIOS_TAG_MAGIC;
        t->seed         = gDiskIOInfo.seed;
        t->lba          = start_lba + idx;
        t->write_loop   = write_loop;
        t->trunk_index  = pThrInfo->cr_trunk;
        t->thread_id    = (U16)pThrInfo->id;
        t->pattern_type = (U8)pThrInfo->pattern_type;
        t->workload     = (U8)pThrInfo->workload;
        t->payload_crc  = crc32(sec + payload_off, sz - payload_off);
    }
#else
    (void)buf; (void)pThrInfo; (void)start_lba; (void)write_loop;
#endif
}

TagError_t verify_sector_tags(unsigned char* buf, ThreadInfo_t* pThrInfo, U64 start_lba, U32 expected_write_loop, U32* err_sector)
{
    U32 sz          = gDiskIOInfo.sz_block;
    U32 payload_off = (U32)sizeof(SectorTag_t);
    U32 idx;

    for (idx = 0; idx < pThrInfo->block_count; idx++)
    {
        unsigned char* sec = buf + (U64)sz * idx;
        SectorTag_t*   t   = (SectorTag_t*)sec;
        U32 crc;

        if (err_sector) *err_sector = idx;

        if (t->magic        != DIOS_TAG_MAGIC)            return TAG_ERR_MAGIC;
        if (t->lba          != start_lba + idx)           return TAG_ERR_LBA;
        if (t->seed         != gDiskIOInfo.seed)          return TAG_ERR_SEED;
        if (t->write_loop   != expected_write_loop)       return TAG_ERR_WRITE_LOOP;
        if (t->thread_id    != (U16)pThrInfo->id)         return TAG_ERR_THREAD;
        if (t->pattern_type != (U8)pThrInfo->pattern_type)return TAG_ERR_PATTERN;
        if (t->workload     != (U8)pThrInfo->workload)    return TAG_ERR_WORKLOAD;

        crc = crc32(sec + payload_off, sz - payload_off);
        if (crc != t->payload_crc)                        return TAG_ERR_PAYLOAD_CRC;
    }

    return TAG_OK;
}

TagError_t scrub_sector_tags(unsigned char* buf, ThreadInfo_t* pThrInfo, U64 start_lba, U32* err_sector)
{
    U32 sz          = gDiskIOInfo.sz_block;
    U32 payload_off = (U32)sizeof(SectorTag_t);
    U32 idx;

    for (idx = 0; idx < pThrInfo->block_count; idx++)
    {
        unsigned char* sec = buf + (U64)sz * idx;
        SectorTag_t*   t   = (SectorTag_t*)sec;
        U32 crc;

        if (err_sector) *err_sector = idx;

        if (t->magic != DIOS_TAG_MAGIC)      return TAG_ERR_MAGIC;
        if (t->lba   != start_lba + idx)     return TAG_ERR_LBA;
        crc = crc32(sec + payload_off, sz - payload_off);
        if (crc != t->payload_crc)           return TAG_ERR_PAYLOAD_CRC;
    }
    return TAG_OK;
}

const char* tag_error_str(TagError_t e)
{
    switch (e)
    {
        case TAG_OK:             return "OK";
        case TAG_ERR_MAGIC:      return "MAGIC (sector never written / torn)";
        case TAG_ERR_LBA:        return "LBA (misdirected IO)";
        case TAG_ERR_SEED:       return "SEED (data from a different run)";
        case TAG_ERR_WRITE_LOOP: return "WRITE_LOOP (stale data)";
        case TAG_ERR_THREAD:     return "THREAD_ID";
        case TAG_ERR_PATTERN:    return "PATTERN_TYPE";
        case TAG_ERR_WORKLOAD:   return "WORKLOAD";
        case TAG_ERR_PAYLOAD_CRC:return "PAYLOAD_CRC (bit-rot)";
        default:                 return "UNKNOWN";
    }
}

void dump_sector_error(ThreadInfo_t* pThrInfo, unsigned char* bufR, U64 lba, U32 err_sector, U32 expected_write_loop)
{
    U32 sz          = gDiskIOInfo.sz_block;
    U32 payload_off = (U32)sizeof(SectorTag_t);
    unsigned char* sec = bufR + (U64)sz * err_sector;
    SectorTag_t*   t   = (SectorTag_t*)sec;
    U64 sec_lba    = lba + err_sector;
    U32 computed   = crc32(sec + payload_off, sz - payload_off);
    char filename[256];
    FILE* fp;
    U32 i;

    sprintf(filename, "%08llX_%03X_%d_%d_%d_bad.dat", lba, pThrInfo->block_count, pThrInfo->id, pThrInfo->cr_loop, pThrInfo->cr_trunk);
    fp = fopen(filename, "wb+");
    if (fp)
    {
        fwrite(bufR, 1, (size_t)pThrInfo->block_count * sz, fp);
        fclose(fp);
    }

    pthread_mutex_lock(&mutex_msg);
    printf("%s=== SECTOR TAG MISMATCH (sector +%u, file:%s) ===%s\n", COLOR_MAGENTA, err_sector, filename, COLOR_RESET);
    printf("  %-12s %-20s %-20s\n", "field", "expected", "actual");
    printf("  %-12s %08X             %08X         %s\n", "magic",      DIOS_TAG_MAGIC,        t->magic,        (t->magic == DIOS_TAG_MAGIC) ? "" : "<==");
    printf("  %-12s %016llX     %016llX %s\n",          "lba",        (unsigned long long)sec_lba, (unsigned long long)t->lba, (t->lba == sec_lba) ? "" : "<==");
    printf("  %-12s %08X             %08X         %s\n", "seed",       gDiskIOInfo.seed,      t->seed,         (t->seed == gDiskIOInfo.seed) ? "" : "<==");
    printf("  %-12s %-20u %-20u %s\n",                  "write_loop", expected_write_loop,   t->write_loop,   (t->write_loop == expected_write_loop) ? "" : "<== STALE");
    printf("  %-12s %-20u %-20u %s\n",                  "thread_id",  pThrInfo->id,          t->thread_id,    (t->thread_id == (U16)pThrInfo->id) ? "" : "<==");
    printf("  %-12s %-20s %-20u\n",                     "trunk_index","(info)",              t->trunk_index);
    printf("  %-12s %-20u %-20u %s\n",                  "pattern",    pThrInfo->pattern_type,t->pattern_type, (t->pattern_type == (U8)pThrInfo->pattern_type) ? "" : "<==");
    printf("  %-12s %-20u %-20u %s\n",                  "workload",   pThrInfo->workload,    t->workload,     (t->workload == (U8)pThrInfo->workload) ? "" : "<==");
    printf("  %-12s %08X             %08X         %s\n", "payload_crc",t->payload_crc,        computed,        (computed == t->payload_crc) ? "" : "<== BIT-ROT");

    printf("%s  --- first 64 bytes of bad sector ---%s", COLOR_MAGENTA, COLOR_RESET);
    for (i = 0; i < 64; i++)
    {
        if ((i % 16) == 0) printf("\n  [%04X]:", i);
        printf(" %02X", sec[i] & 0xFF);
    }
    printf("\n");
    pthread_mutex_unlock(&mutex_msg);
}
