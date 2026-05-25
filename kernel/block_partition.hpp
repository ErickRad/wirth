#pragma once

#include <stdint.h>

namespace kernel {

struct BlockDevice;

namespace block_partition {

bool scan_mbr_partitions(BlockDevice* base);

}  // namespace block_partition
}  // namespace kernel