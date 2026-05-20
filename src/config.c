#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <getopt.h>
#include "config.h"
#include "util.h"
#include "main_app.h"

const NameMap_t gPatternMap[] =
{
    {"zero",         PATTERN_ALLZERO},
    {"one",          PATTERN_ALLONE},
    {"working_zero", PATTERN_WORKING_ZERO},
    {"working_one",  PATTERN_WORKING_ONE},
    {"inc_byte",     PATTERN_SEQU_INC_BYTE},
    {"dec_byte",     PATTERN_SEQU_DEC_BYTE},
    {"inc_word",     PATTERN_SEQU_INC_WORD},
    {"dec_word",     PATTERN_SEQU_DEC_WORD},
    {"inc_dword",    PATTERN_SEQU_INC_DWORD},
    {"dec_dword",    PATTERN_SEQU_DEC_DWORD},
    {"random",       PATTERN_RANDOM},
    {"addr",         PATTERN_ADDR},
    {NULL, -1}
};

/* Canonical names come first (used for display); the old short names are kept
 * after them as accepted aliases so existing configs/scripts still work. */
const NameMap_t gWorkloadMap[] =
{
    {"seq-verify",     WORKLOAD_SEQ_WRC},
    {"rand-verify",    WORKLOAD_RAND_WRC},
    {"seq-verify-2x",  WORKLOAD_SEQ_WRRC},
    {"retention",      WORKLOAD_SEQ_W1RCN},
    {"concurrent-rw",  WORKLOAD_MIX_RW},
    /* legacy aliases (still accepted, not shown as canonical) */
    {"seq_wrc",        WORKLOAD_SEQ_WRC},
    {"rand_wrc",       WORKLOAD_RAND_WRC},
    {"seq_wrrc",       WORKLOAD_SEQ_WRRC},
    {"seq_w1rcn",      WORKLOAD_SEQ_W1RCN},
    {"mix_rw",         WORKLOAD_MIX_RW},
    {NULL, -1}
};

int parse_enum(const char* s, const NameMap_t* map)
{
    int i;
    for (i = 0; map[i].name; i++)
    {
        if (strcasecmp(map[i].name, s) == 0) return map[i].val;
    }
    return -1;
}

const char* enum_name(int v, const NameMap_t* map)
{
    int i;
    for (i = 0; map[i].name; i++)
    {
        if (map[i].val == v) return map[i].name;
    }
    return "?";
}

void config_set_defaults(Config_t* cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->nr_thread = MAX_THREAD_NUM;
    cfg->qd        = 32;
    cfg->io_size   = 64 * 1024;
    cfg->use_direct = 1;
    cfg->read_retries = 3;
    cfg->rw_ratio  = 70;          /* mix_rw: 70% reads, 30% writes */
    cfg->nr_loop   = MAX_LOOP_NUM;
    cfg->sz_trunk  = MAX_TRUNK_SIZE;
    cfg->test_time = MAX_TEST_TIME;
    cfg->pattern   = DEFAULT_PATTERN;
    cfg->workload  = DEFAULT_WORKLOAD;
    cfg->seed      = 0;
    cfg->seed_set  = 0;
}

int config_apply_kv(Config_t* cfg, const char* key, const char* val, const char* src, int lineno)
{
    if (strcmp(key, "threads") == 0)
    {
        cfg->nr_thread = (U32)strtoul(val, NULL, 0);
    }
    else if (strcmp(key, "qd") == 0)
    {
        cfg->qd = (U32)strtoul(val, NULL, 0);
    }
    else if (strcmp(key, "io_size") == 0)
    {
        U64 v; if (parse_size(val, &v)) return -1; cfg->io_size = (U32)v;
    }
    else if (strcmp(key, "ranges") == 0)
    {
        strncpy(cfg->ranges, val, sizeof(cfg->ranges) - 1);
    }
    else if (strcmp(key, "trim") == 0)
    {
        cfg->trim = (U32)strtoul(val, NULL, 0) ? 1 : 0;
    }
    else if (strcmp(key, "read_retries") == 0)
    {
        cfg->read_retries = (U32)strtoul(val, NULL, 0);
    }
    else if (strcmp(key, "rw_ratio") == 0)
    {
        cfg->rw_ratio = (U32)strtoul(val, NULL, 0);
    }
    else if (strcmp(key, "continue_on_error") == 0)
    {
        cfg->continue_on_error = (U32)strtoul(val, NULL, 0) ? 1 : 0;
    }
    else if (strcmp(key, "align") == 0)
    {
        U64 v; if (parse_size(val, &v)) return -1; cfg->align = (U32)v;
    }
    else if (strcmp(key, "align_mode") == 0)
    {
        if      (!strcmp(val, "aligned"))   cfg->align_mode = 0;
        else if (!strcmp(val, "unaligned")) cfg->align_mode = 1;
        else if (!strcmp(val, "mixed"))     cfg->align_mode = 2;
        else { fprintf(stderr, "%s:%d: align_mode must be aligned|unaligned|mixed\n", src, lineno); return -1; }
    }
    else if (strcmp(key, "align_offset") == 0)
    {
        if (!strcmp(val, "random")) cfg->align_offset_random = 1;
        else { U64 v; if (parse_size(val, &v)) return -1; cfg->align_offset = (U32)v; cfg->align_offset_random = 0; }
    }
    else if (strcmp(key, "direct") == 0)
    {
        cfg->use_direct = (U32)strtoul(val, NULL, 0) ? 1 : 0;
    }
    else if (strcmp(key, "loops") == 0)
    {
        cfg->nr_loop = (U32)strtoul(val, NULL, 0);
    }
    else if (strcmp(key, "trunk_size") == 0)
    {
        cfg->sz_trunk = (U64)strtoull(val, NULL, 0) * SIZE_1M;
    }
    else if (strcmp(key, "test_time") == 0)
    {
        if (parse_duration(val, &cfg->test_time)) return -1;
    }
    else if (strcmp(key, "pattern") == 0)
    {
        int v = parse_enum(val, gPatternMap);
        if (v < 0) { fprintf(stderr, "%s:%d: unknown pattern '%s'\n", src, lineno, val); return -1; }
        cfg->pattern = (U32)v;
    }
    else if (strcmp(key, "workload") == 0)
    {
        int v = parse_enum(val, gWorkloadMap);
        if (v < 0) { fprintf(stderr, "%s:%d: unknown workload '%s'\n", src, lineno, val); return -1; }
        cfg->workload = (U32)v;
    }
    else if (strcmp(key, "seed") == 0)
    {
        U32 s = (U32)strtoul(val, NULL, 0);
        if (s != 0)
        {
            cfg->seed = s;
            cfg->seed_set = 1;
        }
    }
    else
    {
        fprintf(stderr, "%s:%d: unknown key '%s'\n", src, lineno, key);
        return -1;
    }
    return 0;
}

