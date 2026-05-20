#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#include <pthread.h>
#include "stress.h"
#include "uring_engine.h"
#include "range.h"
#include "pattern.h"
#include "config.h"
#include "util.h"
#include "globals.h"

static volatile sig_atomic_t s_stop = 0;
static void s_sig(int sig) { (void)sig; s_stop = 1; }

/* test-time deadline, checked inside the passes so we stop promptly */
static struct timeval g_start;
static U32 g_test_time;
static int time_up(void)
{
    struct timeval n;
    if (!g_test_time) return 0;
    gettimeofday(&n, NULL);
    return (U32)(n.tv_sec - g_start.tv_sec) >= g_test_time;
}

/* ---- device link / bus speed (best effort, from sysfs) ---- */
static int read_attr(const char* dir, const char* name, char* out, size_t n)
{
    char p[1100];
    FILE* f;
    size_t L;
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    f = fopen(p, "r");
    if (!f) return -1;
    if (!fgets(out, (int)n, f)) { fclose(f); return -1; }
    fclose(f);
    L = strlen(out);
    while (L && (out[L-1] == '\n' || out[L-1] == ' ' || out[L-1] == '\t')) out[--L] = '\0';
    return 0;
}

static void device_link_info(const char* device, char* out, size_t outlen)
{
    char path[1024], real[4096], spd[64], wid[64], ver[64];
    const char* b = strrchr(device, '/');
    b = b ? b + 1 : device;

    snprintf(out, outlen, "n/a");
    snprintf(path, sizeof(path), "/sys/block/%s", b);
    if (!realpath(path, real)) return;

    for (;;)
    {
        char* slash;
        /* PCIe (NVMe): current_link_speed + current_link_width */
        if (read_attr(real, "current_link_speed", spd, sizeof(spd)) == 0 &&
            read_attr(real, "current_link_width", wid, sizeof(wid)) == 0)
        {
            const char* gen = strstr(spd, "32") ? "Gen5" : strstr(spd, "16") ? "Gen4" :
                              strstr(spd, "8.0") ? "Gen3" : strstr(spd, "5.0") ? "Gen2" :
                              strstr(spd, "2.5") ? "Gen1" : "PCIe";
            snprintf(out, outlen, "PCIe %s x%s (%s)", gen, wid, spd);
            return;
        }
        /* USB: a device dir has both 'speed' (Mbps) and 'version' */
        if (read_attr(real, "version", ver, sizeof(ver)) == 0 &&
            read_attr(real, "speed", spd, sizeof(spd)) == 0)
        {
            long mbps = atol(spd);
            const char* nm = mbps >= 20000 ? "USB3.2 (20 Gbps)" : mbps >= 10000 ? "USB3.1 (10 Gbps)" :
                             mbps >= 5000  ? "USB3 (5 Gbps)"    : mbps >= 480   ? "USB2 (480 Mbps)" : "USB";
            char* v = ver; while (*v == ' ') v++;
            snprintf(out, outlen, "%s [USB %s]", nm, v);
            return;
        }
        slash = strrchr(real, '/');
        if (!slash || slash == real) break;
        *slash = '\0';
    }
}

/* ---- live throughput meter ----
 * multi  : 5-line ANSI dashboard (info + write 2-row sparkline + read 2-row), tty only
 * simple : one newline-terminated line every 5 s with the interval average (any env) */
#define MTR_HIST 40
typedef struct
{
    struct timeval start, last;
    U64 wbytes, rbytes;          /* cumulative bytes per phase */
    U64 last_total;              /* w+r at last multi sample */
    double w_secs, r_secs;       /* approx time spent writing / reading */
    double wcur, rcur;           /* latest write / read MB/s */
    double whist[MTR_HIST], rhist[MTR_HIST];
    int wn, rn;
    U32 loop, gen;
    int simple, rendered;
    struct timeval slast;        /* simple-mode: last printed time */
    U64 sw, sr;                  /* simple-mode: bytes at last print */
    char cur_phase;              /* phase-time accounting (for report averages) */
    struct timeval phase_t0;
} Meter;

