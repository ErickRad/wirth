#pragma once

#include <stdint.h>

namespace kernel {

struct BlockDevice {
    const char* name;
    uint32_t block_size;
    uint64_t block_count;
    bool (*read_sectors)(uint64_t lba, uint8_t* buf, uint32_t count);
    bool (*write_sectors)(uint64_t lba, const uint8_t* buf, uint32_t count);
    void* ctx;
};

// Register a block device. Returns true on success.
bool block_register_device(BlockDevice* dev);

using BlockDeviceVisitor = void (*)(BlockDevice* dev, void* user_data);

// Visit all registered block devices in registration order.
bool block_visit_devices(BlockDeviceVisitor visitor, void* user_data);

// Return the primary (first) registered block device or nullptr.
BlockDevice* block_get_primary();

// Find by name
BlockDevice* block_find_by_name(const char* name);

} // namespace kernel
