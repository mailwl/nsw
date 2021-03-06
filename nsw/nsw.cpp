
#include <Windows.h>

#include <ida.hpp>
#include <idp.hpp>
#include <typeinf.hpp>
#include "idaldr.h"
#include "lz4/lz4.h"
#include <vector>
#include <string>
#include <array>

struct NpdmHeader {
    std::array<char, 4> magic;
    std::array<uchar, 8> reserved;
    uchar flags;

    uchar reserved_3;
    uchar main_thread_priority;
    uchar main_thread_cpu;
    std::array<uchar, 8> reserved_4;
    uint32_t process_category;
    uint32_t main_stack_size;
    std::array<uchar, 0x10> application_name;
    std::array<uchar, 0x40> reserved_5;
    uint32_t aci_offset;
    uint32_t aci_size;
    uint32_t acid_offset;
    uint32_t acid_size;
};

struct NsoSegmentHeader {
    uint32_t offset;
    uint32_t location;
    uint32_t size;
    union {
        uint32_t alignment;
        uint32_t bss_size;
    };
};
static_assert(sizeof(NsoSegmentHeader) == 0x10, "NsoSegmentHeader has incorrect size.");

struct NsoHeader {
    uint32_t magic;
    uchar reserved0[0xc];
    std::array<NsoSegmentHeader, 3> segments; // Text, RoData, Data (in that order)
    uint32_t bss_size;
    uchar reserved1[0x1c];
    std::array<uint32_t, 3> segments_compressed_size;
};
static_assert(sizeof(NsoHeader) == 0x6c, "NsoHeader has incorrect size.");

struct ModHeader {
    uint32_t magic;
    uint32_t dynamic_offset;
    uint32_t bss_start_offset;
    uint32_t bss_end_offset;
    uint32_t eh_frame_hdr_start_offset;
    uint32_t eh_frame_hdr_end_offset;
    uint32_t module_offset; // Offset to runtime-generated module object. typically equal to .bss base
};
static_assert(sizeof(ModHeader) == 0x1c, "ModHeader has incorrect size.");

typedef unsigned int VAddr;

#define DIR_SEP "\\"

void extract() {
    const char* src{};
    char* dst{};
    LZ4_decompress_safe(src, dst, 0, 0);
}

constexpr uint32_t MakeMagic(char a, char b, char c, char d) {
    return a | b << 8 | c << 16 | d << 24;
}

static const VAddr PROCESS_IMAGE_VADDR = 0x08000000;

struct CodeSet final {
    CodeSet() {}
    std::shared_ptr<std::vector<uchar>> memory;

    std::size_t bss_size;
    std::size_t data_size;

    struct Segment {
        std::size_t offset = 0;
        VAddr addr = 0;
        uint32_t size = 0;
    };

    Segment segments[3];
    Segment& code = segments[0];
    Segment& rodata = segments[1];
    Segment& data = segments[2];

    bool has_mod_header;
    ModHeader mod_header{};

    VAddr entrypoint{};
    ~CodeSet() = default;

};

static constexpr uint32_t PageAlignSize(uint32_t size) {
    constexpr std::size_t PAGE_BITS = 12;
    constexpr std::size_t PAGE_SIZE = 1 << PAGE_BITS;
    constexpr std::size_t PAGE_MASK = PAGE_SIZE - 1;
    return (size +PAGE_MASK) & ~PAGE_MASK;
}

static std::vector<uchar> ReadSegment(linput_t* file, const NsoSegmentHeader& header, int compressed_size) {
    std::vector<uchar> compressed_data;
    compressed_data.resize(compressed_size);

    qlseek(file, header.offset, SEEK_SET);
    if (compressed_size != qlread(file, compressed_data.data(), compressed_size)) {
        msg("Failed to read %u NSO LZ4 compressed bytes\n", compressed_size);
        return {};
    }

    std::vector<uchar> uncompressed_data;
    uncompressed_data.resize(header.size);
    const int bytes_uncompressed = LZ4_decompress_safe(
        reinterpret_cast<const char*>(compressed_data.data()),
        reinterpret_cast<char*>(uncompressed_data.data()), compressed_size, header.size);

    return uncompressed_data;
}

