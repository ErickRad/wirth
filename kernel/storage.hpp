#pragma once

#include <stdint.h>

namespace kernel {

struct StorageHandle {
    uint32_t start_lba;
    uint32_t sectors;
};

void storage_init();
bool storage_ready();
uint32_t storage_data_start_lba();
uint32_t storage_next_free_lba();
uint32_t storage_used_sectors();
bool storage_flush();
bool storage_write(const uint8_t* data, uint32_t size, StorageHandle* out);
bool storage_read(const StorageHandle& h, uint8_t* out_buf);
bool storage_read_raw(uint32_t start_lba, uint32_t sectors, uint8_t* out_buf);
void storage_restore_packages();
bool storage_add_index_entry(const char* name, uint32_t start_lba, uint32_t sectors);

} // namespace kernel
