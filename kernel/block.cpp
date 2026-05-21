#include "block.hpp"

#include "serial.hpp"

namespace kernel {

static BlockDevice* g_devices[8] = {};

bool block_register_device(BlockDevice* dev) {
    if (dev == nullptr) return false;
    for (int i = 0; i < 8; ++i) {
        if (g_devices[i] == nullptr) {
            g_devices[i] = dev;
            kernel::serial::write("[block]: registered device ");
            kernel::serial::write(dev->name ? dev->name : "unnamed");
            kernel::serial::write("\n");
            return true;
        }
    }
    return false;
}

BlockDevice* block_get_primary() {
    return g_devices[0];
}

BlockDevice* block_find_by_name(const char* name) {
    if (name == nullptr) return nullptr;
    for (int i = 0; i < 8; ++i) {
        if (g_devices[i] == nullptr) continue;
        const char* n = g_devices[i]->name;
        if (n == nullptr) continue;
        // simple strcmp
        int k = 0;
        while (n[k] != '\0' && name[k] != '\0' && n[k] == name[k]) ++k;
        if (n[k] == '\0' && name[k] == '\0') return g_devices[i];
    }
    return nullptr;
}

} // namespace kernel
