#ifndef DISKIOSTRESS_RANGE_H
#define DISKIOSTRESS_RANGE_H

#include "types.h"

/*===================================================
| Range / access model for the power-cycle generator.
|
| A test operates over a set of LBA ranges, written in fixed-size "units"
| (io-size). A pass covers every unit in the range set exactly once; the
| traversal order is either sequential or a full-coverage random permutation.
===================================================*/

#define MAX_RANGES 64

typedef struct
{
    U64 start_lba;   /* inclusive, sectors */
    U64 end_lba;     /* exclusive, sectors */
} Range;

typedef struct
{
    Range ranges[MAX_RANGES];
    U32   count;
    U32   unit_blocks;   /* sectors per io-unit */
    U64   total_units;   /* sum of whole units across all ranges */
} RangeSet;

/* Parse a --ranges spec into rs.
 *   "whole"                cover the whole device
 *   "0-1G,4G-5G"           explicit byte ranges (K/M/G/T suffix or 0x.. / dec)
 * Byte offsets are converted to sectors using blk; ranges are clamped to
 * cap_blocks and truncated to whole units. Returns 0 ok, -1 on error. */
int range_parse(RangeSet* rs, const char* spec, U32 blk, U64 cap_blocks, U32 unit_blocks);

/* Build nregions regions of region_bytes each, spread evenly across the device. */
int range_make_regions(RangeSet* rs, U32 nregions, U64 region_bytes,
                        U32 blk, U64 cap_blocks, U32 unit_blocks);

/* Starting LBA of the unit at sequential position `unit_index` (< total_units). */
U64 range_unit_lba(const RangeSet* rs, U64 unit_index);

/* Full-coverage random permutation: bijectively map step in [0,total_units)
 * to a unit index in [0,total_units). `round` reshuffles between passes. */
U64 range_permute(U64 total_units, U64 step, U32 round);

#endif /* DISKIOSTRESS_RANGE_H */