static const char* SPARK[8] = { "▁","▂","▃","▄","▅","▆","▇","█" };

static void meter_init(Meter* m, int force_simple)
{
    memset(m, 0, sizeof(*m));
    gettimeofday(&m->start, NULL);
    m->last = m->start;
    m->slast = m->start;
    m->simple = force_simple || !isatty(STDOUT_FILENO);
}

static void meter_progress(Meter* m, U32 loop, U32 gen) { m->loop = loop; m->gen = gen; }

static void meter_push(double* h, int* n, double v)
{
    if (*n < MTR_HIST) h[(*n)++] = v;
    else { memmove(h, h + 1, (MTR_HIST - 1) * sizeof(double)); h[MTR_HIST - 1] = v; }
}

/* Build two rows (top/bottom) of a 16-level bar chart from `h`. */
static void meter_spark2(const double* h, int n, char* top, char* bot, size_t cap)
{
    double mx = 1.0;
    int i;
    size_t to = 0, bo = 0;
    for (i = 0; i < n; i++) if (h[i] > mx) mx = h[i];
    for (i = 0; i < n; i++)
    {
        int lvl = (int)(h[i] / mx * 16.0);
        const char* bc;
        const char* tc;
        if (lvl < 0) lvl = 0;
        if (lvl > 16) lvl = 16;
        bc = (lvl >= 8) ? "█" : (lvl >= 1) ? SPARK[lvl - 1] : " ";
        tc = (lvl >  8) ? SPARK[lvl - 9] : " ";
        if (bo + 4 < cap) bo += (size_t)snprintf(bot + bo, cap - bo, "%s", bc);
        if (to + 4 < cap) to += (size_t)snprintf(top + to, cap - to, "%s", tc);
    }
    top[to] = '\0'; bot[bo] = '\0';
}

static void meter_tick(Meter* m, U64 bytes, char phase)
{
    struct timeval now;
    double dt, rate, wgb, rgb;
    U64 total;
    int el;

    if (phase == 'W') m->wbytes += bytes; else m->rbytes += bytes;
    gettimeofday(&now, NULL);

    /* per-phase time at boundaries (mode-independent; for report averages) */
    if (m->cur_phase == 0) { m->cur_phase = phase; m->phase_t0 = now; }
    else if (phase != m->cur_phase)
    {
        double seg = (now.tv_sec - m->phase_t0.tv_sec) + (now.tv_usec - m->phase_t0.tv_usec) / 1e6;
        if (m->cur_phase == 'W') m->w_secs += seg; else m->r_secs += seg;
        m->cur_phase = phase; m->phase_t0 = now;
    }

    el  = (int)(now.tv_sec - m->start.tv_sec);
    wgb = (double)m->wbytes / (1024.0 * 1024 * 1024);
    rgb = (double)m->rbytes / (1024.0 * 1024 * 1024);

    if (m->simple)
    {
        double sw, sr;
        dt = (now.tv_sec - m->slast.tv_sec) + (now.tv_usec - m->slast.tv_usec) / 1e6;
        if (dt < 5.0) return;
        sw = (double)(m->wbytes - m->sw) / (1024.0 * 1024.0) / dt;
        sr = (double)(m->rbytes - m->sr) / (1024.0 * 1024.0) / dt;
        printf("[%5ds] write %7.1f MB/s  read %7.1f MB/s  (written %.2f GB, read %.2f GB)\n",
               el, sw, sr, wgb, rgb);
        fflush(stdout);
        m->slast = now; m->sw = m->wbytes; m->sr = m->rbytes;
        return;
    }

    dt = (now.tv_sec - m->last.tv_sec) + (now.tv_usec - m->last.tv_usec) / 1e6;
    if (dt < 0.25) return;
    total = m->wbytes + m->rbytes;
    rate  = (double)(total - m->last_total) / (1024.0 * 1024.0) / dt;
    if (phase == 'W') { m->wcur = rate; meter_push(m->whist, &m->wn, rate); }
    else              { m->rcur = rate; meter_push(m->rhist, &m->rn, rate); }
    m->last = now; m->last_total = total;

    {
        char wtop[256], wbot[256], rtop[256], rbot[256], pfx[64];
        int plen;
        meter_spark2(m->whist, m->wn, wtop, wbot, sizeof(wtop));
        meter_spark2(m->rhist, m->rn, rtop, rbot, sizeof(rtop));

        if (m->rendered) printf("\033[5A");                   /* back up 5 lines */
        if (m->gen)
            printf("\r\033[K[%5ds] written %.2f GB  read %.2f GB  loop %u  gen %u\n", el, wgb, rgb, m->loop, m->gen);
        else
            printf("\r\033[K[%5ds] read %.2f GB  (verify-only)\n", el, rgb);

        plen = snprintf(pfx, sizeof(pfx), "  %-5s %8.1f MB/s  ", "write", m->wcur);
        printf("\r\033[K%*s%s\n", plen, "", wtop);          /* top row: no label    */
        printf("\r\033[K%s%s\n", pfx, wbot);                /* label on bottom row  */
        plen = snprintf(pfx, sizeof(pfx), "  %-5s %8.1f MB/s  ", "read", m->rcur);
        printf("\r\033[K%*s%s\n", plen, "", rtop);
        printf("\r\033[K%s%s\n", pfx, rbot);
        m->rendered = 1;
        fflush(stdout);
    }
}

