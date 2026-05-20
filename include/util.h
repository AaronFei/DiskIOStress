#ifndef DISKIOSTRESS_UTIL_H
#define DISKIOSTRESS_UTIL_H

#include <sys/time.h>
#include <stdio.h>
#include "types.h"

/* dbg_printf depends on gDiskIOInfo, so include globals from translation units that use it. */

U32     get_timeval_sec(struct timeval base_timeval);
double  get_timeval_sec_usec(struct timeval base_timeval);
U32     get_core_number(void);
void    set_process_priority(S32 priority);

void    toLowerCase(char* src, char* dest, int len);
U64     hex2dec(char* buf);
void    dump_buffer(char* buf, U32 len);
char*   str_trim(char* s);

void    crc32_init(void);
U32     crc32(const void* data, U32 len);

/* Parse "4096" / "0x1000" / "64K" / "1M" / "2G" / "1T" (1024-based) into bytes.
 * Returns 0 on success, -1 on malformed input. */
int     parse_size(const char* s, U64* out);

/* Parse "300" / "30s" / "5m" / "2h" / "1d" into seconds. 0 ok, -1 malformed. */
int     parse_duration(const char* s, U32* out);

#endif /* DISKIOSTRESS_UTIL_H */
