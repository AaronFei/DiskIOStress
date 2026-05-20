#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include "subcommand.h"
#include "uring_engine.h"
#include "nvme_cmd.h"
#include "util.h"
#include "globals.h"
#include "main_app.h"
#include "powercycle.h"

const CmdTbl_t cmdList[] =
{
    {"w",       "single write cmd",     "[device] [lba] [len]",                   single_write},
    {"r",       "single read cmd",      "[device] [lba] [len]",                   single_read},
    {"t",       "single trim cmd",      "[device] [lba] [len]",                   single_trim},
    {"rw",      "random write",         "[device] [lba start] [lba end] [count]", ramdom_write},
    {"rr",      "random read",          "[device] [lba start] [lba end] [count]", ramdom_read},
    {"sw",      "sequential write",     "[device] [lba start] [lba end] [count]", sequential_write},
    {"sr",      "sequential read",      "[device] [lba start] [lba end] [count]", sequential_read},
    {"rst",     "reset controller",     "[device]",                               reset_controller},
    {"log",     "log page",             "[device] [log id]",                      log_page},
    {"fwact",   "fw activation",        "[device] [action] [slot]",               fw_activation},
    {"pcwrite", "power-cycle write",    "[device] [--ranges=.. --access=.. ...]", pcwrite_cmd},
    {"pcscan",  "power-cycle scan",     "[device] [--journal=..]",                pcscan_cmd},
    {"",        "",                     "",                                       NULL}
};

int command_parser(char* device, char* cmd_str, int cmd_argc, char* cmd_argv[])
{
    char  option[256];
    const CmdTbl_t* pCmd;
    CmdFunc_t pFunc = NULL;

    toLowerCase(cmd_str, option, strlen(cmd_str));

    pCmd = &cmdList[0];

    while (pCmd->pFunc != NULL)
    {
        if (strcmp(pCmd->cmdStr, option) == 0)
        {
            pFunc = pCmd->pFunc;
            break;
        }

        pCmd++;
    }

    if (pFunc)
    {
        /* rst + power-cycle subcommands manage their own device handle. */
        if (strcmp(pCmd->cmdStr, "rst")     != 0 &&
            strcmp(pCmd->cmdStr, "pcwrite") != 0 &&
            strcmp(pCmd->cmdStr, "pcscan")  != 0)
        {
            if (get_disk_info(device) == (U32)-1)
            {
                printf("Device[%s] not found!\n", device);
                exit(1);
            }
        }

        pFunc(device, cmd_argc, cmd_argv);
        return 0;
    }

    return 1;
}

/* Open an io_uring engine, preferring O_DIRECT but falling back to buffered. */
static UringEngine* sc_open(const char* device, int write)
{
    U32 base = write ? URING_WRITE : 0;
    UringEngine* e = uring_open(device, 32, base | URING_DIRECT);
    if (!e) e = uring_open(device, 32, base);
    return e;
}

/* One blocking io_uring op (single read/write of `len` sectors at `lba`). */
static int sc_one(UringEngine* e, int write, void* buf, U64 lba, U32 len)
{
    UringCqe cqe;
    U32 bytes = len * uring_block_size(e);
    int q = write ? uring_queue_write(e, buf, lba, len, 0, 0)
                  : uring_queue_read(e, buf, lba, len, 0);
    if (q < 0 || uring_submit(e) < 0) return -1;
    if (uring_reap(e, &cqe, 1, 1) != 1) return -1;
    return (cqe.result == (S32)bytes) ? 0 : -1;
}

int single_read(char* device, int argc, char* argv[])
{
    UringEngine* e = sc_open(device, 0);
    (void)argc;

    if (e)
    {
        U32 blk = uring_block_size(e);
        U64 lba = hex2dec(argv[0]);
        U32 len = hex2dec(argv[1]);
        unsigned char* buf = uring_alloc_buffer(e, (size_t)len * blk);

        printf("=== Read LBA[0x%08llX] Len[0x%04X] ==========\n", lba, len);
        if (buf && sc_one(e, 0, buf, lba, len) == 0) dump_buffer((char*)buf, len * blk);
        else printf("read failed\n");

        uring_free_buffer(buf);
        uring_close(e);
    }
    else printf("can not open: %s\n", device);

    return 0;
}

int single_write(char* device, int argc, char* argv[])
{
    UringEngine* e = sc_open(device, 1);
    (void)argc;

    if (e)
    {
        U32 blk = uring_block_size(e);
        U64 lba = hex2dec(argv[0]);
        U32 len = hex2dec(argv[1]);
        unsigned char* buf = uring_alloc_buffer(e, (size_t)len * blk);

        printf("=== Write LBA[0x%08llX] Len[0x%04X] ==========\n", lba, len);
        if (buf)
        {
            memset(buf, 0x5A, (size_t)len * blk);
            if (sc_one(e, 1, buf, lba, len) != 0) printf("write failed\n");
        }

        uring_free_buffer(buf);
        uring_close(e);
    }
    else printf("can not open: %s\n", device);

    return 0;
}

