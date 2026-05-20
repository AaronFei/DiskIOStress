#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <linux/fs.h>
#include "types.h"
#include "globals.h"
#include "config.h"
#include "util.h"
#include "nvme_cmd.h"
#include "stress.h"
#include "subcommand.h"
#include "pattern.h"
#include "main_app.h"

void sig_handler(int signo)
{
    (void)signo;
    printf("\nInterrupted.\n");
    exit(1);
}

U32 get_disk_info(char* device)
{
    int fd;
    U32 idx;
    int is_nvme = (strncmp(device, "/dev/nvme", 9) == 0);
    struct streams_directive_params params;
    (void)params; (void)idx;

    if (is_nvme)
        sscanf(device, "/dev/nvme%dn%d", &gDiskIOInfo.slot, &gDiskIOInfo.nsid);

    fd = open(device, O_RDONLY);

    if (fd != -1)
    {
        U32 allocCnt; (void)allocCnt;

        ioctl(fd, BLKSSZGET, &gDiskIOInfo.sz_block);
        ioctl(fd, BLKGETSIZE, &gDiskIOInfo.nr_block);
        ioctl(fd, BLKSECTGET, &gDiskIOInfo.max_sector);

        if (gDiskIOInfo.max_sector > 2048) gDiskIOInfo.max_sector = 2048;
        if (gDiskIOInfo.sz_block == 0)     gDiskIOInfo.sz_block = 512;

        gDiskIOInfo.max_sector = gDiskIOInfo.max_sector * 512 / gDiskIOInfo.sz_block;

        if (is_nvme)
        {
            nvme_identify(fd, 1, 0, (void*)&gDiskIOInfo.id_ctrl);
            nvme_identify(fd, 0, gDiskIOInfo.nsid, (void*)&gDiskIOInfo.id_ns);

            if (gDiskIOInfo.id_ctrl.oacs & NVME_CTRL_OACS_DIRECTIVES)
            {
                gDiskIOInfo.stream_support = TRUE;

                nvme_stream_enable(fd, gDiskIOInfo.nsid, TRUE);
                nvme_stream_get_param(fd, gDiskIOInfo.nsid, &gDiskIOInfo.stream_param);

                gDiskIOInfo.stream_param.sws = (1 << gDiskIOInfo.id_ns.lbaf[gDiskIOInfo.id_ns.flbas & NVME_NS_FLBAS_LBA_MASK].ds) * gDiskIOInfo.stream_param.sws / 1024;
                gDiskIOInfo.stream_param.sgs = gDiskIOInfo.stream_param.sws * gDiskIOInfo.stream_param.sgs / 1024;

                allocCnt = gDiskIOInfo.stream_param.msl - gDiskIOInfo.stream_param.nsa;

                if (allocCnt)
                {
                    nvme_stream_rel_resource(fd, gDiskIOInfo.nsid);
                    nvme_stream_alloc_resource(fd, gDiskIOInfo.nsid, gDiskIOInfo.stream_param.msl);
                }

                if (gDiskIOInfo.stream_param.nso)
                {
                    nvme_stream_get_status(fd,  gDiskIOInfo.nsid, &gDiskIOInfo.stream_status);

                    for (idx = 0; idx < gDiskIOInfo.stream_status.openCnt; idx++)
                    {
                        nvme_stream_rel_id(fd, gDiskIOInfo.nsid, gDiskIOInfo.stream_status.idTable[idx]);
                    }
                }
            }
        }

        close(fd);

        gDiskIOInfo.nr_block /= (gDiskIOInfo.sz_block / 512);
    }

    return fd;
}