int config_load_file(const char* path, Config_t* cfg)
{
    FILE* fp = fopen(path, "r");
    char line[512];
    int  lineno = 0;
    int  errors = 0;

    if (!fp)
    {
        fprintf(stderr, "Cannot open config file: %s\n", path);
        return -1;
    }

    while (fgets(line, sizeof(line), fp))
    {
        char* p;
        char* eq;
        char* key;
        char* val;
        char* hash;

        lineno++;
        hash = strchr(line, '#');
        if (hash) *hash = '\0';
        hash = strchr(line, ';');
        if (hash) *hash = '\0';

        p = str_trim(line);
        if (!*p) continue;

        eq = strchr(p, '=');
        if (!eq)
        {
            fprintf(stderr, "%s:%d: missing '='\n", path, lineno);
            errors++;
            continue;
        }
        *eq = '\0';
        key = str_trim(p);
        val = str_trim(eq + 1);

        if (config_apply_kv(cfg, key, val, path, lineno) != 0) errors++;
    }

    fclose(fp);
    strncpy(cfg->config_path, path, sizeof(cfg->config_path) - 1);
    return errors ? -1 : 0;
}

int parse_cli_options(int argc, char* argv[], Config_t* cfg)
{
    static struct option long_opts[] =
    {
        {"threads",    required_argument, 0, 't'},
        {"qd",         required_argument, 0, 'q'},
        {"io-size",    required_argument, 0, 'I'},
        {"no-direct",  no_argument,       0,  1 },
        {"ranges",     required_argument, 0,  2 },
        {"trim",       no_argument,       0,  3 },
        {"align",      required_argument, 0,  4 },
        {"align-mode", required_argument, 0,  5 },
        {"align-offset", required_argument, 0, 6 },
        {"verify-only", no_argument,        0, 7 },
        {"simple-progress", no_argument,    0, 8 },
        {"read-retries", required_argument, 0, 9 },
        {"continue-on-error", no_argument,  0, 10 },
        {"rw-ratio",   required_argument, 0, 11 },
        {"loops",      required_argument, 0, 'l'},
        {"trunk-size", required_argument, 0, 'T'},
        {"test-time",  required_argument, 0, 'D'},
        {"pattern",    required_argument, 0, 'p'},
        {"workload",   required_argument, 0, 'w'},
        {"seed",       required_argument, 0, 's'},
        {"config",     required_argument, 0, 'c'},
        {"yes",        no_argument,       0, 'y'},
        {"help",       no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };
    int c;
    int i;

    /* First pass: locate --config so that subsequent CLI options override it.
     * Stop at the first positional (the device); options after it belong to a
     * subcommand (e.g. pcwrite --config=...) and must not be consumed here. */
    for (i = 1; i < argc; i++)
    {
        const char* p = NULL;
        if (argv[i][0] != '-') break;
        if (strncmp(argv[i], "--config=", 9) == 0)            p = argv[i] + 9;
        else if (strcmp(argv[i], "--config") == 0 && i+1<argc) p = argv[++i];
        else if (strcmp(argv[i], "-c") == 0 && i+1<argc)       p = argv[++i];

        if (p && config_load_file(p, cfg) != 0) exit(1);
    }

    optind = 1;
    opterr = 1;

    while ((c = getopt_long(argc, argv, "+t:q:I:l:T:D:p:w:s:c:hy", long_opts, NULL)) != -1)
    {
        switch (c)
        {
            case 't': cfg->nr_thread = (U32)strtoul(optarg, NULL, 0); break;
            case 'q': cfg->qd = (U32)strtoul(optarg, NULL, 0); break;
            case 'I': { U64 v; if (parse_size(optarg, &v)) { fprintf(stderr, "bad io-size: %s\n", optarg); exit(1); } cfg->io_size = (U32)v; break; }
            case  1 : cfg->use_direct = 0; break;
            case  2 : strncpy(cfg->ranges, optarg, sizeof(cfg->ranges) - 1); break;
            case  3 : cfg->trim = 1; break;
            case  4 : { U64 v; if (parse_size(optarg, &v)) { fprintf(stderr, "bad align: %s\n", optarg); exit(1); } cfg->align = (U32)v; break; }
            case  5 :
                if      (!strcmp(optarg, "aligned"))   cfg->align_mode = 0;
                else if (!strcmp(optarg, "unaligned")) cfg->align_mode = 1;
                else if (!strcmp(optarg, "mixed"))     cfg->align_mode = 2;
                else { fprintf(stderr, "align-mode must be aligned|unaligned|mixed\n"); exit(1); }
                break;
            case  6 :
                if (!strcmp(optarg, "random")) cfg->align_offset_random = 1;
                else { U64 v; if (parse_size(optarg, &v)) { fprintf(stderr, "bad align-offset: %s\n", optarg); exit(1); } cfg->align_offset = (U32)v; cfg->align_offset_random = 0; }
                break;
            case  7 : cfg->verify_only = 1; break;
            case  8 : cfg->simple_progress = 1; break;
            case  9 : cfg->read_retries = (U32)strtoul(optarg, NULL, 0); break;
            case 10 : cfg->continue_on_error = 1; break;
            case 11 : cfg->rw_ratio = (U32)strtoul(optarg, NULL, 0); break;
            case 'l': cfg->nr_loop   = (U32)strtoul(optarg, NULL, 0); break;
            case 'T': cfg->sz_trunk  = (U64)strtoull(optarg, NULL, 0) * SIZE_1M; break;
            case 'D': if (parse_duration(optarg, &cfg->test_time)) { fprintf(stderr, "bad test-time: %s\n", optarg); exit(1); } break;
            case 'p':
            {
                int v = parse_enum(optarg, gPatternMap);
                if (v < 0) { fprintf(stderr, "Unknown pattern: %s\n", optarg); exit(1); }
                cfg->pattern = (U32)v;
                break;
            }
            case 'w':
            {
                int v = parse_enum(optarg, gWorkloadMap);
                if (v < 0) { fprintf(stderr, "Unknown workload: %s\n", optarg); exit(1); }
                cfg->workload = (U32)v;
                break;
            }
            case 's':
                cfg->seed = (U32)strtoul(optarg, NULL, 0);
                cfg->seed_set = 1;
                break;
            case 'c': break; /* already processed */
            case 'y': cfg->assume_yes = 1; break;
            case 'h': show_usage(argc, argv); exit(0);
            default:  show_usage(argc, argv); exit(1);
        }
    }

    return optind;
}

void config_validate(Config_t* cfg)
{
    if (cfg->qd == 0 || cfg->qd > 1024)
    {
        fprintf(stderr, "qd must be in [1, 1024] (got %u)\n", cfg->qd);
        exit(1);
    }
    if (cfg->io_size < 512)
    {
        fprintf(stderr, "io-size must be >= 512 bytes (got %u)\n", cfg->io_size);
        exit(1);
    }
    if (cfg->nr_loop == 0)
    {
        fprintf(stderr, "loops must be >= 1\n");
        exit(1);
    }
    if (cfg->test_time == 0)
    {
        fprintf(stderr, "test_time must be >= 1 second\n");
        exit(1);
    }
    if (cfg->rw_ratio > 100)
    {
        fprintf(stderr, "rw-ratio must be in [0, 100] (got %u)\n", cfg->rw_ratio);
        exit(1);
    }
}

void config_print(const Config_t* cfg, const char* device)
{
    U32 tt = cfg->test_time;
    printf("=== Configuration =========================\n");
    if (cfg->config_path[0])
    printf("= Config File      : %s\n", cfg->config_path);
    printf("= Loops            : %u\n", cfg->nr_loop);
    printf("= Threads          : %u\n", cfg->nr_thread);
    printf("= Test Time        : %dd %2dh %2dm %2ds\n", tt / 86400, (tt / 3600) % 24, (tt / 60) % 60, tt % 60);
    printf("= Trunk Size       : %llu MB\n", (unsigned long long)(cfg->sz_trunk / SIZE_1M));
    printf("= Data Pattern     : %s\n", enum_name(cfg->pattern, gPatternMap));
    printf("= Workload         : %s\n", enum_name(cfg->workload, gWorkloadMap));
    printf("= Random Seed      : %u (0x%08X) %s\n", cfg->seed, cfg->seed, cfg->seed_set ? "[user]" : "[auto/time]");
    printf("= Device           : %s\n", device);
}