int single_trim(char* device, int argc, char* argv[])
{
    int fd = open(device, O_RDWR);
    (void)argc;

    if (fd != -1)
    {
        U32 blk = 512;
        U64 lba = hex2dec(argv[0]);
        U32 len = hex2dec(argv[1]);
        U64 range[2];

        if (ioctl(fd, BLKSSZGET, &blk) != 0 || blk == 0) blk = 512;
        range[0] = lba * (U64)blk;
        range[1] = (U64)len * blk;

        printf("=== Trim LBA[0x%08llX] Len[0x%04X] ==========\n", lba, len);
        if (ioctl(fd, BLKDISCARD, range) != 0) printf("BLKDISCARD failed\n");

        close(fd);
    }
    else printf("can not open: %s\n", device);

    return 0;
}

/* Sequential / random read or write throughput profile (io_uring, QD 32). */
static int sc_profile(char* device, int do_write, int random, int argc, char* argv[])
{
    UringEngine* e = sc_open(device, do_write);
    U32 blk, qd = 32, unit, ntop = 0, inflight = 0, i;
    U64 cap, bstart, bend, lba, issued = 0, completed = 0, ncmd, total = 0;
    U32 bcount;
    unsigned char** bufs;
    U32* freestk;
    UringCqe* cqes;
    struct timeval start;
    double secs;

    if (!e) { printf("can not open: %s\n", device); return 0; }
    blk = uring_block_size(e);
    cap = uring_capacity_blocks(e);

    if (argc == 3) { bstart = hex2dec(argv[0]); bend = hex2dec(argv[1]); bcount = hex2dec(argv[2]); }
    else           { bstart = 0; bend = cap; bcount = (do_write || !random) ? 0x100 : 0x8; }
    if (bcount == 0) bcount = 0x100;
    if (bend > cap)  bend = cap;
    if (bstart + bcount > bend) { printf("range too small\n"); uring_close(e); return 0; }
    unit = bcount;
    ncmd = (bend - bstart) / bcount;

    printf("=== %s%s profile : start=%llX end=%llX count=%X (%u B)  cmds=%llu ===\n",
           random ? "Random " : "Sequential ", do_write ? "Write" : "Read",
           (unsigned long long)bstart, (unsigned long long)bend, bcount, bcount * blk,
           (unsigned long long)ncmd);

    bufs    = (unsigned char**)calloc(qd, sizeof(*bufs));
    freestk = (U32*)calloc(qd, sizeof(U32));
    cqes    = (UringCqe*)calloc(qd, sizeof(UringCqe));
    for (i = 0; i < qd; i++)
    {
        bufs[i] = uring_alloc_buffer(e, (size_t)unit * blk);
        if (do_write && bufs[i]) memset(bufs[i], 0x5A, (size_t)unit * blk);
        freestk[ntop++] = i;
    }

    gettimeofday(&start, NULL);
    lba = bstart;

    while (completed < ncmd)
    {
        while (inflight < qd && issued < ncmd)
        {
            U32 bi = freestk[--ntop];
            U64 use_lba;
            if (random)
            {
                U64 span = bend - bstart - unit;
                U64 r = ((U64)rand() << 32) ^ (U64)rand();
                use_lba = bstart + (span ? (r % span) : 0);
            }
            else
            {
                use_lba = lba;
                lba += unit;
                if (lba + unit > bend) lba = bstart;
            }
            if ((do_write ? uring_queue_write(e, bufs[bi], use_lba, unit, bi, 0)
                          : uring_queue_read (e, bufs[bi], use_lba, unit, bi)) < 0)
            { freestk[ntop++] = bi; break; }
            inflight++; issued++;
        }
        if (uring_submit(e) < 0) break;
        {
            int r = uring_reap(e, cqes, qd, inflight ? 1 : 0);
            if (r < 0) break;
            for (i = 0; i < (U32)r; i++) { freestk[ntop++] = (U32)cqes[i].user_data; inflight--; completed++; total += unit; }
        }
    }

    secs = get_timeval_sec_usec(start);
    if (secs <= 0) secs = 1e-6;
    printf("=== done: %s %llu MB in %.1fs (%.1f MB/s) ===\n",
           do_write ? "wrote" : "read",
           (unsigned long long)(total * blk / SIZE_1M), secs,
           (double)total * blk / SIZE_1M / secs);

    for (i = 0; i < qd; i++) if (bufs[i]) uring_free_buffer(bufs[i]);
    free(bufs); free(freestk); free(cqes);
    uring_close(e);
    return 0;
}