void show_result(struct tm* tm_info)
{
    U32 tm;
    FILE* logFp;
    char buffer[26];

    logFp = fopen("log.txt", "a+");

    strftime(buffer, 26, "%Y:%m:%d %H:%M:%S", tm_info);

    tm = get_timeval_sec(gDiskIOInfo.timeval);

    fprintf(logFp, "=== Seed:%u (0x%08X) Threads:%u Loops:%u Trunk:%lluMB Pattern:%s Workload:%s\n",
            gDiskIOInfo.seed, gDiskIOInfo.seed,
            gConfig.nr_thread, gConfig.nr_loop,
            (unsigned long long)(gConfig.sz_trunk / SIZE_1M),
            enum_name(gConfig.pattern, gPatternMap),
            enum_name(gConfig.workload, gWorkloadMap));

    switch (gDiskIOInfo.status)
    {
        case STATUS_PASS:
            fprintf(logFp, "=== Start Time:%s Testing Time:(%2dh:%2dm:%2ds) => COMPARE PASS\n",   buffer, tm / 3600, (tm / 60) % 60, tm % 60);
            printf("\n===%s COMPARE PASS %s==========================\n", COLOR_GREEN, COLOR_RESET);
            break;
        case STATUS_COMPARE_ERROR:
            fprintf(logFp, "=== Start Time:%s Testing Time:(%2dh:%2dm:%2ds) => Compare ERROR!\n", buffer, tm / 3600, (tm / 60) % 60, tm % 60);
            printf("===%s COMPARE ERROR %s===========================\n", COLOR_RED, COLOR_RESET);
            break;
        case STATUS_WRITE_ERROR:
            fprintf(logFp, "=== Start Time:%s Testing Time:(%2dh:%2dm:%2ds) => WRITE ERROR!\n",   buffer, tm / 3600, (tm / 60) % 60, tm % 60);
            printf("===%s WRITE ERROR %s=============================\n", COLOR_RED, COLOR_RESET);
            break;
        case STATUS_READ_ERROR:
            fprintf(logFp, "=== Start Time:%s Testing Time:(%2dh:%2dm:%2ds) => READ ERROR!\n",    buffer, tm / 3600, (tm / 60) % 60, tm % 60);
            printf("===%s READ ERROR %s==============================\n", COLOR_RED, COLOR_RESET);
            break;
        case STATUS_FORCE_STOP:
            fprintf(logFp, "=== Start Time:%s Testing Time:(%2dh:%2dm:%2ds) => TIMOUET COMPLETE!\n",    buffer, tm / 3600, (tm / 60) % 60, tm % 60);
            printf("\n===%s TIMOUET COMPLETE %s======================\n", COLOR_CYAN, COLOR_RESET);
            break;
        default:
            fprintf(logFp, "=== Start Time:%s Testing Time:(%2dh:%2dm:%2ds) => UNKNOWN ERROR!\n",    buffer, tm / 3600, (tm / 60) % 60, tm % 60);
            printf("===%s UNKNOWN ERROR:%d %s=============================\n", COLOR_RED, gDiskIOInfo.status, COLOR_RESET);
            break;
    }

    fclose(logFp);
}

void show_usage(int argc, char* argv[])
{
    const CmdTbl_t* pCmd;
    int i;
    (void)argc;

    printf("Usage: %s [options] <device>                       (stress test mode)\n", argv[0]);
    printf("Usage: %s [options] <device> <subcmd> [args...]    (single-command mode)\n", argv[0]);
    printf("Example: %s --qd=64 --io-size=128K --pattern=addr /dev/sdb\n", argv[0]);
    printf("Example: %s --config=DiskIOStress.conf /dev/nvme0n1\n", argv[0]);
    printf("\n");
    printf("Stress options (single-thread io_uring, works on NVMe/SATA/USB):\n");
    printf("  -c, --config=PATH        Load INI-style config file\n");
    printf("  -q, --qd=N               io_uring queue depth (default 32)\n");
    printf("  -I, --io-size=N          IO unit size, bytes/K/M (default 64K)\n");
    printf("      --ranges=SPEC        whole (default) or 0-1G,4G-5G\n");
    printf("      --no-direct          disable O_DIRECT\n");
    printf("      --verify-only        read-only scrub: verify magic/LBA/CRC, no writes\n");
    printf("      --simple-progress    single-line 5s-average progress (any env; no multi-line)\n");
    printf("      --read-retries=N     re-read a sector on verify mismatch to classify it (default 3);\n");
    printf("                           transient (wrong-then-right) and persistent BOTH fail the run\n");
    printf("      --continue-on-error  keep running and tally errors after a mismatch (default: stop)\n");
    printf("      --trim               TRIM the range after each verify pass\n");
    printf("      --align=N            alignment boundary to test (e.g. 4K)\n");
    printf("      --align-mode=MODE    aligned | unaligned | mixed (with --align)\n");
    printf("      --align-offset=N     unaligned offset (bytes) or 'random' (default 1 sector)\n");
    printf("  -l, --loops=N            Max write/verify passes (default: run until --test-time)\n");
    printf("  -D, --test-time=N[s|m|h|d]  Run for this long, e.g. 30s / 10m / 2h (recommended)\n");
    printf("  -p, --pattern=NAME       Data pattern (see list below)\n");
    printf("  -w, --workload=NAME      Workload     (see list below)\n");
    printf("  -s, --seed=N             Random seed (0 = use time())\n");
    printf("  -y, --yes                Skip the erase confirmation (CI). Still refuses system drives\n");
    printf("  -h, --help               Show this help\n");
    printf("\n");
    printf("Patterns:");
    for (i = 0; gPatternMap[i].name; i++) printf(" %s", gPatternMap[i].name);
    printf("\nWorkloads:");
    for (i = 0; gWorkloadMap[i].name; i++) printf(" %s", gWorkloadMap[i].name);
    printf("\n\n");

    pCmd = &cmdList[0];
    printf("Sub-commands (single-command mode):\n");
    printf("-------------------------------------------------------------------------------\n");
    printf("  %-10s%-20s%-15s\n", "[Subcmd]", "[Description]", "[Parameters]");
    while (pCmd->pFunc != NULL)
    {
        printf("  %-10s%-20s%-45s\n", pCmd->cmdStr, pCmd->helpStr, pCmd->fmtStr);
        pCmd++;
    }
    printf("-------------------------------------------------------------------------------\n");
}

/* Scan /proc/mounts for filesystems backed by `device` (or its partitions).
 * Returns 0 = not mounted, 1 = mounted, 2 = carries a system mount (/ or /boot).
 * Fills `out` with a short "src on mnt, ..." description. */
