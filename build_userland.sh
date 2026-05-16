#!/bin/bash
# Build userland test program as 32-bit ELF

set -e

ARCH=i386
CROSS=i686-elf

# Assemble
echo "[build] Assembling userland test program..."
$CROSS-as -32 -o /tmp/test_prog.o userland/test_prog.s

# Link with minimal entry point, producing PIE-like but fixed load address
echo "[build] Linking userland ELF..."
$CROSS-ld -32 -Ttext=0x08000000 -static -o /tmp/test_prog.elf /tmp/test_prog.o

# Extract as binary for embedding
echo "[build] Creating userland image..."
$CROSS-objcopy -O binary /tmp/test_prog.elf /tmp/test_prog.bin

# Create header with embedded binary
cat > kernel/loader/test_prog_data.hpp <<'EOF'
#pragma once

#include <cstdint>

namespace kernigham::loader {

// Embedded test userland program (32-bit ELF)
extern "C" const uint8_t g_test_prog_elf[];
extern "C" const size_t g_test_prog_elf_size;

}
EOF

# Create data object file
cat > /tmp/test_prog_data.c <<'EOF'
const unsigned char g_test_prog_elf[] __attribute__((section(".rodata"))) = {
EOF

# Embed binary as hex (limit to reasonable size for demo)
xxd -i < /tmp/test_prog.elf >> /tmp/test_prog_data.c

cat >> /tmp/test_prog_data.c <<'EOF'
};
const unsigned int g_test_prog_elf_size = sizeof(g_test_prog_elf);
EOF

# Compile embedded data
echo "[build] Compiling embedded program data..."
$CROSS-gcc -c -x c -o kernel/loader/test_prog_data.o /tmp/test_prog_data.c

echo "[build] Userland build complete"
