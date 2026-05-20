#ifndef DISKIOSTRESS_JOURNAL_H
#define DISKIOSTRESS_JOURNAL_H

#include "types.h"

/*===================================================
| Persistent power-cycle journal (host-side source of truth).
|
| Records what pcwrite intended and how far it got durably, so that after a
| power cut + reboot pcscan knows what *should* have survived. Stored on the
| host (NOT the device under test) and written atomically (tmp + fsync +
| rename) so a crash mid-update never corrupts it.
|
| Progress is tracked as positions within the *current pass* traversal:
|   pos < durable_units                  -> must be at generation `gen`
|   durable_units <= pos < submitted_units -> at-risk window (gen or gen-1)
|   pos >= submitted_units               -> not yet written this pass (gen-1)
| The traversal order (sequential or random permutation, round = gen) lets
| pcscan map each LBA back to its pass position.
===================================================*/

typedef struct
{
    char device[256];
    U32  seed;
    U32  blk;             /* logical sector size, bytes        */
    U32  unit_blocks;     /* sectors per io-unit               */
    char ranges[1024];    /* --ranges spec string              */
    char access[16];      /* "seq" | "random"                  */
    char durability[16];  /* "volatile" | "plp"                */
    U32  pattern;         /* payload pattern enum              */
    U32  gen;             /* current pass generation           */
    U64  total_units;     /* units per full pass               */
    U64  durable_units;   /* units confirmed durable this pass */
    U64  submitted_units; /* units submitted this pass (at-risk upper bound) */
    U32  cycle;           /* power-cycle iteration count        */
} Journal;

/* Load journal from `path`. Returns 0 on success, -1 if missing/malformed. */
int journal_load(const char* path, Journal* j);

/* Atomically persist journal to `path` (tmp + fsync + rename + dir fsync).
 * Returns 0 on success, -1 on error. */
int journal_save(const char* path, const Journal* j);

#endif /* DISKIOSTRESS_JOURNAL_H */