static int device_mounts(const char* device, char* out, size_t outlen)
{
    FILE* fp = fopen("/proc/mounts", "r");
    char src[256], mnt[256];
    size_t tlen = strlen(device);
    int level = 0;

    out[0] = '\0';
    if (!fp) return 0;

    while (fscanf(fp, "%255s %255s %*[^\n]", src, mnt) == 2)
    {
        /* match device itself or a partition of it (sdaN / nvme0n1pN) */
        if (strncmp(src, device, tlen) == 0 &&
            (src[tlen] == '\0' || isdigit((unsigned char)src[tlen]) || src[tlen] == 'p'))
        {
            size_t l = strlen(out);
            snprintf(out + l, outlen - l, "%s%s on %s", l ? ", " : "", src, mnt);
            if (strcmp(mnt, "/") == 0 || strncmp(mnt, "/boot", 5) == 0) level = 2;
            else if (level < 1) level = 1;
        }
    }
    fclose(fp);
    return level;
}

static int is_destructive_subcmd(const char* name)
{
    return !strcasecmp(name, "w")  || !strcasecmp(name, "t")  ||
           !strcasecmp(name, "sw") || !strcasecmp(name, "rw") ||
           !strcasecmp(name, "fwact") || !strcasecmp(name, "pcwrite");
}

/* Show a clean device list, detect system drives, and confirm before erasing. */
static void confirm_target(const char* device)
{
    char mounts[512];
    char line[256];
    int level;

    printf("=== Block devices ===\n");
    fflush(stdout);
    if (system("lsblk -e 7 -o NAME,SIZE,TYPE,TRAN,MODEL,MOUNTPOINTS") != 0)
        printf("(lsblk unavailable)\n");

    level = device_mounts(device, mounts, sizeof(mounts));

    if (level == 2)
    {
        /* A system drive is never wiped unattended — even with --yes, abort loudly. */
        fprintf(stderr, "\n%s*** REFUSING: %s carries SYSTEM mounts: %s ***%s\n",
                COLOR_RED, device, mounts, COLOR_RESET);
        if (gConfig.assume_yes)
        {
            fprintf(stderr, "--yes will NOT wipe a system drive. Aborting.\n");
            exit(1);
        }
        printf("If you are SURE, type the device path exactly to proceed (%s): ", device);
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) exit(1);
        line[strcspn(line, "\n")] = '\0';
        if (strcmp(line, device) != 0) { printf("Aborted.\n"); exit(1); }
        return;
    }

    if (level == 1)
        printf("\n%sNote: %s is mounted: %s%s\n", COLOR_YELLOW, device, mounts, COLOR_RESET);

    if (gConfig.assume_yes)   /* CI / non-interactive */
    {
        printf("\n%sErasing all data on %s%s (--yes).\n", COLOR_RED, device, COLOR_RESET);
        return;
    }

    printf("\n%sThis ERASES all data on %s.%s Continue? (y/n) ", COLOR_RED, device, COLOR_RESET);
    fflush(stdout);
    if (!fgets(line, sizeof(line), stdin) || (line[0] != 'y' && line[0] != 'Y'))
    { printf("Aborted.\n"); exit(1); }
}

int main(int argc, char *argv[])
{
    char* device;
    int pos_idx;

    /* 1) Parse defaults / config / CLI */
    config_set_defaults(&gConfig);
    pos_idx = parse_cli_options(argc, argv, &gConfig);

    if (pos_idx >= argc)
    {
        show_usage(argc, argv);
        exit(1);
    }

    device = argv[pos_idx];

    /* 2) Resolve seed and seed the RNG ONCE */
    if (!gConfig.seed_set) gConfig.seed = (U32)time(NULL);
    srand(gConfig.seed);
    crc32_init();

    memset(&gDiskIOInfo, 0x00, sizeof(gDiskIOInfo));
    gDiskIOInfo.seed = gConfig.seed;

    gettimeofday(&gDiskIOInfo.timeval, NULL);
    signal(SIGINT, sig_handler);

    /* 3) Sub-command mode: device followed by w/r/t/sw/sr/rw/rr/rst/log/fwact */
    if (pos_idx + 1 < argc)
    {
        char* cmd_str  = argv[pos_idx + 1];
        int   cmd_argc = argc - (pos_idx + 2);
        char** cmd_argv = (cmd_argc > 0) ? &argv[pos_idx + 2] : NULL;

        if (is_destructive_subcmd(cmd_str)) confirm_target(device);

        if (command_parser(device, cmd_str, cmd_argc, cmd_argv))
        {
            printf("Undefined parameter!\n");
            show_usage(argc, argv);
            exit(1);
        }
    }
    else
    {
        /* Stress mode: single-thread io_uring (block-layer O_DIRECT), transport-agnostic. */
        if (!gConfig.verify_only)      /* verify-only is read-only: no confirm, no swapoff */
        {
            confirm_target(device);
            system("swapoff -a");      /* avoid swapping during a long run */
        }
        config_validate(&gConfig);
        return stress_run(device, &gConfig);
    }

    return 0;
}
