#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <getopt.h>
#include <libgen.h>
#include <pthread.h>
#include <stdatomic.h>
#include "powercycle.h"
#include "uring_engine.h"
#include "range.h"
#include "journal.h"
#include "pattern.h"
#include "config.h"
#include "util.h"
#include "globals.h"

/* graceful-stop request from SIGINT/SIGTERM during a write phase */
static volatile sig_atomic_t g_stop = 0;
static void pc_sigstop(int s) { (void)s; g_stop = 1; }

static int pc_call_hook(const PcOptions* o, const char* verb, const char* device, const Journal* j);

/*===================================================
| Concurrent power cut (ungraceful only).
|
| The IO pump (main thread) streams writes continuously without draining and
| publishes how many bytes it has submitted. This cutter thread waits until that
| reaches `cut_at_bytes`, then fires the power-cut hook *immediately* while the
| pump keeps the queue full — so power physically drops with writes genuinely in
| flight (a true mid-IO cut), not at a quiescent boundary.
|
| Reproducibility: the trigger (cut_at_bytes) is deterministic and the at-risk
| set [durable_units, submitted_units) is journaled before issue, so the verdict
| is replayable. Which sector the controller is mid-program on at the instant the
| relay opens is physical and not host-controllable.
===================================================*/
typedef struct
{
    _Atomic unsigned long long submitted_bytes; /* pump -> cutter: progress     */
    _Atomic int  cut_fired;     /* cutter -> pump: hook invoked, power dropping  */
    _Atomic int  cut_done;      /* cutter -> pump: hook returned (dry-run/sim)   */
    _Atomic int  pump_stopped;  /* pump -> cutter: pump exited, stop waiting     */
    unsigned long long cut_at_bytes;
    const PcOptions* o;
    const char*      device;
    const Journal*   j;
    int rc;                     /* hook return code                              */
} Cutter;

static void* pc_cutter_thread(void* arg)
{
    Cutter* c = (Cutter*)arg;
    while (!atomic_load(&c->pump_stopped) && !g_stop &&
           atomic_load(&c->submitted_bytes) < c->cut_at_bytes)
        usleep(200);                              /* fine-grained: catch it mid-stream */
    if (atomic_load(&c->pump_stopped) || g_stop)
        return NULL;                              /* interrupted before the trigger */
    atomic_store(&c->cut_fired, 1);               /* pump: keep IO outstanding NOW */
    c->rc = pc_call_hook(c->o, "cut-ungraceful", c->device, c->j);
    atomic_store(&c->cut_done, 1);                /* dry-run/sim: device survives  */
    return NULL;
}

/* ---------------- options ---------------- */

static void pc_defaults(PcOptions* o)
{
    memset(o, 0, sizeof(*o));
    o->io_size        = 64 * 1024;
    o->qd             = 32;
    o->flush_interval = 16 * 1024 * 1024ULL;
    o->ckpt_interval  = 1024 * 1024ULL;   /* plp durable-prefix persist cadence */
    o->cut_after      = 64 * 1024 * 1024ULL;
    o->graceful_ratio = 0.5;
    o->cycles         = 0;
    o->use_direct     = 1;
    o->pattern        = PATTERN_RANDOM;
}

enum {
    OPT_RANGES = 1000, OPT_REGIONS, OPT_REGION_SIZE, OPT_IO_SIZE, OPT_QD,
    OPT_ACCESS, OPT_DURABILITY, OPT_FLUSH_INTERVAL, OPT_CKPT_INTERVAL, OPT_CUT_AFTER,
    OPT_GRACEFUL_RATIO, OPT_CYCLES, OPT_JOURNAL, OPT_POWER_HOOK,
    OPT_DRY_RUN, OPT_NO_DIRECT, OPT_SEED, OPT_PATTERN, OPT_CONFIG
};