static void meter_finish(Meter* m)
{
    struct timeval now;
    if (!m->cur_phase) return;
    gettimeofday(&now, NULL);
    {
        double seg = (now.tv_sec - m->phase_t0.tv_sec) + (now.tv_usec - m->phase_t0.tv_usec) / 1e6;
        if (m->cur_phase == 'W') m->w_secs += seg; else m->r_secs += seg;
    }
    m->cur_phase = 0;
}

/* Map a workload enum to single-thread io_uring behaviour. */
static void wl_flags(U32 wl, int* random_order, int* write_each, int* reread)
{
    *random_order = 0; *write_each = 1; *reread = 1;
    switch (wl)
    {
        case WORKLOAD_SEQ_WRC:   *random_order = 0; *write_each = 1; *reread = 1; break;
        case WORKLOAD_SEQ_WRRC:  *random_order = 0; *write_each = 1; *reread = 2; break;
        case WORKLOAD_SEQ_W1RCN: *random_order = 0; *write_each = 0; *reread = 1; break;
        case WORKLOAD_RAND_WRC:  *random_order = 1; *write_each = 1; *reread = 1; break;
        default: break;
    }
}

/* pos -> LBA mapping. Without alignment: tile the range set in io_size units.
 * With alignment: place each command in an `align`-sized "slot" (with headroom
 * so an offset command never overlaps its neighbour) and apply a per-command
 * offset selected by mode (aligned / unaligned / deterministic-mixed). */
typedef struct
{
    int use_align;
    const RangeSet* rs;     /* non-align path */
    U64 astart;             /* align path: first align-aligned LBA */
    U64 slot_blocks;        /* sectors per slot */
    U64 nslots;             /* number of commands */
    U32 off_blocks;         /* fixed unaligned offset in sectors */
    U32 align_blocks;       /* align boundary in sectors */
    int off_random;         /* per-command random offset in [1, align_blocks) */
    int mode;               /* 0 aligned, 1 unaligned, 2 mixed */
    U32 seed;               /* run seed: diversifies order + mixed choice */
} Mapper;

static U64 mapper_total(const Mapper* m) { return m->use_align ? m->nslots : m->rs->total_units; }

static U64 mapper_lba(const Mapper* m, int random_order, U32 gen, U64 pos)
{
    U64 idx = random_order ? range_permute(mapper_total(m), pos, gen ^ m->seed) : pos;
    if (!m->use_align)
        return range_unit_lba(m->rs, idx);
    {
        U64 base = m->astart + idx * m->slot_blocks;
        U32 off = 0;
        int apply = (m->mode == 1) ? 1                                    /* unaligned: always */
                  : (m->mode == 2) ? (int)((((idx ^ (U64)m->seed) * 2654435761ULL) >> 16) & 1) /* mixed */
                  : 0;                                                    /* aligned: never */
        if (apply)
        {
            if (m->off_random)   /* random offset in [1, align_blocks) sectors */
                off = 1 + (U32)((((idx ^ ((U64)m->seed << 7)) * 2654435761ULL) >> 20) % (m->align_blocks - 1));
            else
                off = m->off_blocks;
        }
        return base + off;
    }
}

