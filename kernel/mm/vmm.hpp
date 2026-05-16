#pragma once

#include <stdint.h>

namespace kernel::mm::vmm {

using AddressSpace = uint32_t*;

void init();
bool map_page(uint32_t virt, uint32_t phys, bool write, bool user);
bool unmap_page(uint32_t virt);
uint32_t virt_to_phys(uint32_t virt);
void enable_paging();
bool paging_enabled();
AddressSpace kernel_address_space();
AddressSpace current_address_space();
AddressSpace create_kernel_clone_address_space();
bool switch_address_space(AddressSpace address_space);

}  // namespace kernel::mm::vmm