/* Load an INI-style power-cycle config into PcOptions (CLI overrides it later). */
static int pc_config_load(const char* path, PcOptions* o)
{
    FILE* fp = fopen(path, "r");
    char line[1280];
    int lineno = 0, errors = 0;

    if (!fp) { fprintf(stderr, "pc: cannot open config %s\n", path); return -1; }

    while (fgets(line, sizeof(line), fp))
    {
        char *p, *eq, *key, *val, *hash;
        lineno++;
        hash = strchr(line, '#'); if (hash) *hash = '\0';
        hash = strchr(line, ';'); if (hash) *hash = '\0';
        p = str_trim(line); if (!*p) continue;
        eq = strchr(p, '='); if (!eq) { fprintf(stderr, "%s:%d: missing '='\n", path, lineno); errors++; continue; }
        *eq = '\0'; key = str_trim(p); val = str_trim(eq + 1);

        if      (!strcmp(key, "ranges"))         strncpy(o->ranges, val, sizeof(o->ranges) - 1);
        else if (!strcmp(key, "regions"))        o->regions = (U32)strtoul(val, NULL, 0);
        else if (!strcmp(key, "region_size"))    { U64 v; if (parse_size(val, &v)) errors++; else o->region_bytes = v; }
        else if (!strcmp(key, "io_size"))        { U64 v; if (parse_size(val, &v)) errors++; else o->io_size = (U32)v; }
        else if (!strcmp(key, "qd"))             o->qd = (U32)strtoul(val, NULL, 0);
        else if (!strcmp(key, "access"))         o->random_access = (strcmp(val, "random") == 0);
        else if (!strcmp(key, "durability"))     o->plp = (strcmp(val, "plp") == 0);
        else if (!strcmp(key, "flush_interval")) { U64 v; if (parse_size(val, &v)) errors++; else o->flush_interval = v; }
        else if (!strcmp(key, "checkpoint_interval")) { U64 v; if (parse_size(val, &v)) errors++; else o->ckpt_interval = v; }
        else if (!strcmp(key, "cut_after"))      { U64 v; if (parse_size(val, &v)) errors++; else o->cut_after = v; }
        else if (!strcmp(key, "graceful_ratio")) o->graceful_ratio = atof(val);
        else if (!strcmp(key, "cycles"))         o->cycles = (U32)strtoul(val, NULL, 0);
        else if (!strcmp(key, "journal"))        strncpy(o->journal, val, sizeof(o->journal) - 1);
        else if (!strcmp(key, "power_hook"))     strncpy(o->power_hook, val, sizeof(o->power_hook) - 1);
        else if (!strcmp(key, "direct"))         o->use_direct = strtoul(val, NULL, 0) ? 1 : 0;
        else if (!strcmp(key, "seed"))           { U32 s = (U32)strtoul(val, NULL, 0); if (s) o->seed = s; }
        else if (!strcmp(key, "pattern"))        { int v = parse_enum(val, gPatternMap); if (v < 0) { fprintf(stderr, "%s:%d: unknown pattern '%s'\n", path, lineno, val); errors++; } else o->pattern = (U32)v; }
        else { fprintf(stderr, "%s:%d: unknown key '%s'\n", path, lineno, key); errors++; }
    }

    fclose(fp);
    return errors ? -1 : 0;
}

