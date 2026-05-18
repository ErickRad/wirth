ARCH := i686

ifeq ($(shell command -v i686-elf-g++ >/dev/null 2>&1 && echo yes),yes)
CROSS_PREFIX := i686-elf-
else
CROSS_PREFIX :=
endif

CC := $(CROSS_PREFIX)gcc
CXX := $(CROSS_PREFIX)g++
LD := $(CROSS_PREFIX)ld

BUILD_DIR := build
ISO_DIR := $(BUILD_DIR)/isodir
GRUB_CD_BOOT := /usr/lib/grub/i386-pc/cdboot.img
GRUB_EFI_DIR := /usr/lib/grub/x86_64-efi
OVMF_CODE := /usr/share/OVMF/OVMF_CODE_4M.fd
OVMF_VARS_TEMPLATE := /usr/share/OVMF/OVMF_VARS_4M.fd
HOSTCXX := g++
HOSTCC := gcc
HOSTLD := ld

KERNEL_ELF := $(BUILD_DIR)/kernel.elf
KERNEL64_ELF := $(BUILD_DIR)/kernel64.elf
ISO_IMAGE := $(BUILD_DIR)/wirth.iso
USB_IMAGE := $(BUILD_DIR)/wirth-usb.img
UEFI_EFI_EXE := $(BUILD_DIR)/BOOTX64.EFI
UEFI_BOOT_IMAGE := $(BUILD_DIR)/efiboot.img
OVMF_VARS := $(BUILD_DIR)/OVMF_VARS.fd
TOOLCHAIN_BIN := $(BUILD_DIR)/macroBuilder
EFI_IMAGE_BUILDER := $(BUILD_DIR)/efi-image-builder
QEMU_DISPLAY := gtk

COMMON_FLAGS := -m32 -ffreestanding -fno-stack-protector -Wall -Wextra -Werror
CXXFLAGS := $(COMMON_FLAGS) -std=gnu++20 -fno-exceptions -fno-rtti -fno-use-cxa-atexit
CFLAGS := $(COMMON_FLAGS)
LDFLAGS := -m elf_i386 -T boot/linker.ld -nostdlib

COMMON_FLAGS64 := -m64 -ffreestanding -fno-stack-protector -Wall -Wextra -Werror
CXXFLAGS64 := $(COMMON_FLAGS64) -std=gnu++20 -fno-exceptions -fno-rtti -fno-use-cxa-atexit
CFLAGS64 := $(COMMON_FLAGS64)
LDFLAGS64 := -m elf_x86_64 -T boot/linker64.ld -nostdlib

OBJS := \
	$(BUILD_DIR)/boot.o \
	$(BUILD_DIR)/gdt_stub.o \
	$(BUILD_DIR)/interrupts_stub.o \
	$(BUILD_DIR)/ring3_entry_stub.o \
	$(BUILD_DIR)/kmain.o \
	$(BUILD_DIR)/multiboot2.o \
	$(BUILD_DIR)/serial.o \
	$(BUILD_DIR)/shell.o \
	$(BUILD_DIR)/interrupts.o \
	$(BUILD_DIR)/gdt.o \
	$(BUILD_DIR)/pic.o \
	$(BUILD_DIR)/pit.o \
	$(BUILD_DIR)/syscall.o \
	$(BUILD_DIR)/scheduler.o \
	$(BUILD_DIR)/ramfs.o \
		$(BUILD_DIR)/ide.o \
		$(BUILD_DIR)/storage.o \
	$(BUILD_DIR)/pmm.o \
	$(BUILD_DIR)/vmm.o \
	$(BUILD_DIR)/heap.o \
	$(BUILD_DIR)/memory.o \
	$(BUILD_DIR)/user_safety.o \
	$(BUILD_DIR)/new_delete.o \
	$(BUILD_DIR)/userland_loader.o \

OBJS64 := \
	$(BUILD_DIR)/boot64.o \
	$(BUILD_DIR)/kmain64.o \
	$(BUILD_DIR)/serial64.o \
	$(BUILD_DIR)/multiboot264.o \
	$(BUILD_DIR)/gdt64.o \
	$(BUILD_DIR)/interrupts64.o \
	$(BUILD_DIR)/ramfs64.o \
	$(BUILD_DIR)/rootfs_embedded.o

