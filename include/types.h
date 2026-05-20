#ifndef DISKIOSTRESS_TYPES_H
#define DISKIOSTRESS_TYPES_H

#include <sys/time.h>
#include "nvme.h"

/*===================================================
| Boolean / size / colour constants
===================================================*/
#define TRUE                1
#define FALSE               0

#define SIZE_1K             (1024)
#define SIZE_1M             (1024 * SIZE_1K)
#define SIZE_1G             (1024 * SIZE_1M)

#define MAX_THREAD_NUM      256
#define MAX_LOOP_NUM        10000000
#define MAX_TRUNK_SIZE      (20 * SIZE_1M)
#define MAX_TEST_TIME       (86400 * 7)
#define MAX_STREAM_NUM      128
#define MAX_SECTOR_COUNT    2048

#define MAX_SLEEP_TIME      15
#define MAX_SLEEP_DELAY     300
#define MIN_SLEEP_DELAY     30

#define MAX_OTF_DELAY       30
#define MIN_OTF_DELAY       15

#define COLOR_RED           "\x1b[31m"
#define COLOR_GREEN         "\x1b[32m"
#define COLOR_YELLOW        "\x1b[33m"
#define COLOR_BLUE          "\x1b[34m"
#define COLOR_MAGENTA       "\x1b[35m"
#define COLOR_CYAN          "\x1b[36m"
#define COLOR_RESET         "\x1b[0m"

#define U8_MAX              0xFF
#define U16_MAX             0xFFFF
#define U32_MAX             0xFFFFFFFF
#define U64_MAX             0xFFFFFFFFFFFFFFFF

#define S8_MAX              0x7F
#define S16_MAX             0x7FFF
#define S32_MAX             0x7FFFFFFF
#define S64_MAX             0x7FFFFFFFFFFFFFFF

#define LOG2(X)             (31 - __builtin_clz(X))
#define CELSIUS_TO_KELVIN(C) (C + 273)
#define KELVIN_TO_CELSIUS(K) (K - 273)

/*===================================================
| Width-specific integer typedefs
===================================================*/
typedef unsigned long long  U64;
typedef unsigned int        U32;
typedef unsigned short      U16;
typedef unsigned char       U8;

typedef long long           S64;
typedef int                 S32;
typedef short               S16;
typedef char                S8;

/*===================================================
| Domain enums
===================================================*/
enum
{
    STATUS_RUNNING = 0,
    STATUS_PASS,
    STATUS_COMPARE_ERROR,
    STATUS_READ_ERROR,
    STATUS_WRITE_ERROR,
    STATUS_OPEN_ERROR,
    STATUS_FORCE_STOP
};

enum
{
    OTF_DONE = 0,
    OTF_HALT
};

enum
{
    THREAD_STATUS_PAUSE = 0,
    THREAD_STATUS_RUNNING
};

enum
{
    WORKLOAD_SEQ_WRC = 0,
    WORKLOAD_SEQ_WRRC,
    WORKLOAD_SEQ_W1RCN,
    WORKLOAD_RAND_WRC,
    WORKLOAD_MIX_RW,      /* concurrent read+write at a ratio (not phase-separated) */
    WORKLOAD_MAX
};

enum
{
    PATTERN_ALLZERO = 0,
    PATTERN_ALLONE,
    PATTERN_WORKING_ZERO,
    PATTERN_WORKING_ONE,
    PATTERN_SEQU_INC_BYTE,
    PATTERN_SEQU_DEC_BYTE,
    PATTERN_SEQU_INC_WORD,
    PATTERN_SEQU_DEC_WORD,
    PATTERN_SEQU_INC_DWORD,
    PATTERN_SEQU_DEC_DWORD,
    PATTERN_RANDOM,
    PATTERN_ADDR,
    PATTERN_MAX
};

enum
{
    OPS_WRITE = 0,
    OPS_READ,
    OPS_NULL
};

#define IO_ENGINE_SYSTEM    0
#define IO_ENGINE_FILE      1
#define IO_ENGINE_NVME      2

#define DEFAULT_PATTERN     PATTERN_RANDOM
#define DEFAULT_WORKLOAD    WORKLOAD_RAND_WRC
#define DISK_IO_ENGINE      IO_ENGINE_NVME

#define SUPPORT_BLKDISCARD  TRUE
#define SUPPORT_BLKFLUSH    FALSE
#define SUPPORT_DATA_TAG    TRUE
#define SUPPORT_RE_READ     TRUE
#define SUPPORT_DATA_VERIFY TRUE
#define SUPPORT_SLEEP       FALSE
#define SUPPORT_OTF_FW_UPD  FALSE