static int pc_parse(int argc, char* argv[], PcOptions* o)
{
    static struct option lo[] = {
        {"config",         required_argument, 0, OPT_CONFIG},
        {"ranges",         required_argument, 0, OPT_RANGES},
        {"regions",        required_argument, 0, OPT_REGIONS},
        {"region-size",    required_argument, 0, OPT_REGION_SIZE},
        {"io-size",        required_argument, 0, OPT_IO_SIZE},
        {"qd",             required_argument, 0, OPT_QD},
        {"access",         required_argument, 0, OPT_ACCESS},
        {"durability",     required_argument, 0, OPT_DURABILITY},
        {"flush-interval", required_argument, 0, OPT_FLUSH_INTERVAL},
        {"checkpoint-interval", required_argument, 0, OPT_CKPT_INTERVAL},
        {"cut-after",      required_argument, 0, OPT_CUT_AFTER},
        {"graceful-ratio", required_argument, 0, OPT_GRACEFUL_RATIO},
        {"cycles",         required_argument, 0, OPT_CYCLES},
        {"journal",        required_argument, 0, OPT_JOURNAL},
        {"power-hook",     required_argument, 0, OPT_POWER_HOOK},
        {"power-hook-dry-run", no_argument,   0, OPT_DRY_RUN},
        {"no-direct",      no_argument,       0, OPT_NO_DIRECT},
        {"seed",           required_argument, 0, OPT_SEED},
        {"pattern",        required_argument, 0, OPT_PATTERN},
        {0, 0, 0, 0}
    };
    char** gv = (char**)calloc(argc + 2, sizeof(char*));
    int c, rc = 0;
    U64 tmp;
    int i;

    if (!gv) return -1;
    gv[0] = "pc";
    for (i = 0; i < argc; i++) gv[i + 1] = argv[i];

    /* First pass: load --config so subsequent CLI options override it. */
    for (i = 0; i < argc; i++)
    {
        const char* p = NULL;
        if      (strncmp(argv[i], "--config=", 9) == 0)              p = argv[i] + 9;
        else if (strcmp(argv[i], "--config") == 0 && i + 1 < argc)   p = argv[++i];
        if (p && pc_config_load(p, o) != 0) { free(gv); return -1; }
    }

    optind = 1;
    opterr = 1;

    while ((c = getopt_long(argc + 1, gv, "", lo, NULL)) != -1)
    {
        switch (c)
        {
            case OPT_CONFIG:      break;   /* already processed in the first pass */
            case OPT_RANGES:      strncpy(o->ranges, optarg, sizeof(o->ranges) - 1); break;
            case OPT_REGIONS:     o->regions = (U32)strtoul(optarg, NULL, 0); break;
            case OPT_REGION_SIZE: if (parse_size(optarg, &o->region_bytes)) rc = -1; break;
            case OPT_IO_SIZE:     if (parse_size(optarg, &tmp)) rc = -1; else o->io_size = (U32)tmp; break;
            case OPT_QD:          o->qd = (U32)strtoul(optarg, NULL, 0); break;
            case OPT_ACCESS:      o->random_access = (strcmp(optarg, "random") == 0); break;
            case OPT_DURABILITY:  o->plp = (strcmp(optarg, "plp") == 0); break;
            case OPT_FLUSH_INTERVAL: if (parse_size(optarg, &o->flush_interval)) rc = -1; break;
            case OPT_CKPT_INTERVAL:  if (parse_size(optarg, &o->ckpt_interval)) rc = -1; break;
            case OPT_CUT_AFTER:   if (parse_size(optarg, &o->cut_after)) rc = -1; break;
            case OPT_GRACEFUL_RATIO: o->graceful_ratio = atof(optarg); break;
            case OPT_CYCLES:      o->cycles = (U32)strtoul(optarg, NULL, 0); break;
            case OPT_JOURNAL:     strncpy(o->journal, optarg, sizeof(o->journal) - 1); break;
            case OPT_POWER_HOOK:  strncpy(o->power_hook, optarg, sizeof(o->power_hook) - 1); break;
            case OPT_DRY_RUN:     o->dry_run = 1; break;
            case OPT_NO_DIRECT:   o->use_direct = 0; break;
            case OPT_SEED:        o->seed = (U32)strtoul(optarg, NULL, 0); break;
            case OPT_PATTERN:
            {
                int v = parse_enum(optarg, gPatternMap);
                if (v < 0) { fprintf(stderr, "unknown pattern: %s\n", optarg); rc = -1; }
                else o->pattern = (U32)v;
                break;
            }
            default: rc = -1; break;
        }
    }

    free(gv);
    if (o->io_size == 0 || o->qd == 0) rc = -1;
    return rc;
}

/* ---------------- helpers ---------------- */

