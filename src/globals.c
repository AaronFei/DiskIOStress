#include "globals.h"

pthread_attr_t  attr;
pthread_mutex_t mutex_msg;
pthread_mutex_t mutex_ops;

pthread_t* gThreads        = NULL;
pthread_t* gTimerThread    = NULL;
pthread_t* gRtcThread      = NULL;
pthread_t* gOtfFwUpdThread = NULL;

ThreadInfo_t gThreadInfo[MAX_THREAD_NUM];
DiskIOInfo_t gDiskIOInfo;
Config_t     gConfig;
