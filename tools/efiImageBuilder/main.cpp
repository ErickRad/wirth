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
    std::string kernel32Path;
    std::string kernel64Path;
    std::string rootfsPath = "boot/rootfs.seed";
    std::string grubCfgPath = "boot/grub/grub.cfg";
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

        } else if (arg == "--kernel32" && i + 1 < argc) {
            opts.kernel32Path = argv[++i];

        } else if (arg == "--kernel64" && i + 1 < argc) {
            opts.kernel64Path = argv[++i];

        } else if (arg == "--out" && i + 1 < argc) {
            opts.outputPath = argv[++i];

        } else if (arg == "--rootfs" && i + 1 < argc) {
            opts.rootfsPath = argv[++i];

        } else if (arg == "--grub-cfg" && i + 1 < argc) {
            opts.grubCfgPath = argv[++i];
        
        } else if (arg == "--size-mib" && i + 1 < argc) {
            opts.sizeMiB = static_cast<uint32_t>(std::stoul(argv[++i]));

        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0]
                      << " --efi-exe <BOOTX64.EFI> [--kernel32 <kernel.elf> --kernel64 <kernel64.elf>]"
                      << " [--rootfs <rootfs.seed>] --out <efiboot.img> [--size-mib 64]\n";
            std::exit(0);

        } else {
            throw std::runtime_error("unknown argument: " + arg);
        }
    }

    if (opts.inputPath.empty() || opts.outputPath.empty()) {
        throw std::runtime_error("missing --efi-exe or --out");
    }

    if (opts.kernel32Path.empty() != opts.kernel64Path.empty()) {
        throw std::runtime_error("--kernel32 and --kernel64 must be provided together");
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

void write_dot_entry(uint8_t* dst, bool parent, uint16_t cluster) {
    std::fill(dst, dst + 32, 0);
    std::fill(dst, dst + 11, ' ');

    dst[0] = '.';

    if (parent) {
        dst[1] = '.';
    }

    dst[11] = 0x10;

    write_le16(dst + 26, cluster);
}

void write_boot_sector(std::fstream& image, const Layout& layout) {
    std::array<uint8_t, kBytesPerSector> sector{};
    
    sector[0] = 0xEB;
    sector[1] = 0x3C;
    sector[2] = 0x90;

    const char oem[] = "WIRTH";
    std::copy(oem, oem + 6, reinterpret_cast<char*>(sector.data() + 3));

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

    const char label[] = "wirth ESP";
    
    std::fill(sector.begin() + 43, sector.begin() + 54, static_cast<uint8_t>(' '));
    std::copy(label, label + std::min<size_t>(11, sizeof(label) - 1), reinterpret_cast<char*>(sector.data() + 43));

    const char fsType[] = "FAT16   ";
    std::copy(fsType, fsType + 8, reinterpret_cast<char*>(sector.data() + 54));
    
    sector[510] = 0x55;
    sector[511] = 0xAA;

    image.seekp(0, std::ios::beg);
    image.write(reinterpret_cast<const char*>(sector.data()), sector.size());
}

void write_fat_tables(std::fstream& image, const Layout& layout, uint32_t efiClusters,
                      uint32_t kernel32Clusters, uint32_t kernel64Clusters, uint32_t rootfsClusters, uint32_t grubCfgClusters,
                      bool includeKernelFiles) {
    
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
    const uint16_t rootBootDirCluster = 4;
    const uint16_t grubCfgCluster = 5;
    const uint16_t fileStartCluster = includeKernelFiles ? 6u : 4u;

    set_entry(efiDirCluster, kFatEoc);
    set_entry(bootDirCluster, kFatEoc);
    set_entry(grubCfgCluster, kFatEoc);

    if (includeKernelFiles) {
        set_entry(rootBootDirCluster, kFatEoc);
    }
    
    auto write_chain = [&](uint32_t startCluster, uint32_t numClusters) {
    
    for (uint32_t i = 0; i < numClusters; ++i) {
        const uint16_t cur = static_cast<uint16_t>(startCluster + i);
        const uint16_t next = (i + 1u < numClusters)
                              ? static_cast<uint16_t>(cur + 1u)
                              : kFatEoc;
        set_entry(cur, next);
    
        }
    };

    write_chain(fileStartCluster, efiClusters);

    if (includeKernelFiles) {
        write_chain(fileStartCluster + efiClusters, kernel32Clusters);
        write_chain(fileStartCluster + efiClusters + kernel32Clusters, kernel64Clusters);
        write_chain(fileStartCluster + efiClusters + kernel32Clusters + kernel64Clusters, rootfsClusters);
        write_chain(fileStartCluster + efiClusters + kernel32Clusters + kernel64Clusters + rootfsClusters, grubCfgClusters);
    }

    for (uint32_t copy = 0; copy < kNumFats; ++copy) {
        const uint32_t sector = layout.firstFatSector + copy * layout.sectorsPerFat;

        image.seekp(static_cast<std::streamoff>(sector) * kBytesPerSector, std::ios::beg);
        image.write(reinterpret_cast<const char*>(fat.data()), static_cast<std::streamsize>(fat.size()));
    }
}

void write_root_directory(std::fstream& image, const Layout& layout, bool includeKernelFiles) {
    std::vector<uint8_t> root(root_dir_sectors() * kBytesPerSector, 0);

    write_volume_label_entry(root.data() + 0, "wirth ESP");
    write_dir_entry(root.data() + 32, "EFI", "", 0x10, 2, 0);

    if (includeKernelFiles) {
        write_dir_entry(root.data() + 64, "BOOT", "", 0x10, 4, 0);
    }

    image.seekp(static_cast<std::streamoff>(layout.rootDirSector) * kBytesPerSector, std::ios::beg);
    image.write(reinterpret_cast<const char*>(root.data()), static_cast<std::streamsize>(root.size()));
}

void write_directory_clusters(std::fstream& image, const Layout& layout, 
    uint32_t efiFileSize, uint32_t kernel32Size, uint32_t kernel64Size, uint32_t rootfsSize, uint32_t grubCfgSize,
    uint32_t efiStartCluster, uint32_t kernel32StartCluster, uint32_t kernel64StartCluster, uint32_t rootfsStartCluster, uint32_t grubCfgStartCluster,
    bool includeKernelFiles) {

    const uint16_t efiDirCluster = 2;
    const uint16_t bootDirCluster = 3;
    const uint16_t rootBootDirCluster = 4;
    const uint16_t grubCfgCluster = 5;

    std::vector<uint8_t> efiCluster(kSectorsPerCluster * kBytesPerSector, 0);     

    write_dot_entry(efiCluster.data() + 0, false, efiDirCluster);
    write_dot_entry(efiCluster.data() + 32, true, 0);
    write_dir_entry(efiCluster.data() + 64, "BOOT", "", 0x10, bootDirCluster, 0);

    std::vector<uint8_t> bootCluster(kSectorsPerCluster * kBytesPerSector, 0);

    write_dot_entry(bootCluster.data() + 0, false, bootDirCluster);
    write_dot_entry(bootCluster.data() + 32, true, efiDirCluster);
    write_dir_entry(bootCluster.data() + 64, "BOOTX64", "EFI", 0x20,
                    static_cast<uint16_t>(efiStartCluster), efiFileSize);

    image.seekp(static_cast<std::streamoff>(cluster_to_sector(layout, efiDirCluster)) * kBytesPerSector, std::ios::beg);
    image.write(reinterpret_cast<const char*>(efiCluster.data()), static_cast<std::streamsize>(efiCluster.size()));

    image.seekp(static_cast<std::streamoff>(cluster_to_sector(layout, bootDirCluster)) * kBytesPerSector, std::ios::beg);
    image.write(reinterpret_cast<const char*>(bootCluster.data()), static_cast<std::streamsize>(bootCluster.size()));

    if (includeKernelFiles) {
        std::vector<uint8_t> rootBootCluster(kSectorsPerCluster * kBytesPerSector, 0);
        std::vector<uint8_t> grubCluster(kSectorsPerCluster * kBytesPerSector, 0);

        write_dot_entry(rootBootCluster.data() + 0, false, rootBootDirCluster);
        write_dot_entry(rootBootCluster.data() + 32, true, 0);

        write_dot_entry(grubCluster.data() + 0, false, grubCfgCluster);
        write_dot_entry(grubCluster.data() + 32, true, rootBootDirCluster);

        write_dir_entry(rootBootCluster.data() + 64, "KERNEL", "ELF", 0x20,
                        static_cast<uint16_t>(kernel32StartCluster), kernel32Size);

        write_dir_entry(rootBootCluster.data() + 96, "KERNEL64", "ELF", 0x20,
                        static_cast<uint16_t>(kernel64StartCluster), kernel64Size);

        write_dir_entry(rootBootCluster.data() + 128, "ROOTFS", "SEED", 0x20,
                        static_cast<uint16_t>(rootfsStartCluster), rootfsSize);
        
        write_dir_entry(rootBootCluster.data() + 160, "GRUB", "", 0x10, grubCfgCluster, 0);
        
        write_dir_entry(grubCluster.data() + 64, "GRUB", "CFG", 0x20,
                        static_cast<uint16_t>(grubCfgStartCluster), grubCfgSize);

        image.seekp(static_cast<std::streamoff>(cluster_to_sector(layout, rootBootDirCluster)) * kBytesPerSector,
                    std::ios::beg);

        image.write(reinterpret_cast<const char*>(rootBootCluster.data()),
                    static_cast<std::streamsize>(rootBootCluster.size()));

        image.seekp(static_cast<std::streamoff>(cluster_to_sector(layout, grubCfgCluster)) * kBytesPerSector,
                    std::ios::beg);

        image.write(reinterpret_cast<const char*>(grubCluster.data()),
                    static_cast<std::streamsize>(grubCluster.size()));
    } 
}

void write_file_clusters(std::fstream& image, const Layout& layout, const std::vector<uint8_t>& file,
                         uint16_t startCluster) {
    const uint32_t clusterSize = kSectorsPerCluster * kBytesPerSector;
    uint32_t clustersWritten = 0;

    while (clustersWritten * clusterSize < file.size()) {
        const uint32_t offset = clustersWritten * clusterSize;
        const uint32_t remaining = static_cast<uint32_t>(file.size()) - offset;

        const uint32_t chunk = std::min(remaining, clusterSize);
        const uint32_t cluster = static_cast<uint32_t>(startCluster) + clustersWritten;

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

        const bool includeKernelFiles = !opts.kernel32Path.empty();

        std::vector<uint8_t> kernel32Binary;
        std::vector<uint8_t> kernel64Binary;
        std::vector<uint8_t> rootfsBinary;
        std::vector<uint8_t> grubCfgBinary;

        if (includeKernelFiles) {
            if (!read_file(opts.kernel32Path, kernel32Binary)) {
                std::cerr << "failed to read kernel binary: " << opts.kernel32Path << "\n";
                return 1;
            }

            if (!read_file(opts.kernel64Path, kernel64Binary)) {
                std::cerr << "failed to read kernel binary: " << opts.kernel64Path << "\n";
                return 1;
            }

            if (!read_file(opts.rootfsPath, rootfsBinary)) {
                std::cerr << "failed to read rootfs seed: " << opts.rootfsPath << "\n";
                return 1;
            }

            if (!read_file(opts.grubCfgPath, grubCfgBinary)) {
                std::cerr << "failed to read GRUB config: " << opts.grubCfgPath << "\n";
                return 1;
            }
        }

        const Layout layout = compute_layout(opts.sizeMiB);
        const uint32_t clusterSize = kSectorsPerCluster * kBytesPerSector;

        const uint32_t efiClusters = (static_cast<uint32_t>(efiBinary.size()) + clusterSize - 1u) / clusterSize;

        const uint32_t kernel32Clusters = includeKernelFiles
            ? (static_cast<uint32_t>(kernel32Binary.size()) + clusterSize - 1u) / clusterSize
            : 0u;

        const uint32_t kernel64Clusters = includeKernelFiles
            ? (static_cast<uint32_t>(kernel64Binary.size()) + clusterSize - 1u) / clusterSize
            : 0u;

        const uint32_t rootfsClusters = includeKernelFiles
            ? (static_cast<uint32_t>(rootfsBinary.size()) + clusterSize - 1u) / clusterSize
            : 0u;
        
        const uint32_t grubCfgClusters = includeKernelFiles
            ? (static_cast<uint32_t>(grubCfgBinary.size()) + clusterSize - 1u) / clusterSize
            : 0u;

        const uint32_t requiredDataClusters = 
            efiClusters + kernel32Clusters + kernel64Clusters + 
            rootfsClusters + grubCfgClusters + (includeKernelFiles ? 4u : 3u);

        if (layout.clusterCount < requiredDataClusters) {
            std::cerr << "EFI image too small for payload" << "\n";

            return 1;
        }

        const std::filesystem::path outPath(opts.outputPath);

        std::filesystem::create_directories(outPath.parent_path());
        std::ofstream create(opts.outputPath, std::ios::binary | std::ios::trunc);

        if (!create) {
            std::cerr << "failed to create output image: " << opts.outputPath << "\n";
            return 1;
        }

        create.seekp(static_cast<std::streamoff>(layout.totalSectors) * kBytesPerSector - 1);
        create.put('\0');
        
        std::fstream image(opts.outputPath, std::ios::in | std::ios::out | std::ios::binary);

        if (!image) {
            std::cerr << "failed to open output image for writing: " << opts.outputPath << "\n";
            return 1;
        }

        write_boot_sector(image, layout);

        write_fat_tables(image, layout, 
            efiClusters, kernel32Clusters, kernel64Clusters, rootfsClusters, grubCfgClusters,
            includeKernelFiles);

        write_root_directory(image, layout, includeKernelFiles);

        const uint32_t efiStartCluster = includeKernelFiles ? 6u : 4u;
        const uint32_t kernel32StartCluster = efiStartCluster + efiClusters;
        const uint32_t kernel64StartCluster = kernel32StartCluster + kernel32Clusters;
        const uint32_t rootfsStartCluster = kernel64StartCluster + kernel64Clusters;
        const uint32_t grubCfgStartCluster = rootfsStartCluster + rootfsClusters;

        write_directory_clusters(image, layout, 
                                efiBinary.empty()      ? 0u : static_cast<uint32_t>(efiBinary.size()),
                                kernel32Binary.empty() ? 0u : static_cast<uint32_t>(kernel32Binary.size()),
                                kernel64Binary.empty() ? 0u : static_cast<uint32_t>(kernel64Binary.size()),
                                rootfsBinary.empty()   ? 0u : static_cast<uint32_t>(rootfsBinary.size()),
                                grubCfgBinary.empty()  ? 0u : static_cast<uint32_t>(grubCfgBinary.size()),
                                efiStartCluster, kernel32StartCluster, kernel64StartCluster, rootfsStartCluster, grubCfgStartCluster,
                                includeKernelFiles);

        write_file_clusters(image, layout, efiBinary, static_cast<uint16_t>(efiStartCluster));

        if (includeKernelFiles) {
            write_file_clusters(image, layout, kernel32Binary, static_cast<uint16_t>(kernel32StartCluster));
            write_file_clusters(image, layout, kernel64Binary, static_cast<uint16_t>(kernel64StartCluster));
            write_file_clusters(image, layout, rootfsBinary, static_cast<uint16_t>(rootfsStartCluster));
            write_file_clusters(image, layout, grubCfgBinary, static_cast<uint16_t>(grubCfgStartCluster));
        }

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
