#pragma once

#include <stdint.h>

namespace kernel {
namespace efifs {

// Try to locate and load /BOOT/ROOTFS.SEED from a FAT16 ESP present on a
// block device (USB mass-storage preferred). Returns true if the seed was
// found and applied into the ramfs (calls fs operations directly).
bool try_load_rootfs_seed_from_esp();

} // namespace efifs
} // namespace kernel