/* Returns 0 ok, -1 on a bad alignment request. */
static int mapper_init(Mapper* m, const RangeSet* rs, const Config_t* cfg, U32 blk, U32 unit_blocks)
{
    memset(m, 0, sizeof(*m));
    m->seed = cfg->seed;
    if (cfg->align == 0)
    {
        m->use_align = 0;
        m->rs = rs;
        return 0;
    }
    if (cfg->align <= blk || (cfg->align % blk) != 0)
    {
        fprintf(stderr, "stress: --align (%u) must be a multiple of the sector size (%u) and larger\n", cfg->align, blk);
        return -1;
    }
    {
        U32 align_blocks = cfg->align / blk;
        U64 io_round = ((unit_blocks + align_blocks - 1) / align_blocks) * align_blocks; /* io rounded up to align */
        const Range* r = &rs->ranges[0];   /* alignment mode uses the first range */
        m->use_align    = 1;
        m->slot_blocks  = io_round + align_blocks;          /* headroom so offsets never overlap */
        m->align_blocks = align_blocks;
        m->off_random   = cfg->align_offset_random;
        m->mode         = cfg->align_mode;
        if (cfg->align_offset == 0)
            m->off_blocks = 1;                              /* default: misalign by one sector */
        else
        {
            if ((cfg->align_offset % blk) != 0 || cfg->align_offset >= cfg->align)
            { fprintf(stderr, "stress: --align-offset (%u) must be a multiple of %u and < align (%u)\n", cfg->align_offset, blk, cfg->align); return -1; }
            m->off_blocks = cfg->align_offset / blk;
        }
        m->astart       = ((r->start_lba + align_blocks - 1) / align_blocks) * align_blocks;
        if (r->end_lba <= m->astart) return -1;
        m->nslots       = (r->end_lba - m->astart) / m->slot_blocks;
        if (m->nslots == 0) { fprintf(stderr, "stress: range too small for --align\n"); return -1; }
    }
    return 0;
}

/* Write every unit at generation `gen`, queue-depth bounded. */
/* Returns units actually written (may be < total if time/Ctrl-C stops it), or -1 on IO error. */
static S64 stress_write_pass(UringEngine* e, const Mapper* m, ThreadInfo_t* ti,
                             U32 gen, U32 pattern, int random_order, U32 unit_blocks,
                             U32 qd, unsigned char** bufs, U32* freestk, UringCqe* cqes,
                             Meter* mtr)
{
    U32 blk = uring_block_size(e);
    U32 unit_bytes = unit_blocks * blk;
    U32 ntop = 0, inflight = 0, i;
    U64 next = 0, done = 0, total = mapper_total(m);

    for (i = 0; i < qd; i++) freestk[ntop++] = i;

    while (done < total && !s_stop && !time_up())
    {
        while (inflight < qd && next < total)
        {
            U32 bi = freestk[--ntop];
            U64 lba = mapper_lba(m, random_order, gen, next);
            fill_unit_payload(bufs[bi], unit_blocks, blk, lba, gen, pattern);
            ti->block_count = unit_blocks;
            stamp_sector_tags(bufs[bi], ti, lba, gen);
            if (uring_queue_write(e, bufs[bi], lba, unit_blocks, bi, 0) < 0) { freestk[ntop++] = bi; break; }
            inflight++; next++;
        }
        if (uring_submit(e) < 0) return -1;
        {
            int r = uring_reap(e, cqes, qd, inflight ? 1 : 0);
            if (r < 0) return -1;
            for (i = 0; i < (U32)r; i++)
            {
                if (cqes[i].result != (S32)unit_bytes) return -1;
                freestk[ntop++] = (U32)cqes[i].user_data; inflight--; done++;
                meter_tick(mtr, unit_bytes, 'W');
            }
        }
    }

    /* Drain any still-in-flight writes so the ring is clean for the verify pass
     * (these were submitted, so they count as written). */
    while (inflight > 0)
    {
        int r = uring_reap(e, cqes, qd, 1);
        if (r < 0) return -1;
        for (i = 0; i < (U32)r; i++)
        {
            if (cqes[i].result != (S32)unit_bytes) return -1;
            freestk[ntop++] = (U32)cqes[i].user_data; inflight--; done++;
        }
    }
    return (S64)done;
}