/*===================================================
| Domain structures
===================================================*/
typedef struct
{
    U16 enable;
    U16 msl;
    U16 nsa;
    U16 nso;
} StreamDirective_t;

typedef struct
{
    U16 openCnt;
    U16 idTable[MAX_STREAM_NUM];
    U16 rsvd;
} StreamStatus_t;

typedef struct
{
    struct timeval timeval;
    U32 status;
    U32 nr_thread;
    U32 nr_loop;
    U32 nr_trunk;
    U64 sz_trunk;
    U64 nr_block;
    U32 sz_block;
    U32 max_sector;
    U32 slot;
    U32 nsid;
    U32 stream_support;
    struct nvme_id_ctrl id_ctrl;
    struct nvme_id_ns id_ns;
    struct streams_directive_params stream_param;
    StreamStatus_t stream_status;
    U32 rtc_cycle;
    U32 rtc_delay;
    U32 otf_delay;
    U32 otf_flag;
    U32 seed;
} DiskIOInfo_t;

typedef struct
{
    U32 id;
    U32 fd;
    U32 ops;
    U32 status;
    U32 nr_loop;
    U32 nr_trunk;
    U32 cr_loop;
    U32 cr_trunk;
    U64 block_per_trunk;
    U64 block_start;
    U64 block_end;
    U32 block_count;
    U32 pattern_type;
    U32 workload;
    unsigned char device_path[256];
    unsigned char* bufR;
    unsigned char* bufW;
} ThreadInfo_t;

typedef int (*CmdFunc_t)(char* device, int argc, char* argv[]);

typedef struct
{
    char* cmdStr;
    char* helpStr;
    char* fmtStr;
    CmdFunc_t pFunc;
} CmdTbl_t;

typedef struct
{
                                               /// indicates critical warnings for the state of the controller (bytes[00])
    U32 criticalWarningSpareSpace:1;           ///< available spare space has fallen below the threshold (bits[00])
    U32 criticalWarningTemperature:1;          ///< temperature has exceeded a critical threshold (bits[01])
    U32 criticalWarningMediaInternalError:1;   ///< device reliability degraded due to media related errors or internal error (bits[02])
    U32 criticalWarningReadOnlyMode:1;         ///< media has been placed in read only mode (bits[03])
    U32 criticalWarningVolatileFail:1;         ///< volatile memory backup device has failed (bits[04])
    U32 reserved:3;                            ///< Reserved (bits[07:05])
    U32 temperature:16;                        ///< Contains the temperature of the overall device (bytes[2:1])
    U32 availableSpare:8;                      ///< Contains normalized percentage of the remaining spare capacity available with (bytes[3])

    U8  availableSpareThreshold;               ///< When the Available Spare falls below the threshold indicated in this field  (bytes[4])
    U8  percentageUsed;                        ///< Contains a vendor specific estimate of the percentage of device life used (bytes[5])
    U8  reserved6[26];                         ///<  Reserved (bytes[31:6])

    U64 dataUnitsRead[2];                      ///<  Contains the number of 512 byte data units the host has read from the controller (bytes[47:32])

    U64 dataUnitsWritten[2];                   ///< the number of 512 byte data units the host has written to the controller (bytes[63:48])
    U64 hostReadCommands[2];                   ///< the number of read commands completed by the controller (bytes[79:64])
    U64 hostWriteCommands[2];                  ///< the number of write commands completed by the controller (bytes[95:80])
    U64 controllerBusyTime[2];                 ///< the amount of time the controller is busy with I/O commands (bytes[111:96])
    U64 powerCycles[2];                        ///< the number of power cycles (bytes[127:112])
    U64 powerOnHours[2];                       ///< the number of power-on hours (bytes[143:128])
    U64 unsafeShutdowns[2];                    ///< the number of unsafe shutdowns (bytes[159:144])
    U64 mediaErrors[2];                        ///< the number of occurrences where controller detected unrecovered data integrity error (bytes[175:160])
    U64 numberofErrorInformationLogEntries[2]; ///< the number of Error Information log entries over life of controller (bytes[191:176])
    U32 WarningTempTime;                       ///< Warning Composite Temperature Time (bytes[195:192])
    U32 CriticalTempTime;                      ///< Critical Composite Temperature Time (bytes[199:196])
    U16 TempSensor1;                           ///< Temperature Sensor 1 (bytes[201:200])
    U16 TempSensor2;                           ///< Temperature Sensor 2 (bytes[203:202])
    U8  reserved202[308];                      ///< Reserved (bytes[511:204])
} LogPageSmart_t;

#endif /* DISKIOSTRESS_TYPES_H */
