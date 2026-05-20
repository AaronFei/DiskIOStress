#ifndef DISKIOSTRESS_STRESS_H
#define DISKIOSTRESS_STRESS_H

#include "types.h"
#include "config.h"

/* Single-thread io_uring (block-layer + O_DIRECT) stress test over the whole
 * device. Works on NVMe / SATA / USB alike. Returns 0 on PASS / time-complete,
 * 2 on a data mismatch, 1 on an engine/setup error. */
int stress_run(const char* device, const Config_t* cfg);

#endif /* DISKIOSTRESS_STRESS_H */