int sequential_read (char* device, int argc, char* argv[]) { return sc_profile(device, 0, 0, argc, argv); }
int sequential_write(char* device, int argc, char* argv[]) { return sc_profile(device, 1, 0, argc, argv); }
int ramdom_read     (char* device, int argc, char* argv[]) { return sc_profile(device, 0, 1, argc, argv); }
int ramdom_write    (char* device, int argc, char* argv[]) { return sc_profile(device, 1, 1, argc, argv); }

int fw_activation(char* device, int argc, char* argv[])
{
    int fd = open(device, O_SYNC | O_RDWR);

    if (fd != -1)
    {
        struct timeval work_tm;
        struct nvme_firmware_log_page fw_log;
        char baseDev[11];
        int rst_fd;
        U32 loop   = atoi(argv[0]);
        U32 action = atoi(argv[1]);
        U32 slot   = atoi(argv[2]);
        FILE* fp = NULL;
        U32 binSize = 0;
        char buffer[1 * SIZE_1M];
        U32 idx;
        U32 nr_slots = 0;

        strncpy(baseDev, device, 10);

        rst_fd = open(baseDev, O_SYNC | O_RDWR);

        if (action == 1)
        {
            if (argc == 4)
            {
                fp = fopen(argv[3], "rb");

                fseek(fp, 0, SEEK_END);
                binSize = ftell(fp);
                rewind(fp);

                if (fread(buffer, 1, binSize, fp) != binSize) printf("warning: short fw read\n");
            }
            else
            {
                printf ("L%d: parameter error\n", __LINE__);
                return 1;
            }
        }

        nvme_get_log(fd, 3, (char*)&fw_log, sizeof(fw_log));

        for (idx = 0; idx < 7; idx++)
        {
            if (fw_log.frs[idx]) nr_slots++;
            else break;
        }

        for (idx = 0; idx < loop; idx++)
        {
            if (action == 1)
            {
                U32 currSize = 0;
                U32 bIdx;

                for (bIdx = 0; bIdx < binSize / (128 * SIZE_1K); bIdx++)
                {
                    nvme_fw_download(fd, &buffer[currSize], 128 * SIZE_1K, currSize);
                    currSize += (128 * SIZE_1K);
                }

                if (currSize != binSize)
                {
                    nvme_fw_download(fd, &buffer[currSize], binSize - currSize, currSize);
                }
            }

            slot = (slot - 1) % nr_slots + 1;
            printf ("%4d) fw commit action:%d slot:%d ", idx + 1, action, slot);
            gettimeofday(&work_tm, NULL);
            nvme_fw_commit(fd, action, slot);
            nvme_reset(rst_fd);

            printf ("=> %.2f seconds\n", get_timeval_sec_usec(work_tm));
            if (loop > 1) sleep(1);
            slot++;
        }

        if (fp != NULL) fclose(fp);

        close(fd);
        close(rst_fd);
    }
    else
    {
        printf("can not open: %s\n", device);
    }

    return 0;
}

int reset_controller(char* device, int argc, char* argv[])
{
    int fd;
    int idx;
    int loop;

    fd = open(device, O_RDWR);

    if (fd != -1)
    {
        if (argc)   loop = atoi(argv[0]);
        else        loop = 1;

        for (idx = 0; idx < loop; idx++)
        {
            printf("%3d) controller reset:%s\n", idx + 1, device);
            nvme_reset(fd);                 /* NVMe-only */
            usleep(500000);
        }

        close(fd);
    }
    else
    {
        printf("can not open: %s\n", device);
    }

    return 0;
}

int log_page(char* device, int argc, char* argv[])
{
    int fd;
    int logId = hex2dec(argv[0]);
    char buff[4096];
    (void)argc;

    memset(buff, 0xFF, sizeof(buff));

    fd = open(device, O_SYNC | O_RDWR);

    if (fd != -1)
    {
        nvme_get_log(fd, logId, &buff[0], sizeof(buff));

        switch (logId)
        {
            case 0x02:
            {
                LogPageSmart_t* pSmartLog = (LogPageSmart_t*)&buff[0];

                printf("TempSensor1:%d\n", KELVIN_TO_CELSIUS(pSmartLog->TempSensor1));
                printf("TempSensor2:%d\n", KELVIN_TO_CELSIUS(pSmartLog->TempSensor2));

                break;
            }
            default:
                dump_buffer((char*)&buff[0], sizeof(buff));
                break;
        }

        close(fd);
    }
    else
    {
        printf("can not open: %s\n", device);
    }

    return 0;
}