/* Read every unit back and verify its tag against `gen`. Returns 0 PASS, 2 FAIL, <0 err. */
/* Verifies the first `nverify` units. Not cut by the time limit — so whatever was
 * written this pass is always fully read back (no written-but-unverified residue). */
/* Re-read one unit's LBA up to `retries` times; returns 1 if a re-read verifies OK
 * (transient read glitch), 0 if it stays bad (persistent). `buf` holds the last read. */
static int stress_reread(UringEngine* e, ThreadInfo_t* ti, unsigned char* buf, U64 lba,
                         U32 gen, int scrub, U32 unit_blocks, U32 retries)
{
    UringCqe cqe;
    U32 unit_bytes = unit_blocks * uring_block_size(e);
    U32 a, es;
    for (a = 0; a < retries; a++)
    {
        if (uring_queue_read(e, buf, lba, unit_blocks, 0) < 0) continue;
        if (uring_submit(e) < 0) continue;
        if (uring_reap(e, &cqe, 1, 1) != 1 || cqe.result != (S32)unit_bytes) continue;
        ti->block_count = unit_blocks;
        if ((scrub ? scrub_sector_tags(buf, ti, lba, &es)
                   : verify_sector_tags(buf, ti, lba, gen, &es)) == TAG_OK)
            return 1;   /* recovered */
    }
    return 0;           /* still bad */
}

