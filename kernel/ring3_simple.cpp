// Simple ring3 test program
// This function will be jumped to via IRET in ring3

extern "C" void simple_ring3_test() {
    // We're in ring3 now!
    // Just loop forever - no syscalls for now
    while (true) {
        asm volatile("hlt");
    }
}