static void pc_default_journal(PcOptions* o, const char* device)
{
    char tmp[256];
    char* base;
    if (o->journal[0]) return;
    strncpy(tmp, device, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    base = basename(tmp);
    snprintf(o->journal, sizeof(o->journal), "/tmp/diskiostress-%s.journal", base);
}

static U64 pos_to_lba(const RangeSet* rs, const PcOptions* o, U64 pos, U32 gen)
{
    U64 ui = o->random_access ? range_permute(rs->total_units, pos, gen ^ o->seed) : pos;
    return range_unit_lba(rs, ui);
}

/* Write ~units_to_write units, maintaining queue depth and tracking the
 * contiguous completion prefix.
 *
 *   volatile : durable advances when a full flush-interval chunk has completed
 *              and been flushed (durable == flushed).
 *   plp      : durable advances to the completed-prefix and is persisted every
 *              checkpoint-interval units (durable == completed). This is the
 *              strict, completion-level PLP guarantee: a completed write that is
 *              lost after a cut falls inside [0,durable) and is reported FAIL.
 *
 * `submitted_units` is always claimed in the journal *before* the corresponding
 * writes are issued, so it is a safe upper bound (never a false positive). */
/* When `cutter` is non-NULL the pump streams continuously (units_to_write should
 * be unbounded): it publishes progress, never marks the at-risk window durable,
 * and treats post-cut IO errors as the expected end of the phase. With cutter ==
 * NULL it behaves as before: write exactly units_to_write, drain, return. */
static int pc_write_phase(UringEngine* e, const RangeSet* rs, Journal* j,
                          const PcOptions* o, ThreadInfo_t* ti, U32 unit_blocks,
                          U64 units_to_write, Cutter* cutter)
{
    U32 unit_bytes  = unit_blocks * uring_block_size(e);
    U64 claim_units = o->flush_interval / unit_bytes;
    U64 ckpt_units  = o->ckpt_interval  / unit_bytes;
    U32 qd          = o->qd;
    U32 WINDOW      = qd * 2; if (WINDOW < 64) WINDOW = 64;

    unsigned char** bufs    = (unsigned char**)calloc(qd, sizeof(*bufs));
    U64*            slotpos = (U64*)calloc(qd, sizeof(U64));
    U32*            freestk = (U32*)calloc(qd, sizeof(U32));
    unsigned char*  done    = (unsigned char*)calloc(WINDOW, 1);
    UringCqe*       cqes     = (UringCqe*)calloc(qd, sizeof(UringCqe));
    U32 ntop = 0, inflight = 0, i;
    U64 written_total = 0;
    U64 issued_phase = 0;          /* units submitted this phase (for cutter trigger) */
    int rc = 0;

    if (claim_units == 0) claim_units = 1;
    if (ckpt_units  == 0) ckpt_units  = 1;
    if (!bufs || !slotpos || !freestk || !done || !cqes) { rc = -1; goto out; }
    for (i = 0; i < qd; i++)
    {
        bufs[i] = uring_alloc_buffer(e, unit_bytes);
        if (!bufs[i]) { rc = -1; goto out; }
        freestk[ntop++] = i;
    }

    while (written_total < units_to_write && !g_stop)
    {
        U64 base, n, prefix = 0, next = 0, claimed = 0;
        U64 last_persist = 0, last_flush = 0;

        if (j->submitted_units >= j->total_units)   /* pass complete -> next gen */
        {
            j->gen += 1;
            j->submitted_units = 0;
            j->durable_units = 0;
            if (journal_save(o->journal, j)) { rc = -1; goto out; }
        }

        base = j->submitted_units;
        n = j->total_units - base;
        if (n > units_to_write - written_total) n = units_to_write - written_total;
        if (n == 0) break;

        while (prefix < n && !g_stop && !(cutter && atomic_load(&cutter->cut_done)))
        {
            int can_submit;
            int r;

            if (next >= claimed && claimed < n)             /* claim ahead, persist S */
            {
                claimed += claim_units;
                if (claimed > n) claimed = n;
                j->submitted_units = base + claimed;
                if (journal_save(o->journal, j)) { rc = -1; goto out; }
            }

            while (inflight < qd && next < claimed && (next - prefix) < WINDOW)
            {
                U32 bi = freestk[--ntop];
                U64 pos = base + next;
                U64 lba = pos_to_lba(rs, o, pos, j->gen);
                fill_unit_payload(bufs[bi], unit_blocks, uring_block_size(e), lba, j->gen, o->pattern);
                ti->block_count = unit_blocks;
                stamp_sector_tags(bufs[bi], ti, lba, j->gen);
                if (uring_queue_write(e, bufs[bi], lba, unit_blocks, bi, 0) < 0)
                { freestk[ntop++] = bi; break; }
                slotpos[bi] = next;
                inflight++; next++; issued_phase++;
            }

            if (uring_submit(e) < 0)
            { if (cutter && atomic_load(&cutter->cut_fired)) goto cut_taken; rc = -1; goto out; }
            if (cutter) atomic_store(&cutter->submitted_bytes, issued_phase * unit_bytes);

            can_submit = (inflight < qd && next < claimed && (next - prefix) < WINDOW);
            r = uring_reap(e, cqes, qd, (inflight && !can_submit) ? 1 : 0);
            if (r < 0)
            { if (cutter && atomic_load(&cutter->cut_fired)) goto cut_taken; rc = -1; goto out; }
            for (i = 0; i < (U32)r; i++)
            {
                U32 bi = (U32)cqes[i].user_data;
                if (cqes[i].result != (S32)unit_bytes)
                { if (cutter && atomic_load(&cutter->cut_fired)) goto cut_taken; rc = -1; goto out; }
                done[slotpos[bi] % WINDOW] = 1;
                freestk[ntop++] = bi;
                inflight--;
            }
            while (done[prefix % WINDOW]) { done[prefix % WINDOW] = 0; prefix++; }

            if (o->plp)
            {
                if (prefix - last_persist >= ckpt_units)    /* durable = completed prefix */
                {
                    j->durable_units = base + prefix;
                    if (journal_save(o->journal, j)) { rc = -1; goto out; }
                    last_persist = prefix;
                }
            }
            else
            {
                while (last_flush + claim_units <= prefix)  /* durable = flushed chunk */
                {
                    if (uring_flush(e) < 0) { rc = -1; goto out; }
                    last_flush += claim_units;
                    j->durable_units = base + last_flush;
                    if (journal_save(o->journal, j)) { rc = -1; goto out; }
                }
            }
        }

        /* Cut took effect in dry-run/sim (device survived the hook): stop here
         * WITHOUT marking the at-risk window durable. */
        if (cutter && atomic_load(&cutter->cut_done)) goto cut_taken;

        if (!o->plp && uring_flush(e) < 0) { rc = -1; goto out; }
        j->submitted_units = base + n;
        j->durable_units   = base + n;
        if (journal_save(o->journal, j)) { rc = -1; goto out; }
        written_total += n;
    }

    goto out;                 /* normal completion / drained */
cut_taken:
    rc = 0;                   /* power cut interrupted in-flight IO (expected) */
    /* On a REAL cut the device is gone; the buffers are irrelevant. But in dry-run
     * (and any sim where the device survives the hook) the kernel may still be
     * consuming the in-flight write buffers — drain them before we free, or we'd
     * corrupt those units under it. Reaps fail fast if the device really died. */
    if (o->dry_run)
        while (inflight > 0)
        {
            int r2 = uring_reap(e, cqes, qd, 1);
            if (r2 <= 0) break;
            inflight -= (U32)r2;
        }
out:
    if (bufs) { for (i = 0; i < qd; i++) if (bufs[i]) uring_free_buffer(bufs[i]); free(bufs); }
    free(slotpos); free(freestk); free(done); free(cqes);
    return rc;
}

/* Classify one unit read into buf against the journal's pass position. */
void pc_classify(unsigned char* buf, ThreadInfo_t* ti, U64 lba, U64 pos,
                 U32 G, U64 D, U64 S, U32 unit_blocks, PcReport* rep)
{
    U32 errsec = 0;
    TagError_t e1;

    rep->checked += unit_blocks;
    ti->block_count = unit_blocks;

    if (pos < D)                       /* durable: MUST be generation G */
    {
        e1 = verify_sector_tags(buf, ti, lba, G, &errsec);
        if      (e1 == TAG_OK)              rep->newest += unit_blocks;
        else if (e1 == TAG_ERR_WRITE_LOOP)  rep->stale  += unit_blocks;  /* durable data lost */
        else                                rep->corrupt += unit_blocks;
    }
    else if (pos < S)                  /* at-risk window: G or G-1 acceptable */
    {
        e1 = verify_sector_tags(buf, ti, lba, G, &errsec);
        if (e1 == TAG_OK) { rep->newest += unit_blocks; return; }

        if (G >= 2)
        {
            TagError_t e2 = verify_sector_tags(buf, ti, lba, G - 1, &errsec);
            if (e2 == TAG_OK)                                     rep->acceptable += unit_blocks;
            else if (e1 == TAG_ERR_WRITE_LOOP && e2 == TAG_ERR_WRITE_LOOP) rep->stale += unit_blocks;
            else                                                  rep->corrupt += unit_blocks;
        }
        else /* G == 1: previous content is blank/unwritten */
        {
            if (e1 == TAG_ERR_MAGIC) rep->acceptable += unit_blocks;  /* never written yet */
            else                     rep->corrupt += unit_blocks;
        }
    }
    else                               /* not yet written this pass */
    {
        if (G >= 2)
        {
            e1 = verify_sector_tags(buf, ti, lba, G - 1, &errsec);
            if      (e1 == TAG_OK)             rep->prev_ok += unit_blocks;
            else if (e1 == TAG_ERR_WRITE_LOOP) rep->stale   += unit_blocks;
            else                               rep->corrupt += unit_blocks;
        }
        /* G == 1: never written this region, content is don't-care */
    }
}

static int pc_scan_phase(UringEngine* e, const RangeSet* rs, const Journal* j,
                         const PcOptions* o, ThreadInfo_t* ti, U32 unit_blocks,
                         PcReport* rep)
{
    U32 unit_bytes = unit_blocks * uring_block_size(e);
    unsigned char* buf = uring_alloc_buffer(e, unit_bytes);
    UringCqe cqe;
    U64 pos;
    int rc = 0;

    if (!buf) return -1;
    memset(rep, 0, sizeof(*rep));

    for (pos = 0; pos < rs->total_units; pos++)
    {
        U64 lba = pos_to_lba(rs, o, pos, j->gen);

        if (uring_queue_read(e, buf, lba, unit_blocks, 0) < 0) { rc = -1; break; }
        if (uring_submit(e) < 0) { rc = -1; break; }
        if (uring_reap(e, &cqe, 1, 1) < 0) { rc = -1; break; }
        if (cqe.result != (S32)unit_bytes) { rc = -1; break; }

        pc_classify(buf, ti, lba, pos, j->gen, j->durable_units, j->submitted_units,
                    unit_blocks, rep);
    }

    uring_free_buffer(buf);
    return rc;
}

static int pc_call_hook(const PcOptions* o, const char* verb, const char* device, const Journal* j)
{
    char cmd[2300];
    char buf[32];

    /* dry-run: never touch power — just exercise the loop (works with any hook). */
    if (o->dry_run)
    {
        printf("[dry-run] would %s %s\n", verb, device);
        return 0;
    }

    if (!o->power_hook[0])
    {
        printf("[no power-hook] would %s %s\n", verb, device);
        return 0;
    }

    snprintf(buf, sizeof(buf), "%u", j->gen);   setenv("DIOS_GENERATION", buf, 1);
    snprintf(buf, sizeof(buf), "%u", j->cycle); setenv("DIOS_CYCLE", buf, 1);
    setenv("DIOS_PHASE", verb, 1);
    setenv("DIOS_TIMEOUT", "60", 1);
    setenv("DIOS_DRY_RUN", o->dry_run ? "1" : "0", 1);

    snprintf(cmd, sizeof(cmd), "'%s' '%s' '%s'", o->power_hook, verb, device);
    return (system(cmd) == 0) ? 0 : -1;
}

static int pc_wait_device(const char* path, int timeout_s)
{
    int i;
    for (i = 0; i < timeout_s * 10; i++)
    {
        int fd = open(path, O_RDONLY);
        if (fd >= 0) { close(fd); return 0; }
        usleep(100000);
    }
    return -1;
}

static void pc_report_print(const PcReport* r, U32 blk, int pass)
{
    printf("=== pcscan report ===\n");
    printf("  checked    : %llu sectors (%.1f MB)\n", (unsigned long long)r->checked, (double)r->checked * blk / (1024*1024));
    printf("  newest(G)  : %llu\n", (unsigned long long)r->newest);
    printf("  prev(G-1)  : %llu\n", (unsigned long long)r->prev_ok);
    printf("  acceptable : %llu  (in-flight loss in at-risk window)\n", (unsigned long long)r->acceptable);
    printf("  %sstale      : %llu%s\n", r->stale ? COLOR_RED : "", (unsigned long long)r->stale, COLOR_RESET);
    printf("  %scorrupt    : %llu%s\n", r->corrupt ? COLOR_RED : "", (unsigned long long)r->corrupt, COLOR_RESET);
    printf("  => %s%s%s\n", pass ? COLOR_GREEN : COLOR_RED, pass ? "PASS" : "FAIL", COLOR_RESET);
}

/* Build the RangeSet from options. */
static int pc_build_rangeset(UringEngine* e, const PcOptions* o, U32 unit_blocks, RangeSet* rs)
{
    U32 blk = uring_block_size(e);
    U64 cap = uring_capacity_blocks(e);

    if (o->regions > 0)
        return range_make_regions(rs, o->regions, o->region_bytes, blk, cap, unit_blocks);
    if (o->ranges[0])
        return range_parse(rs, o->ranges, blk, cap, unit_blocks);
    return range_parse(rs, "whole", blk, cap, unit_blocks);
}

static void pc_init_threadinfo(ThreadInfo_t* ti, const PcOptions* o)
{
    memset(ti, 0, sizeof(*ti));
    ti->id           = 0;
    ti->cr_trunk     = 0;
    ti->pattern_type = o->pattern;
    ti->workload     = WORKLOAD_RAND_WRC;
}

/* ---------------- entry points ---------------- */

int pcwrite_cmd(char* device, int argc, char* argv[])
{
    PcOptions o;
    UringEngine* e;
    RangeSet rs;
    Journal j;
    ThreadInfo_t ti;
    U32 blk, unit_blocks;
    U64 units_per_cut;

    pc_defaults(&o);
    if (pc_parse(argc, argv, &o)) { fprintf(stderr, "pcwrite: bad options\n"); return 1; }
    pc_default_journal(&o, device);

    /* one seed: a pcwrite --seed wins; else inherit the global --seed; else time. */
    if (!o.seed) o.seed = gConfig.seed_set ? gConfig.seed : (U32)time(NULL);
    crc32_init();

    e = uring_open(device, o.qd, URING_WRITE | (o.use_direct ? URING_DIRECT : 0));
    if (!e) { fprintf(stderr, "pcwrite: cannot open %s\n", device); return 1; }

    blk = uring_block_size(e);
    gDiskIOInfo.sz_block = blk;
    gDiskIOInfo.seed     = o.seed;

    if (o.io_size % blk != 0) { fprintf(stderr, "io-size must be a multiple of %u\n", blk); uring_close(e); return 1; }
    unit_blocks = o.io_size / blk;

    if (pc_build_rangeset(e, &o, unit_blocks, &rs)) { fprintf(stderr, "pcwrite: bad ranges\n"); uring_close(e); return 1; }
    pc_init_threadinfo(&ti, &o);

    /* resume existing journal for this device, else start fresh */
    if (journal_load(o.journal, &j) != 0 || strcmp(j.device, device) != 0)
    {
        memset(&j, 0, sizeof(j));
        strncpy(j.device, device, sizeof(j.device) - 1);
        strncpy(j.access, o.random_access ? "random" : "seq", sizeof(j.access) - 1);
        strncpy(j.durability, o.plp ? "plp" : "volatile", sizeof(j.durability) - 1);
        strncpy(j.ranges, o.ranges[0] ? o.ranges : "whole", sizeof(j.ranges) - 1);
        j.seed = o.seed; j.blk = blk; j.unit_blocks = unit_blocks; j.pattern = o.pattern;
        j.gen = 1; j.total_units = rs.total_units;
        j.durable_units = 0; j.submitted_units = 0; j.cycle = 0;
        journal_save(o.journal, &j);
    }
    /* journal is authoritative (esp. on resume): all randomness uses j.seed */
    o.seed = j.seed;
    gDiskIOInfo.seed = j.seed;
    srand(j.seed);

    signal(SIGINT,  pc_sigstop);
    signal(SIGTERM, pc_sigstop);

    units_per_cut = o.cut_after / o.io_size;
    if (units_per_cut == 0) units_per_cut = 1;

    printf("=== pcwrite ===\n");
    printf("  device=%s  blk=%u  cap=%llu sectors\n", device, blk, (unsigned long long)uring_capacity_blocks(e));
    printf("  ranges=%s  access=%s  durability=%s\n", j.ranges, j.access, j.durability);
    printf("  io_size=%u  qd=%u  total_units=%llu  seed=0x%08X\n", o.io_size, o.qd, (unsigned long long)rs.total_units, j.seed);
    if (o.plp) printf("  PLP: durable=completion-prefix, checkpoint every %lluKB\n", (unsigned long long)(o.ckpt_interval / 1024));
    else       printf("  volatile: durable=flushed, flush every %lluKB\n", (unsigned long long)(o.flush_interval / 1024));
    printf("  journal=%s  power_hook=%s%s\n", o.journal, o.power_hook[0] ? o.power_hook : "(none)", o.dry_run ? "  [DRY-RUN]" : "");

    for (;;)
    {
        int graceful;
        PcReport rep;
        int pass;

        /* rewind the at-risk window so it gets rewritten cleanly this cycle */
        j.submitted_units = j.durable_units;
        journal_save(o.journal, &j);

        graceful = ((double)rand() / RAND_MAX) < o.graceful_ratio;

        if (graceful)
        {
            /* write a chunk, DRAIN it, flush, then cut at a quiescent point so
             * nothing should be lost. */
            if (pc_write_phase(e, &rs, &j, &o, &ti, unit_blocks, units_per_cut, NULL))
            { fprintf(stderr, "pcwrite: write phase error\n"); uring_close(e); return 1; }

            if (!o.plp) uring_flush(e);
            j.durable_units = j.submitted_units;
            journal_save(o.journal, &j);

            if (pc_call_hook(&o, "cut-graceful", device, &j))
            { fprintf(stderr, "pcwrite: power hook (cut) failed\n"); uring_close(e); return 1; }
        }
        else
        {
            /* stream writes continuously; a cutter thread drops power mid-IO once
             * cut_after bytes have been submitted (true in-flight cut). */
            Cutter c;
            pthread_t th;
            U32 unit_bytes = unit_blocks * uring_block_size(e);

            memset(&c, 0, sizeof(c));
            c.cut_at_bytes = units_per_cut * (unsigned long long)unit_bytes;
            c.o = &o; c.device = device; c.j = &j;

            if (pthread_create(&th, NULL, pc_cutter_thread, &c))
            { fprintf(stderr, "pcwrite: cannot start cutter thread\n"); uring_close(e); return 1; }

            if (pc_write_phase(e, &rs, &j, &o, &ti, unit_blocks, (U64)-1, &c))
            {
                atomic_store(&c.pump_stopped, 1); pthread_join(th, NULL);
                fprintf(stderr, "pcwrite: write phase error\n"); uring_close(e); return 1;
            }
            atomic_store(&c.pump_stopped, 1);
            pthread_join(th, NULL);

            if (!atomic_load(&c.cut_fired))   /* interrupted (Ctrl-C) before any cut */
            { printf("stop requested before cut\n"); break; }
            if (c.rc)
            { fprintf(stderr, "pcwrite: power hook (cut) failed\n"); uring_close(e); return 1; }
        }

        uring_close(e);

        if (pc_call_hook(&o, "restore", device, &j))
        { fprintf(stderr, "pcwrite: power hook (restore) failed\n"); return 1; }

        if (pc_wait_device(device, 60))
        { fprintf(stderr, "pcwrite: device %s did not return\n", device); return 1; }

        e = uring_open(device, o.qd, (o.use_direct ? URING_DIRECT : 0));  /* read-only for scan */
        if (!e) { fprintf(stderr, "pcwrite: reopen failed\n"); return 1; }

        j.cycle += 1;
        journal_save(o.journal, &j);

        if (pc_scan_phase(e, &rs, &j, &o, &ti, unit_blocks, &rep))
        { fprintf(stderr, "pcwrite: scan error\n"); uring_close(e); return 1; }

        pass = (rep.corrupt == 0 && rep.stale == 0);
        printf("--- cycle %u (%s) ---\n", j.cycle, graceful ? "graceful" : "ungraceful");
        pc_report_print(&rep, blk, pass);

        if (!pass) { uring_close(e); return 2; }
        if (o.cycles && j.cycle >= o.cycles) break;
        if (g_stop) { printf("stop requested\n"); break; }

        uring_close(e);
        e = uring_open(device, o.qd, URING_WRITE | (o.use_direct ? URING_DIRECT : 0));
        if (!e) { fprintf(stderr, "pcwrite: reopen (write) failed\n"); return 1; }
    }

    uring_close(e);
    return 0;
}

int pcscan_cmd(char* device, int argc, char* argv[])
{
    PcOptions o;
    UringEngine* e;
    RangeSet rs;
    Journal j;
    ThreadInfo_t ti;
    PcReport rep;
    U32 unit_blocks;
    int pass;

    pc_defaults(&o);
    if (pc_parse(argc, argv, &o)) { fprintf(stderr, "pcscan: bad options\n"); return 1; }
    pc_default_journal(&o, device);
    crc32_init();

    if (journal_load(o.journal, &j) != 0) { fprintf(stderr, "pcscan: cannot load journal %s\n", o.journal); return 1; }

    /* the journal is the source of truth for geometry + pattern + access + seed */
    o.random_access = (strcmp(j.access, "random") == 0);
    o.pattern       = j.pattern;
    o.seed          = j.seed;   /* must match write so the random order replays */

    e = uring_open(device, o.qd, (o.use_direct ? URING_DIRECT : 0));
    if (!e) { fprintf(stderr, "pcscan: cannot open %s\n", device); return 1; }

    gDiskIOInfo.sz_block = j.blk ? j.blk : uring_block_size(e);
    gDiskIOInfo.seed     = j.seed;
    unit_blocks          = j.unit_blocks;

    if (range_parse(&rs, j.ranges, gDiskIOInfo.sz_block, uring_capacity_blocks(e), unit_blocks))
    { fprintf(stderr, "pcscan: bad ranges in journal\n"); uring_close(e); return 1; }

    pc_init_threadinfo(&ti, &o);

    if (pc_scan_phase(e, &rs, &j, &o, &ti, unit_blocks, &rep)) { fprintf(stderr, "pcscan: scan error\n"); uring_close(e); return 1; }

    pass = (rep.corrupt == 0 && rep.stale == 0);
    pc_report_print(&rep, gDiskIOInfo.sz_block, pass);

    uring_close(e);
    return pass ? 0 : 2;
}
