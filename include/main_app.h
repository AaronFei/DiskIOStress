#ifndef DISKIOSTRESS_MAIN_APP_H
#define DISKIOSTRESS_MAIN_APP_H

#include <time.h>
#include "types.h"

#define dbg_printf(color, format, ...)   {int tm = get_timeval_sec(gDiskIOInfo.timeval); fprintf(stdout, "\r%sTime(%2dh:%2dm:%2ds) S(%d) O(%d) => ", color, tm / 3600, (tm / 60) % 60, tm % 60, gDiskIOInfo.rtc_delay / 4, gDiskIOInfo.otf_delay / 4);fprintf(stdout, format, ##__VA_ARGS__);fprintf(stdout, "%s", COLOR_RESET); fflush(stdout);}

void sig_handler(int signo);
U32  get_disk_info(char* device);
void show_result(struct tm* tm_info);
void show_usage(int argc, char* argv[]);

#endif /* DISKIOSTRESS_MAIN_APP_H */
