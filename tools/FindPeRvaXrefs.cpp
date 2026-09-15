#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {
std::uint16_t U16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    std::uint16_t value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

std::uint32_t U32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    std::uint32_t value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

std::int32_t I32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    std::int32_t value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

struct Section {
    std::string name;
    std::uint32_t rva{};
    std::uint32_t raw_size{};
    std::uint32_t raw_offset{};
};
}

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: FindPeRvaXrefs <image> <ascii-string>\n";
        return 2;
    }

    std::ifstream input(argv[1], std::ios::binary | std::ios::ate);
    if (!input) return 3;
    const auto length = static_cast<std::size_t>(input.tellg());
    input.seekg(0);
    std::vector<std::uint8_t> bytes(length);
    if (!input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(length))) return 4;

    const auto pe = U32(bytes, 0x3c);
    if (U32(bytes, pe) != 0x00004550) return 5;
    const auto coff = pe + 4;
    const auto section_count = U16(bytes, coff + 2);
    const auto optional_size = U16(bytes, coff + 16);
    const auto section_table = coff + 20 + optional_size;
    std::vector<Section> sections;
    for (std::uint16_t index = 0; index < section_count; ++index) {
        const auto header = section_table + 40 * index;
        char name[9]{};
        std::memcpy(name, bytes.data() + header, 8);
        sections.push_back({name, U32(bytes, header + 12), U32(bytes, header + 16), U32(bytes, header + 20)});
    }

    const auto text = std::find_if(sections.begin(), sections.end(), [](const Section& value) {
        return value.name == ".text";
    });
    if (text == sections.end()) return 6;

    const std::string needle = std::string(argv[2]) + '\0';
    std::vector<std::uint32_t> targets;
    for (std::size_t raw = 0; raw + needle.size() <= bytes.size(); ++raw) {
        if (std::memcmp(bytes.data() + raw, needle.data(), needle.size()) != 0) continue;
        for (const auto& section : sections) {
            if (raw >= section.raw_offset && raw < section.raw_offset + section.raw_size) {
                targets.push_back(section.rva + static_cast<std::uint32_t>(raw - section.raw_offset));
                std::cout << "string raw=0x" << std::hex << std::uppercase << raw
                          << " rva=0x" << targets.back() << std::dec << '\n';
                break;
            }
        }
    }

    for (const auto target : targets) {
        std::size_t count = 0;
        const auto end = static_cast<std::size_t>(text->raw_offset) + text->raw_size;
        for (std::size_t raw = text->raw_offset; raw + 4 <= end; ++raw) {
            const auto displacement_rva = static_cast<std::uint64_t>(text->rva) + raw - text->raw_offset;
            if (static_cast<std::int64_t>(displacement_rva + 4) + I32(bytes, raw) != target) continue;
            std::cout << "xref displacement_rva=0x" << std::hex << std::uppercase << displacement_rva
                      << " candidate_instruction_rva=0x" << (displacement_rva >= 4 ? displacement_rva - 4 : 0)
                      << " context=";
            const auto begin = raw >= 12 ? raw - 12 : raw;
            const auto context_end = std::min(end, raw + 20);
            for (auto cursor = begin; cursor < context_end; ++cursor) {
                std::cout << std::setw(2) << std::setfill('0') << static_cast<unsigned>(bytes[cursor]);
                if (cursor + 1 != context_end) std::cout << ' ';
            }
            std::cout << std::dec << '\n';
            if (++count >= 64) break;
        }
        std::cout << "target_rva=0x" << std::hex << std::uppercase << target
                  << " xrefs_returned=" << std::dec << count << '\n';
    }
    return targets.empty() ? 7 : 0;
}
