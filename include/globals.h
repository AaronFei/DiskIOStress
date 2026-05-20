#ifndef DISKIOSTRESS_GLOBALS_H
#define DISKIOSTRESS_GLOBALS_H

#include <pthread.h>
#include "types.h"
#include "config.h"

extern pthread_attr_t  attr;
extern pthread_mutex_t mutex_msg;
extern pthread_mutex_t mutex_ops;

extern pthread_t* gThreads;
extern pthread_t* gTimerThread;
extern pthread_t* gRtcThread;
extern pthread_t* gOtfFwUpdThread;

extern ThreadInfo_t gThreadInfo[MAX_THREAD_NUM];
extern DiskIOInfo_t gDiskIOInfo;
extern Config_t     gConfig;

#endif /* DISKIOSTRESS_GLOBALS_H */
