#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kBytesPerSector = 512;
constexpr uint32_t kSectorsPerCluster = 8;  // 4 KiB clusters
constexpr uint32_t kReservedSectors = 1;
constexpr uint32_t kNumFats = 2;
constexpr uint32_t kRootEntryCount = 512;
constexpr uint32_t kDefaultSizeMiB = 31;
constexpr uint32_t kMediaDescriptor = 0xF8;
constexpr uint8_t kEndOfChain = 0xFF;

constexpr uint16_t kFatEoc = 0xFFFF;
constexpr uint16_t kFatFirstDataCluster = 2;

struct Options {
    std::string inputPath;
    std::string outputPath;
    uint32_t sizeMiB = kDefaultSizeMiB;
};

void write_le16(uint8_t* dst, uint16_t value) {
    dst[0] = static_cast<uint8_t>(value & 0xFFu);
    dst[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
}

void write_le32(uint8_t* dst, uint32_t value) {
    dst[0] = static_cast<uint8_t>(value & 0xFFu);
    dst[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    dst[2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
    dst[3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
}

bool read_file(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    if (size < 0) {
        return false;
    }
    out.resize(static_cast<size_t>(size));
    file.seekg(0, std::ios::beg);
    if (!out.empty()) {
        file.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
        if (!file) {
            return false;
        }
    }
    return true;
}

Options parse_args(int argc, char* argv[]) {
    Options opts;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--efi-exe" && i + 1 < argc) {
            opts.inputPath = argv[++i];
        } else if (arg == "--out" && i + 1 < argc) {
            opts.outputPath = argv[++i];
        } else if (arg == "--size-mib" && i + 1 < argc) {
            opts.sizeMiB = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " --efi-exe <BOOTX64.EFI> --out <efiboot.img> [--size-mib 64]\n";
            std::exit(0);
        } else {
            throw std::runtime_error("unknown argument: " + arg);
        }
    }
    if (opts.inputPath.empty() || opts.outputPath.empty()) {
        throw std::runtime_error("missing --efi-exe or --out");
    }
    return opts;
}

uint32_t root_dir_sectors() {
    return (kRootEntryCount * 32u + kBytesPerSector - 1u) / kBytesPerSector;
}

struct Layout {
    uint32_t totalSectors = 0;
    uint32_t sectorsPerFat = 0;
    uint32_t clusterCount = 0;
    uint32_t firstFatSector = 0;
    uint32_t rootDirSector = 0;
    uint32_t firstDataSector = 0;
};

Layout compute_layout(uint32_t sizeMiB) {
    Layout layout;
    layout.totalSectors = sizeMiB * 1024u * 1024u / kBytesPerSector;

    const uint32_t rootSectors = root_dir_sectors();
    uint32_t fatSectors = 0;
    uint32_t clusterCount = 0;

    for (int i = 0; i < 16; ++i) {
        const uint32_t dataSectors = layout.totalSectors - kReservedSectors - rootSectors - kNumFats * fatSectors;
        const uint32_t newClusterCount = dataSectors / kSectorsPerCluster;
        const uint32_t newFatSectors = ((newClusterCount + 2u) * 2u + kBytesPerSector - 1u) / kBytesPerSector;
        if (newFatSectors == fatSectors) {
            clusterCount = newClusterCount;
            break;
        }
        fatSectors = newFatSectors;
        clusterCount = newClusterCount;
    }

    layout.sectorsPerFat = fatSectors;
    layout.clusterCount = clusterCount;
    layout.firstFatSector = kReservedSectors;
    layout.rootDirSector = kReservedSectors + kNumFats * layout.sectorsPerFat;
    layout.firstDataSector = kReservedSectors + kNumFats * layout.sectorsPerFat + rootSectors;
    return layout;
}

uint32_t cluster_to_sector(const Layout& layout, uint32_t cluster) {
    return layout.firstDataSector + (cluster - kFatFirstDataCluster) * kSectorsPerCluster;
}

void set_dir_entry_name(uint8_t* entry, const std::string& name8, const std::string& ext3) {
    std::fill(entry, entry + 11, static_cast<uint8_t>(' '));
    for (size_t i = 0; i < name8.size() && i < 8; ++i) {
        entry[i] = static_cast<uint8_t>(name8[i]);
    }
    for (size_t i = 0; i < ext3.size() && i < 3; ++i) {
        entry[8 + i] = static_cast<uint8_t>(ext3[i]);
    }
}

void write_dir_entry(uint8_t* dst, const std::string& name8, const std::string& ext3,
                     uint8_t attributes, uint16_t firstCluster, uint32_t fileSize) {
    std::fill(dst, dst + 32, 0);
    set_dir_entry_name(dst, name8, ext3);
    dst[11] = attributes;
    write_le16(dst + 26, firstCluster);
    write_le32(dst + 28, fileSize);
}

void write_volume_label_entry(uint8_t* dst, const char* label11) {
    std::fill(dst, dst + 32, 0);
    for (size_t i = 0; i < 11 && label11[i] != '\0'; ++i) {
        dst[i] = static_cast<uint8_t>(label11[i]);
    }
    dst[11] = 0x08;
}

void write_dot_entry(uint8_t* dst, const std::string& name8, uint16_t cluster, uint16_t parentCluster) {
    std::fill(dst, dst + 32, 0);
    set_dir_entry_name(dst, name8, "");
    dst[11] = 0x10;
    write_le16(dst + 26, cluster);
    write_le16(dst + 20, 0);
    write_le16(dst + 24, parentCluster);
}

void write_boot_sector(std::fstream& image, const Layout& layout) {
    std::array<uint8_t, kBytesPerSector> sector{};
    sector[0] = 0xEB;
    sector[1] = 0x3C;
    sector[2] = 0x90;

    const char oem[] = "KERNIGHM";
    std::copy(oem, oem + 8, reinterpret_cast<char*>(sector.data() + 3));

    write_le16(sector.data() + 11, static_cast<uint16_t>(kBytesPerSector));
    sector[13] = static_cast<uint8_t>(kSectorsPerCluster);
    write_le16(sector.data() + 14, static_cast<uint16_t>(kReservedSectors));
    sector[16] = static_cast<uint8_t>(kNumFats);
    write_le16(sector.data() + 17, static_cast<uint16_t>(kRootEntryCount));
    write_le16(sector.data() + 19, layout.totalSectors < 65536 ? static_cast<uint16_t>(layout.totalSectors) : 0);
    sector[21] = static_cast<uint8_t>(kMediaDescriptor);
    write_le16(sector.data() + 22, static_cast<uint16_t>(layout.sectorsPerFat));
    write_le16(sector.data() + 24, 63);
    write_le16(sector.data() + 26, 255);
    write_le32(sector.data() + 28, 0);
    write_le32(sector.data() + 32, layout.totalSectors >= 65536 ? layout.totalSectors : 0);

    sector[36] = 0x80;
    sector[37] = 0;
    sector[38] = 0x29;
    write_le32(sector.data() + 39, 0x20260516u);

    const char label[] = "KERNIGHAM ESP";
    std::fill(sector.begin() + 43, sector.begin() + 54, static_cast<uint8_t>(' '));
    std::copy(label, label + std::min<size_t>(11, sizeof(label) - 1), reinterpret_cast<char*>(sector.data() + 43));

    const char fsType[] = "FAT16   ";
    std::copy(fsType, fsType + 8, reinterpret_cast<char*>(sector.data() + 54));
    sector[510] = 0x55;
    sector[511] = 0xAA;

    image.seekp(0, std::ios::beg);
    image.write(reinterpret_cast<const char*>(sector.data()), sector.size());
}

void write_fat_tables(std::fstream& image, const Layout& layout, uint32_t fileClusters) {
    const uint32_t fatBytes = layout.sectorsPerFat * kBytesPerSector;
    std::vector<uint8_t> fat(fatBytes, 0);
    auto set_entry = [&](uint16_t cluster, uint16_t value) {
        const size_t offset = static_cast<size_t>(cluster) * 2u;
        if (offset + 2 <= fat.size()) {
            write_le16(fat.data() + offset, value);
        }
    };

    set_entry(0, static_cast<uint16_t>(0xFFF8u));
    set_entry(1, kFatEoc);

    const uint16_t efiDirCluster = 2;
    const uint16_t bootDirCluster = 3;
    const uint16_t fileStartCluster = 4;

    set_entry(efiDirCluster, kFatEoc);
    set_entry(bootDirCluster, kFatEoc);
    for (uint32_t i = 0; i < fileClusters; ++i) {
        const uint16_t current = static_cast<uint16_t>(fileStartCluster + i);
        const uint16_t next = (i + 1u < fileClusters) ? static_cast<uint16_t>(current + 1u) : kFatEoc;
        set_entry(current, next);
    }

    for (uint32_t copy = 0; copy < kNumFats; ++copy) {
        const uint32_t sector = layout.firstFatSector + copy * layout.sectorsPerFat;
        image.seekp(static_cast<std::streamoff>(sector) * kBytesPerSector, std::ios::beg);
        image.write(reinterpret_cast<const char*>(fat.data()), static_cast<std::streamsize>(fat.size()));
    }
}

void write_root_directory(std::fstream& image, const Layout& layout) {
    std::vector<uint8_t> root(root_dir_sectors() * kBytesPerSector, 0);

    write_volume_label_entry(root.data() + 0, "KERNIGHAM E");
    write_dir_entry(root.data() + 32, "EFI", "", 0x10, 2, 0);

    image.seekp(static_cast<std::streamoff>(layout.rootDirSector) * kBytesPerSector, std::ios::beg);
    image.write(reinterpret_cast<const char*>(root.data()), static_cast<std::streamsize>(root.size()));
}

void write_directory_clusters(std::fstream& image, const Layout& layout, uint32_t fileSize) {
    const uint16_t efiDirCluster = 2;
    const uint16_t bootDirCluster = 3;
    const uint16_t fileStartCluster = 4;

    std::vector<uint8_t> efiCluster(kSectorsPerCluster * kBytesPerSector, 0);
    write_dot_entry(efiCluster.data() + 0, ".", efiDirCluster, 0);
    write_dot_entry(efiCluster.data() + 32, "..", 0, 0);
    write_dir_entry(efiCluster.data() + 64, "BOOT", "", 0x10, bootDirCluster, 0);

    std::vector<uint8_t> bootCluster(kSectorsPerCluster * kBytesPerSector, 0);
    write_dot_entry(bootCluster.data() + 0, ".", bootDirCluster, efiDirCluster);
    write_dot_entry(bootCluster.data() + 32, "..", efiDirCluster, 0);
    write_dir_entry(bootCluster.data() + 64, "BOOTX64", "EFI", 0x20, fileStartCluster, fileSize);

    image.seekp(static_cast<std::streamoff>(cluster_to_sector(layout, efiDirCluster)) * kBytesPerSector, std::ios::beg);
    image.write(reinterpret_cast<const char*>(efiCluster.data()), static_cast<std::streamsize>(efiCluster.size()));

    image.seekp(static_cast<std::streamoff>(cluster_to_sector(layout, bootDirCluster)) * kBytesPerSector, std::ios::beg);
    image.write(reinterpret_cast<const char*>(bootCluster.data()), static_cast<std::streamsize>(bootCluster.size()));
}

void write_file_clusters(std::fstream& image, const Layout& layout, const std::vector<uint8_t>& file) {
    const uint32_t clusterSize = kSectorsPerCluster * kBytesPerSector;
    uint32_t clustersWritten = 0;
    while (clustersWritten * clusterSize < file.size()) {
        const uint32_t offset = clustersWritten * clusterSize;
        const uint32_t remaining = static_cast<uint32_t>(file.size()) - offset;
        const uint32_t chunk = std::min(remaining, clusterSize);
        const uint32_t cluster = 4u + clustersWritten;
        image.seekp(static_cast<std::streamoff>(cluster_to_sector(layout, cluster)) * kBytesPerSector, std::ios::beg);
        image.write(reinterpret_cast<const char*>(file.data() + offset), static_cast<std::streamsize>(chunk));
        if (chunk < clusterSize) {
            std::vector<uint8_t> padding(clusterSize - chunk, 0);
            image.write(reinterpret_cast<const char*>(padding.data()), static_cast<std::streamsize>(padding.size()));
        }
        ++clustersWritten;
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        const Options opts = parse_args(argc, argv);
        std::vector<uint8_t> efiBinary;
        if (!read_file(opts.inputPath, efiBinary)) {
            std::cerr << "failed to read EFI binary: " << opts.inputPath << "\n";
            return 1;
        }

        const Layout layout = compute_layout(opts.sizeMiB);
        const uint32_t clusterSize = kSectorsPerCluster * kBytesPerSector;
        const uint32_t fileClusters = (static_cast<uint32_t>(efiBinary.size()) + clusterSize - 1u) / clusterSize;
        if (layout.clusterCount < fileClusters + 4u) {
            std::cerr << "EFI image too small for payload" << "\n";
            return 1;
        }

        const std::filesystem::path outPath(opts.outputPath);
        std::filesystem::create_directories(outPath.parent_path());
        {
            std::ofstream create(opts.outputPath, std::ios::binary | std::ios::trunc);
            if (!create) {
                std::cerr << "failed to create output image: " << opts.outputPath << "\n";
                return 1;
            }
            create.seekp(static_cast<std::streamoff>(layout.totalSectors) * kBytesPerSector - 1);
            create.put('\0');
        }

        std::fstream image(opts.outputPath, std::ios::in | std::ios::out | std::ios::binary);
        if (!image) {
            std::cerr << "failed to open output image for writing: " << opts.outputPath << "\n";
            return 1;
        }

        write_boot_sector(image, layout);
        write_fat_tables(image, layout, fileClusters);
        write_root_directory(image, layout);
        write_directory_clusters(image, layout, static_cast<uint32_t>(efiBinary.size()));
        write_file_clusters(image, layout, efiBinary);

        image.flush();
        if (!image) {
            std::cerr << "failed while writing EFI image" << "\n";
            return 1;
        }

        std::cout << "EFI boot image written: " << opts.outputPath << " (" << opts.sizeMiB << " MiB)\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << "\n";
        return 1;
    }
}
