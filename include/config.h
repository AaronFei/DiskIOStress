#ifndef DISKIOSTRESS_CONFIG_H
#define DISKIOSTRESS_CONFIG_H

#include "types.h"

/*===================================================
| Runtime configuration (CLI + config file)
===================================================*/
typedef struct
{
    U32  nr_thread;    /* deprecated (stress is single-thread io_uring; use qd) */
    U32  qd;           /* io_uring queue depth held by the single thread */
    U32  io_size;      /* bytes per IO unit */
    int  use_direct;   /* O_DIRECT (default on; --no-direct disables) */
    int  verify_only;  /* read-only scrub: verify magic/LBA/CRC, no writes */
    int  assume_yes;   /* -y/--yes: skip the destructive confirmation (CI). Still
                          refuses system drives. */
    int  simple_progress; /* single-line, 5s-average progress (no ANSI multi-line) */
    U32  read_retries;    /* re-reads of a sector on verify mismatch (transient vs persistent) */
    int  continue_on_error; /* keep running after a persistent error (else stop) */
    U32  rw_ratio;        /* mix_rw workload: percent of ops that are reads (0..100) */
    int  trim;         /* trim (BLKDISCARD) the range after each verify pass */
    U32  align;            /* alignment boundary in bytes (0 = off / natural grid) */
    int  align_mode;       /* 0 aligned, 1 unaligned, 2 mixed (when align > 0) */
    U32  align_offset;     /* unaligned offset in bytes (0 = default 1 sector) */
    int  align_offset_random; /* per-command random offset in [1 sector, align) */
    U32  nr_loop;
    U64  sz_trunk;     /* deprecated */
    U32  test_time;    /* seconds */
    U32  pattern;
    U32  workload;
    U32  seed;
    int  seed_set;     /* 1 if user provided, 0 if auto from time() */
    char ranges[1024]; /* stress LBA ranges spec ("" => whole device) */
    char config_path[256];
} Config_t;

typedef struct { const char* name; int val; } NameMap_t;

extern const NameMap_t gPatternMap[];
extern const NameMap_t gWorkloadMap[];

int         parse_enum(const char* s, const NameMap_t* map);
const char* enum_name(int v, const NameMap_t* map);

void config_set_defaults(Config_t* cfg);
int  config_apply_kv(Config_t* cfg, const char* key, const char* val, const char* src, int lineno);
int  config_load_file(const char* path, Config_t* cfg);
int  parse_cli_options(int argc, char* argv[], Config_t* cfg);
void config_validate(Config_t* cfg);
void config_print(const Config_t* cfg, const char* device);

#endif /* DISKIOSTRESS_CONFIG_H */
