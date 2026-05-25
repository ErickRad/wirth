#include <stddef.h>

#include "heap.hpp"

void* operator new(size_t size) {
    return kernel::mm::heap::alloc(size, 16);
}

void* operator new[](size_t size) {
    return kernel::mm::heap::alloc(size, 16);
}

void operator delete(void* ptr) noexcept {
    kernel::mm::heap::free(ptr);
}

void operator delete[](void* ptr) noexcept {
    kernel::mm::heap::free(ptr);
}

void operator delete(void* ptr, size_t) noexcept {
    kernel::mm::heap::free(ptr);
}

void operator delete[](void* ptr, size_t) noexcept {
    kernel::mm::heap::free(ptr);
}