static int stress_verify_pass(UringEngine* e, const Mapper* m, ThreadInfo_t* ti,
                              U32 gen, int scrub, int random_order, U32 unit_blocks, U32 qd,
                              unsigned char** bufs, U64* slotlba, U32* freestk,
                              UringCqe* cqes, U64* checked, Meter* mtr, U64 nverify,
                              U32 retries, int continue_on_error, U64* transient, U64* errors)
{
    U32 blk = uring_block_size(e);
    U32 unit_bytes = unit_blocks * blk;
    U32 ntop = 0, inflight = 0, i;
    U64 next = 0, done = 0, total = nverify;
    int fail = 0, io_err = 0;
    int got_mismatch = 0;
    U64 mm_lba = 0;
    U32 mm_bi = 0, mm_es = 0;
    TagError_t mm_te = TAG_OK;

    for (i = 0; i < qd; i++) freestk[ntop++] = i;

    while (done < total && !s_stop && !fail)
    {
        while (inflight < qd && next < total)
        {
            U32 bi = freestk[--ntop];
            U64 lba = mapper_lba(m, random_order, gen, next);
            slotlba[bi] = lba;
            if (uring_queue_read(e, bufs[bi], lba, unit_blocks, bi) < 0) { freestk[ntop++] = bi; break; }
            inflight++; next++;
        }
        if (uring_submit(e) < 0) return -1;
        {
            int r = uring_reap(e, cqes, qd, inflight ? 1 : 0);
            if (r < 0) return -1;
            for (i = 0; i < (U32)r; i++)
            {
                U32 bi = (U32)cqes[i].user_data;
                if (cqes[i].result != (S32)unit_bytes) { io_err = 1; fail = 1; }
                else if (!got_mismatch)
                {
                    U32 errsec = 0;
                    TagError_t te;
                    ti->block_count = unit_blocks;
                    te = scrub ? scrub_sector_tags(bufs[bi], ti, slotlba[bi], &errsec)
                               : verify_sector_tags(bufs[bi], ti, slotlba[bi], gen, &errsec);
                    if (te != TAG_OK)
                    {
                        /* record the first mismatch; classify (transient/persistent) after draining */
                        got_mismatch = 1; mm_lba = slotlba[bi]; mm_bi = bi; mm_es = errsec; mm_te = te;
                        fail = 1;
                    }
                }
                freestk[ntop++] = bi; inflight--; done++; *checked += unit_blocks;
                meter_tick(mtr, unit_bytes, scrub ? 'V' : 'R');
            }
        }
    }

    /* drain any still-in-flight reads so the ring is idle for the re-read */
    while (inflight > 0)
    {
        int r = uring_reap(e, cqes, qd, 1);
        if (r < 0) break;
        for (i = 0; i < (U32)r; i++) { freestk[ntop++] = (U32)cqes[i].user_data; inflight--; }
    }

    if (io_err) return -1;
    if (!got_mismatch) return 0;

    /* A mismatch happened — re-read the sector to classify it. A read that fails
     * once then succeeds is NOT acceptable: the device returned wrong data for a
     * sector that was correctly written, which means a firmware/controller defect.
     * We label it "transient" so the cause is clear, but it still FAILS the run. */
    if (retries > 0 && stress_reread(e, ti, bufs[mm_bi], mm_lba, gen, scrub, unit_blocks, retries))
    {
        pthread_mutex_lock(&mutex_msg);
        printf("\n%s*** TRANSIENT READ ERROR @LBA %08llX (first read: %s, re-read OK)"
               " => firmware/controller bug%s\n",
               COLOR_RED, (unsigned long long)mm_lba, tag_error_str(mm_te), COLOR_RESET);
        pthread_mutex_unlock(&mutex_msg);
        (*transient)++;
        return continue_on_error ? 0 : 2;          /* a wrong-then-right read is a defect */
    }

    pthread_mutex_lock(&mutex_msg);
    printf("\n%s*** PERSISTENT ERROR @LBA %08llX gen %u: %s (after %u re-reads)%s\n",
           COLOR_RED, (unsigned long long)mm_lba, gen, tag_error_str(mm_te), retries, COLOR_RESET);
    pthread_mutex_unlock(&mutex_msg);
    dump_sector_error(ti, bufs[mm_bi], mm_lba, mm_es, gen);
    (*errors)++;
    return continue_on_error ? 0 : 2;
}