.PHONY: all clean iso run check-tools check-iso-tools check-run-tools toolchain

all: toolchain $(KERNEL_ELF)

toolchain: $(TOOLCHAIN_BIN)

$(TOOLCHAIN_BIN): tools/macroBuilder/main.cpp | $(BUILD_DIR)
	$(HOSTCXX) -std=c++20 -O2 -Wall -Wextra -Werror $< -o $@

$(EFI_IMAGE_BUILDER): tools/efiImageBuilder/main.cpp | $(BUILD_DIR)
	$(HOSTCXX) -std=c++20 -O2 -Wall -Wextra -Werror $< -o $@

check-tools:
	@command -v $(CC) >/dev/null || (echo "Compiler $(CC) not found" && exit 1)
	@command -v $(CXX) >/dev/null || (echo "Compiler $(CXX) not found" && exit 1)
	@command -v $(LD) >/dev/null || (echo "Linker $(LD) not found" && exit 1)

check-iso-tools:
	@command -v grub-mkimage >/dev/null || (echo "grub-mkimage not found" && exit 1)
	@command -v grub-mkstandalone >/dev/null || (echo "grub-mkstandalone not found" && exit 1)
	@command -v xorriso >/dev/null || (echo "xorriso not found" && exit 1)
	@test -f $(GRUB_CD_BOOT) || (echo "GRUB CD boot image not found at $(GRUB_CD_BOOT)" && exit 1)
	@test -d $(GRUB_EFI_DIR) || (echo "GRUB EFI modules not found at $(GRUB_EFI_DIR)" && exit 1)

check-run-tools:
	@command -v qemu-system-i386 >/dev/null || (echo "qemu-system-i386 not found" && exit 1)

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/boot64.o: boot/boot64.S | $(BUILD_DIR) check-tools
	$(HOSTCC) $(CFLAGS64) -c $< -o $@

$(UEFI_EFI_EXE): boot/grub.cfg | $(BUILD_DIR) check-iso-tools
	# Create a GRUB EFI image that reads its config from the FAT image at /boot/grub
	grub-mkimage -O x86_64-efi -p /boot/grub \
		part_gpt part_msdos fat iso9660 multiboot2 \
		normal configfile search all_video \
		efi_gop efi_uga gfxterm font test\
		-o $@

$(UEFI_BOOT_IMAGE): $(UEFI_EFI_EXE) $(EFI_IMAGE_BUILDER) | $(BUILD_DIR) check-iso-tools
	./$(EFI_IMAGE_BUILDER) --efi-exe $(UEFI_EFI_EXE) --out $@

$(USB_IMAGE): $(UEFI_EFI_EXE) $(KERNEL_ELF) $(KERNEL64_ELF) $(EFI_IMAGE_BUILDER) | $(BUILD_DIR) check-iso-tools
	./$(EFI_IMAGE_BUILDER) --efi-exe $(UEFI_EFI_EXE) \
		--kernel32 $(KERNEL_ELF) --kernel64 $(KERNEL64_ELF) \
		--size-mib 64 --out $@

$(BUILD_DIR)/boot.o: boot/boot.S | $(BUILD_DIR) check-tools
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/gdt_stub.o: boot/gdt.S | $(BUILD_DIR) check-tools
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/interrupts_stub.o: boot/interrupts.S | $(BUILD_DIR) check-tools
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/ring3_entry_stub.o: boot/ring3_entry.S | $(BUILD_DIR) check-tools
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kmain.o: kernel/kmain.cpp kernel/serial.hpp kernel/fs/ramfs.hpp kernel/fs/vfs.hpp | $(BUILD_DIR) check-tools
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/kmain64.o: kernel/kmain64.cpp kernel/serial.hpp kernel/boot/multiboot2.hpp kernel/arch/x86_64/gdt.hpp kernel/arch/x86_64/interrupts.hpp | $(BUILD_DIR) check-tools
	$(HOSTCXX) $(CXXFLAGS64) -c $< -o $@