void LoadModule(const CodeSet& module_,VAddr base_addr) {
    
}

VAddr LoadModule(const std::string& path, VAddr load_base) {
    linput_t* file = open_linput(path.c_str(), false);
    if (file == nullptr) {
        return {};
    }

    // Read NSO header
    NsoHeader nso_header{};
    qlseek(file, 0, SEEK_SET);
    
    if (sizeof(NsoHeader) != qlread(file, &nso_header, sizeof nso_header)) {
        return {};
    }
    if (nso_header.magic != MakeMagic('N', 'S', 'O', '0')) {
        return {};
    }

    // Build program image
    CodeSet codeset;
    std::vector<uchar> program_image;
    for (int i = 0; i < nso_header.segments.size(); ++i) {
        std::vector<uchar> data = ReadSegment(file, nso_header.segments[i], nso_header.segments_compressed_size[i]);
        program_image.resize(nso_header.segments[i].location);
        program_image.insert(program_image.end(), data.begin(), data.end());
        codeset.segments[i].addr = nso_header.segments[i].location;
        codeset.segments[i].offset = nso_header.segments[i].location;
        codeset.segments[i].size = PageAlignSize(static_cast<uint32_t>(data.size()));
    }

    // MOD header pointer is at .text offset + 4
    uint32_t module_offset;
    std::memcpy(&module_offset, program_image.data() + 4, sizeof(uint32_t));

    // Read MOD header
    
    // Default .bss to size in segment header if MOD0 section doesn't exist
    codeset.bss_size = PageAlignSize(nso_header.segments[2].bss_size);
    std::memcpy(&codeset.mod_header, program_image.data() + module_offset, sizeof(ModHeader));
    codeset.has_mod_header = codeset.mod_header.magic == MakeMagic('M', 'O', 'D', '0') ;
    if (codeset.has_mod_header) {
        // Resize program image to include .bss section and page align each section
        codeset.bss_size = PageAlignSize(codeset.mod_header.bss_end_offset - codeset.mod_header.bss_start_offset);
    }
    codeset.data.size += codeset.bss_size;
    const uint32_t image_size{ PageAlignSize(static_cast<uint32_t>(program_image.size()) + codeset.bss_size) };
    program_image.resize(image_size);

    // Load codeset
    codeset.memory = std::make_shared<std::vector<uchar>>(std::move(program_image));
    LoadModule(codeset, load_base);

    return load_base + image_size;
}

int idaapi accept_file(qstring *fileformatname, qstring *processor, linput_t *li, const char *filename) {
    qlseek(li, 0, SEEK_SET);
    if (qlsize(li) < sizeof NpdmHeader) {
        return 0;
    }
    NpdmHeader h;
    qlread(li, &h, sizeof NpdmHeader);
    if(qstrcmp(h.magic.data(), "META")) {
        return 0;
    }
    *processor = "arm";
    *fileformatname = "Switch ExeFS";
    return 1 | ACCEPT_FIRST;
}

void idaapi load_file(linput_t *li, ushort neflags, const char *fileformatname) {
    inf.start_ip = PROCESS_IMAGE_VADDR;
    inf.lflags |= LFLG_64BIT;
    set_compiler_id(COMP_GNU);
    add_til("gnulnx_arm64", ADDTIL_DEFAULT);

    char directory[0x100];
    GetCurrentDirectory(0x100, directory);
    // Load NSO modules
    VAddr next_load_addr{ PROCESS_IMAGE_VADDR };
    for (const auto& module : { "rtld", "main", "subsdk0", "subsdk1", "subsdk2", "subsdk3",
        "subsdk4", "subsdk5", "subsdk6", "subsdk7", "sdk" }) {
        const std::string path = std::string(directory) + DIR_SEP + module;
        const VAddr load_addr = next_load_addr;
        next_load_addr = LoadModule(path, load_addr);
        if (next_load_addr) {
            msg("loaded module %s @ 0x%X\n", module, load_addr);
        }
        else {
            next_load_addr = load_addr;
        }
    }
}

loader_t LDSC{
    IDP_INTERFACE_VERSION,
    0,
    accept_file,
    load_file,
};