int stress_run(const char* device, const Config_t* cfg)
{
    U32 qd = cfg->qd ? cfg->qd : 32;
    U32 io_size = cfg->io_size ? cfg->io_size : (64 * 1024);
    UringEngine* e;
    RangeSet rs;
    ThreadInfo_t ti;
    Mapper map;
    Meter meter;
    char linkinfo[128];
    unsigned char** bufs = NULL;
    U64* slotlba = NULL;
    U32* freestk = NULL;
    UringCqe* cqes = NULL;
    U32 blk, unit_blocks, unit_bytes, i;
    int random_order, write_each, reread, rc = 0, status = STATUS_PASS;
    U64 checked_total = 0, written_total = 0, transient_total = 0, error_total = 0;
    U32 loop;
    struct timeval start;

    e = uring_open(device, qd, (cfg->verify_only ? 0 : URING_WRITE) | (cfg->use_direct ? URING_DIRECT : 0));
    if (!e) { fprintf(stderr, "stress: cannot open %s (O_DIRECT? try --no-direct)\n", device); return 1; }

    blk = uring_block_size(e);
    if (io_size % blk != 0) { fprintf(stderr, "io-size must be a multiple of %u\n", blk); uring_close(e); return 1; }
    unit_blocks = io_size / blk;
    unit_bytes  = unit_blocks * blk;

    {
        const char* spec = cfg->ranges[0] ? cfg->ranges : "whole";
        if (range_parse(&rs, spec, blk, uring_capacity_blocks(e), unit_blocks))
        { fprintf(stderr, "stress: bad ranges '%s'\n", spec); uring_close(e); return 1; }
    }

    if (mapper_init(&map, &rs, cfg, blk, unit_blocks)) { uring_close(e); return 1; }

    gDiskIOInfo.sz_block = blk;
    gDiskIOInfo.seed     = cfg->seed;
    crc32_init();
    pthread_mutex_init(&mutex_msg, NULL);

    memset(&ti, 0, sizeof(ti));
    ti.id = 0; ti.cr_trunk = 0; ti.pattern_type = cfg->pattern; ti.workload = cfg->workload;
    wl_flags(cfg->workload, &random_order, &write_each, &reread);

    bufs    = (unsigned char**)calloc(qd, sizeof(*bufs));
    slotlba = (U64*)calloc(qd, sizeof(U64));
    freestk = (U32*)calloc(qd, sizeof(U32));
    cqes    = (UringCqe*)calloc(qd, sizeof(UringCqe));
    if (!bufs || !slotlba || !freestk || !cqes) { rc = 1; goto out; }
    for (i = 0; i < qd; i++) { bufs[i] = uring_alloc_buffer(e, unit_bytes); if (!bufs[i]) { rc = 1; goto out; } }

    signal(SIGINT,  s_sig);
    signal(SIGTERM, s_sig);

    device_link_info(device, linkinfo, sizeof(linkinfo));
    printf("=== stress (io_uring, single-thread) ===\n");
    printf("  device=%s  blk=%u  cap=%llu sectors (%.2f GB)\n", device, blk,
           (unsigned long long)uring_capacity_blocks(e),
           (double)uring_capacity_blocks(e) * blk / (1024.0 * 1024 * 1024));
    printf("  link=%s\n", linkinfo);
    printf("  io_size=%u  qd=%u  total_units=%llu\n", io_size, qd, (unsigned long long)mapper_total(&map));
    printf("  workload=%s  pattern=%s  seed=0x%08X  O_DIRECT=%s%s\n",
           enum_name(cfg->workload, gWorkloadMap), enum_name(cfg->pattern, gPatternMap),
           cfg->seed, cfg->use_direct ? "on" : "off", cfg->verify_only ? "  [VERIFY-ONLY]" : "");
    if (cfg->align)
        printf("  align=%u  mode=%s  (slot=%llu sectors, %llu commands)\n", cfg->align,
               cfg->align_mode == 1 ? "unaligned" : cfg->align_mode == 2 ? "mixed" : "aligned",
               (unsigned long long)map.slot_blocks, (unsigned long long)map.nslots);
    printf("=== Start Testing =========================\n");

    gettimeofday(&start, NULL);
    g_start = start;
    g_test_time = cfg->test_time;
    meter_init(&meter, cfg->simple_progress);

    /* read-only scrub: one pass, verify magic/LBA/CRC of whatever is on the device */
    if (cfg->verify_only)
    {
        int v = stress_verify_pass(e, &map, &ti, 0, 1 /*scrub*/, 0 /*seq*/, unit_blocks, qd,
                                   bufs, slotlba, freestk, cqes, &checked_total, &meter, mapper_total(&map),
                                   cfg->read_retries, cfg->continue_on_error, &transient_total, &error_total);
        status = (v < 0) ? STATUS_READ_ERROR : (v == 2) ? STATUS_COMPARE_ERROR : STATUS_PASS;
        goto done;
    }

    {
        U64 total = mapper_total(&map);
        U64 written_units = 0;   /* units written by the most recent write pass */

        for (loop = 0; loop < cfg->nr_loop && !s_stop && !time_up(); loop++)
        {
            U32 gen = write_each ? (loop + 1) : 1;
            int do_write = write_each || (loop == 0);
            int cut = 0, r;

            ti.cr_loop = loop;
            meter_progress(&meter, loop + 1, gen);

            if (do_write)
            {
                S64 w = stress_write_pass(e, &map, &ti, gen, cfg->pattern, random_order, unit_blocks, qd, bufs, freestk, cqes, &meter);
                if (w < 0) { status = STATUS_WRITE_ERROR; break; }
                written_units = (U64)w;
                written_total += written_units * unit_blocks;
                uring_flush(e);
                if (written_units < total) cut = 1;     /* stopped mid-write (time/Ctrl-C) */
            }

            /* Always read back EVERYTHING that was written (>= once), even if time is
             * up — so nothing written is left unverified. Extra rereads are skipped. */
            for (r = 0; r < reread && !s_stop && (r == 0 || !time_up()); r++)
            {
                int v = stress_verify_pass(e, &map, &ti, gen, 0 /*scrub*/, random_order, unit_blocks, qd, bufs, slotlba, freestk, cqes, &checked_total, &meter, written_units,
                                           cfg->read_retries, cfg->continue_on_error, &transient_total, &error_total);
                if (v < 0) { status = STATUS_READ_ERROR; goto done; }
                if (v == 2) { status = STATUS_COMPARE_ERROR; goto done; }
            }

            if (cfg->trim && !cut)   /* trim the tested ranges; next loop rewrites them */
            {
                static int trim_warned = 0;
                U32 ri;
                for (ri = 0; ri < rs.count; ri++)
                {
                    U64 nb = rs.ranges[ri].end_lba - rs.ranges[ri].start_lba;
                    if (uring_discard(e, rs.ranges[ri].start_lba, nb) < 0 && !trim_warned)
                    { printf("\n(note: trim/BLKDISCARD not supported here; continuing)\n"); trim_warned = 1; }
                }
            }

            if (s_stop || time_up() || cut) { status = STATUS_FORCE_STOP; break; }
        }
    }

done:
    meter_finish(&meter);
    {
        double secs = get_timeval_sec_usec(start);
        int pass;
        if ((error_total > 0 || transient_total > 0)
            && status != STATUS_READ_ERROR && status != STATUS_WRITE_ERROR)
            status = STATUS_COMPARE_ERROR;     /* persistent AND transient both fail the run */
        pass = (status == STATUS_PASS || status == STATUS_FORCE_STOP);
        const char* verdict =
            (status == STATUS_PASS)          ? "PASS" :
            (status == STATUS_FORCE_STOP)    ? "PASS (time complete)" :
            (status == STATUS_COMPARE_ERROR) ? "FAIL (compare error)" :
            (status == STATUS_WRITE_ERROR)   ? "FAIL (write error)" :
            (status == STATUS_READ_ERROR)    ? "FAIL (read error)" : "STOPPED";
        const char* col = pass ? COLOR_GREEN : COLOR_RED;
        double wgb = (double)meter.wbytes / (1024.0*1024*1024);
        double rgb = (double)meter.rbytes / (1024.0*1024*1024);
        double avgw = meter.w_secs > 0 ? (double)meter.wbytes / (1024.0*1024) / meter.w_secs : 0;
        double avgr = meter.r_secs > 0 ? (double)meter.rbytes / (1024.0*1024) / meter.r_secs : 0;

        printf("\n========== Test Report ==========\n");
        printf("  result    : %s%s%s\n", col, verdict, COLOR_RESET);
        printf("  device    : %s  (%s)\n", device, linkinfo);
        printf("  workload  : %s   pattern: %s   seed: 0x%08X\n",
               enum_name(cfg->workload, gWorkloadMap), enum_name(cfg->pattern, gPatternMap), cfg->seed);
        printf("  duration  : %.0f s\n", secs);
        if (!cfg->verify_only)
            printf("  passes    : %u (generation)\n", meter.gen);
        printf("  written   : %.2f GB   (avg %.1f MB/s)\n", wgb, avgw);
        printf("  read      : %.2f GB   (avg %.1f MB/s)\n", rgb, avgr);
        printf("  errors    : %llu persistent", (unsigned long long)error_total);
        if (transient_total) printf(", %llu transient (wrong-then-right => FW bug)", (unsigned long long)transient_total);
        printf("\n=================================\n");
        rc = pass ? 0 : 2;
    }

out:
    if (bufs) { for (i = 0; i < qd; i++) if (bufs[i]) uring_free_buffer(bufs[i]); free(bufs); }
    free(slotlba); free(freestk); free(cqes);
    uring_close(e);
    return rc;
}