$(BUILD_DIR)/multiboot2.o: kernel/boot/multiboot2.cpp kernel/boot/multiboot2.hpp kernel/serial.hpp | $(BUILD_DIR) check-tools
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/multiboot264.o: kernel/boot/multiboot2.cpp kernel/boot/multiboot2.hpp kernel/serial.hpp | $(BUILD_DIR) check-tools
	$(HOSTCXX) $(CXXFLAGS64) -c $< -o $@

$(BUILD_DIR)/serial.o: kernel/serial.cpp kernel/serial.hpp | $(BUILD_DIR) check-tools
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/serial64.o: kernel/serial.cpp kernel/serial.hpp | $(BUILD_DIR) check-tools
	$(HOSTCXX) $(CXXFLAGS64) -c $< -o $@

$(BUILD_DIR)/shell.o: kernel/shell.cpp kernel/shell.hpp kernel/fs/vfs.hpp kernel/serial.hpp kernel/task/scheduler.hpp | $(BUILD_DIR) check-tools
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/interrupts.o: kernel/arch/x86/interrupts.cpp kernel/arch/x86/interrupts.hpp kernel/serial.hpp | $(BUILD_DIR) check-tools
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/interrupts64.o: kernel/arch/x86_64/interrupts.cpp kernel/arch/x86_64/interrupts.hpp kernel/serial.hpp | $(BUILD_DIR) check-tools
	$(HOSTCXX) $(CXXFLAGS64) -c $< -o $@

$(BUILD_DIR)/ramfs64.o: kernel/fs/ramfs.cpp kernel/fs/ramfs.hpp kernel/fs/vfs.hpp kernel/sync/spinlock.hpp kernel/serial.hpp | $(BUILD_DIR) check-tools
	$(HOSTCXX) $(CXXFLAGS64) -c $< -o $@

$(BUILD_DIR)/rootfs_embedded.o: kernel/rootfs_embedded.cpp | $(BUILD_DIR) check-tools
	$(HOSTCXX) $(CXXFLAGS64) -c $< -o $@

$(BUILD_DIR)/gdt.o: kernel/arch/x86/gdt.cpp kernel/arch/x86/gdt.hpp | $(BUILD_DIR) check-tools
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/gdt64.o: kernel/arch/x86_64/gdt.cpp kernel/arch/x86_64/gdt.hpp | $(BUILD_DIR) check-tools
	$(HOSTCXX) $(CXXFLAGS64) -c $< -o $@

$(BUILD_DIR)/pic.o: kernel/arch/x86/pic.cpp kernel/arch/x86/pic.hpp kernel/arch/x86/io.hpp | $(BUILD_DIR) check-tools
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/pit.o: kernel/arch/x86/pit.cpp kernel/arch/x86/pit.hpp kernel/arch/x86/io.hpp | $(BUILD_DIR) check-tools
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/syscall.o: kernel/syscall/syscall.cpp kernel/syscall/syscall.hpp kernel/fs/vfs.hpp kernel/serial.hpp kernel/user_safety.hpp | $(BUILD_DIR) check-tools
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/scheduler.o: kernel/task/scheduler.cpp kernel/task/scheduler.hpp kernel/mm/heap.hpp kernel/mm/vmm.hpp | $(BUILD_DIR) check-tools
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/ramfs.o: kernel/fs/ramfs.cpp kernel/fs/ramfs.hpp kernel/fs/vfs.hpp kernel/sync/spinlock.hpp kernel/serial.hpp | $(BUILD_DIR) check-tools
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/ide.o: kernel/drivers/ide.cpp kernel/drivers/ide.hpp kernel/arch/x86/io.hpp kernel/serial.hpp | $(BUILD_DIR) check-tools
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/storage.o: kernel/storage.cpp kernel/storage.hpp kernel/drivers/ide.hpp kernel/serial.hpp | $(BUILD_DIR) check-tools
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/pmm.o: kernel/mm/pmm.cpp kernel/mm/pmm.hpp kernel/boot/multiboot2.hpp | $(BUILD_DIR) check-tools
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/vmm.o: kernel/mm/vmm.cpp kernel/mm/vmm.hpp | $(BUILD_DIR) check-tools
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/heap.o: kernel/mm/heap.cpp kernel/mm/heap.hpp kernel/mm/vmm.hpp kernel/mm/pmm.hpp | $(BUILD_DIR) check-tools
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/memory.o: kernel/memory.cpp | $(BUILD_DIR) check-tools
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/user_safety.o: kernel/user_safety.cpp kernel/user_safety.hpp | $(BUILD_DIR) check-tools
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/new_delete.o: kernel/mm/new_delete.cpp kernel/mm/heap.hpp | $(BUILD_DIR) check-tools
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/userland_loader.o: kernel/loader/userland.cpp kernel/loader/userland.hpp kernel/loader/elf.hpp kernel/serial.hpp | $(BUILD_DIR) check-tools
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(KERNEL_ELF): $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

