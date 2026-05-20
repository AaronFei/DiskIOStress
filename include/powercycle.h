#ifndef DISKIOSTRESS_POWERCYCLE_H
#define DISKIOSTRESS_POWERCYCLE_H

#include "types.h"
#include "range.h"
#include "journal.h"

/*===================================================
| Power-cycle test: single-thread io_uring generator (pcwrite) + post-reboot
| verifier (pcscan). pcwrite runs the whole loop (write -> power hook cut ->
| restore -> wait -> scan -> repeat).
===================================================*/

typedef struct
{
    char   ranges[1024];     /* --ranges spec ("" => use regions)        */
    U32    regions;          /* --regions N                              */
    U64    region_bytes;     /* --region-size                            */
    U32    io_size;          /* bytes per io-unit (--io-size)            */
    U32    qd;               /* queue depth (--qd)                       */
    int    random_access;    /* --access=random                          */
    int    plp;              /* --durability=plp                         */
    U64    flush_interval;   /* bytes between flush+checkpoint (volatile)  */
    U64    ckpt_interval;    /* bytes between durable-prefix journal persists (plp) */
    U64    cut_after;        /* bytes written per cycle before a cut      */
    double graceful_ratio;   /* fraction of cuts that are graceful [0,1]  */
    U32    cycles;           /* number of power cycles (0 => infinite)    */
    char   journal[1024];    /* journal path                             */
    char   power_hook[1024]; /* external power-control script ("" => none)*/
    int    dry_run;          /* --power-hook-dry-run                      */
    int    use_direct;       /* O_DIRECT (default on; --no-direct off)    */
    U32    seed;             /* RNG seed (0 => time)                      */
    U32    pattern;          /* payload pattern enum                      */
} PcOptions;

typedef struct
{
    U64 checked;       /* sectors verified                       */
    U64 newest;        /* at current generation                  */
    U64 prev_ok;       /* at gen-1 within at-risk / not-yet pass  */
    U64 acceptable;    /* in-flight loss within at-risk window    */
    U64 corrupt;       /* magic/crc/lba failures                 */
    U64 stale;         /* older than gen-1 / unexpected new       */
} PcReport;

int pcwrite_cmd(char* device, int argc, char* argv[]);
int pcscan_cmd (char* device, int argc, char* argv[]);

/* Classify one read-back unit against its pass position (exposed for tests).
 *   pos < D            -> must be generation G (durable)
 *   D <= pos < S       -> at-risk window (G or G-1 acceptable)
 *   pos >= S           -> not yet written this pass (expect G-1, or don't-care if G==1)
 * Tallies into rep; requires gDiskIOInfo.sz_block / .seed set and ti populated. */
void pc_classify(unsigned char* buf, ThreadInfo_t* ti, U64 lba, U64 pos,
                 U32 G, U64 D, U64 S, U32 unit_blocks, PcReport* rep);

#endif /* DISKIOSTRESS_POWERCYCLE_H */
