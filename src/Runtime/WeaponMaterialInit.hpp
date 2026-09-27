#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace nskinzcs2::runtime
{
// The native refresh loop only visits nonempty composite-material owners.
// Decode its two callees so a verified standard gun can use the same engine
// operations directly, including the empty -> first finish transition.
struct WeaponMaterialCalls
{
    std::uint32_t primary_offset = 0, companion_offset = 0, owner_offset = 0;
    std::int32_t clear_relative = 0, generate_relative = 0;
};

inline bool DecodeWeaponMaterialCalls(std::span<const std::uint8_t> bytes,
    WeaponMaterialCalls& out) noexcept
{
    out = {};
    constexpr std::array<std::uint8_t, 42> expected {
        0x83,0xBB,0xB0,0x0A,0,0,0,0x7F,0x09,
        0x83,0xBB,0xC8,0x0A,0,0,0,0x7E,0x18,
        0x48,0x8D,0x8B,0x10,0x06,0,0,0xB2,0x01,
        0xE8,0,0,0,0,0x33,0xD2,0x48,0x8B,0xCB,0xE8,0,0,0,0
    };
    if (bytes.size() != expected.size()) return false;
    for (std::size_t i = 0; i < expected.size(); ++i)
    {
        if ((i >= 28 && i < 32) || i >= 38) continue; // rel32, not opcodes/ABI.
        if (bytes[i] != expected[i]) return false;
    }
    out.primary_offset = 0xAB0;
    out.companion_offset = 0xAC8;
    out.owner_offset = 0x610;
    std::memcpy(&out.clear_relative, bytes.data() + 28, 4);
    std::memcpy(&out.generate_relative, bytes.data() + 38, 4);
    return true;
}

inline bool StandardWeaponHasCompositeMaterial(int primary, int companion) noexcept
{
    return primary >= 0 && primary <= 128 && companion >= 0 && companion <= 128 &&
        (primary > 0 || companion > 0);
}
}