$(KERNEL64_ELF): $(OBJS64)
	$(HOSTLD) $(LDFLAGS64) -o $@ $(OBJS64)

$(ISO_IMAGE): toolchain $(KERNEL_ELF) $(KERNEL64_ELF) $(UEFI_BOOT_IMAGE) boot/grub.cfg boot/rootfs.seed check-iso-tools
	@mkdir -p $(ISO_DIR)/boot/grub/i386-pc
	@mkdir -p $(ISO_DIR)/EFI/BOOT
	@cp $(KERNEL_ELF) $(ISO_DIR)/boot/kernel.elf
	@cp $(KERNEL64_ELF) $(ISO_DIR)/boot/kernel64.elf
	@cp boot/rootfs.seed $(ISO_DIR)/boot/rootfs.seed
	@cp boot/grub.cfg $(ISO_DIR)/boot/grub/grub.cfg
	@cp $(UEFI_BOOT_IMAGE) $(ISO_DIR)/EFI/BOOT/efiboot.img
	@cp $(UEFI_EFI_EXE) $(ISO_DIR)/EFI/BOOT/BOOTX64.EFI
	@cp -a /usr/lib/grub/i386-pc/* $(ISO_DIR)/boot/grub/i386-pc/
	@grub-mkimage -O i386-pc -p /boot/grub -o $(BUILD_DIR)/core.img \
		biosdisk iso9660 multiboot2 normal configfile search
	@cat $(GRUB_CD_BOOT) $(BUILD_DIR)/core.img > $(ISO_DIR)/boot/grub/bios.img
	@xorriso -as mkisofs -R \
		-b boot/grub/bios.img \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-eltorito-alt-boot \
		-e EFI/BOOT/efiboot.img \
		-no-emul-boot \
		-isohybrid-gpt-basdat \
		-o $(ISO_IMAGE) $(ISO_DIR) >/dev/null

iso: $(ISO_IMAGE)

usb: $(USB_IMAGE)

$(BUILD_DIR)/hdd.img: | $(BUILD_DIR)
	dd if=/dev/zero of=$@ bs=1M count=32 >/dev/null 2>&1 || truncate -s 32M $@

run: iso check-run-tools $(BUILD_DIR)/hdd.img
	@pkill -f "qemu-system-i386.*$(BUILD_DIR)/hdd.img" >/dev/null 2>&1 || true
	qemu-system-i386 -drive file=$(BUILD_DIR)/hdd.img,format=raw,if=ide,index=0,media=disk \
		-cdrom $(ISO_IMAGE) -vga std -serial stdio -display $(QEMU_DISPLAY)

run-uefi: iso $(BUILD_DIR)/hdd.img
	@cp $(OVMF_VARS_TEMPLATE) $(OVMF_VARS)
	qemu-system-x86_64 \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=$(OVMF_VARS) \
		-cdrom $(ISO_IMAGE) -vga std -serial stdio -display $(QEMU_DISPLAY)

run-usb: $(USB_IMAGE) $(BUILD_DIR)/hdd.img
	@cp $(OVMF_VARS_TEMPLATE) $(OVMF_VARS)
	qemu-system-x86_64 -machine q35 -device qemu-xhci \
		-drive if=none,id=usbdisk,format=raw,readonly=on,file=$(USB_IMAGE) \
		-device usb-storage,drive=usbdisk \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=$(OVMF_VARS) \
		-vga std -serial stdio -display $(QEMU_DISPLAY)

clean:
	rm -rf $(BUILD_DIR)
