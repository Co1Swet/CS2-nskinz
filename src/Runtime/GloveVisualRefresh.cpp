#include "Runtime/GloveVisualRefresh.hpp"

namespace nskinzcs2::runtime
{
const std::array<GloveVisualEvidenceWindow, 7>& GloveVisualEvidenceWindows() noexcept
{
    static constexpr std::uint8_t entry[] {0x40,0x55,0x41,0x56,0x41,0x57,0x48,0x8D,0xAC,0x24,0,0xFD,0xFF,0xFF,
        0x48,0x81,0xEC,0,0x04,0,0,0x4C,0x8B,0xB1,0xB0,0,0,0,0x4C,0x8B,0xF9};
    static constexpr std::uint8_t consumer[] {0x41,0xC6,0x86,0x6D,0x17,0,0,0x01,0x49,0x8B,0xCE,
        0xE8,0x1D,0x4D,0xB2,0xFF};
    static constexpr std::uint8_t econ[] {0x49,0x8D,0xBE,0x70,0x17,0,0,0x48,0x8B,0x08,
        0x49,0x89,0x8F,0xA8,0,0,0};
    // mov r8,[client+0x236FA70]; the following identity lookup masks the full
    // output handle and indexes 0x70-byte entries in 512-entry chunks.
    static constexpr std::uint8_t output_lookup[] {
        0x4C,0x8B,0x05,0xEC,0xB7,0x70,0x01,0x4D,0x85,0xC0,0x74,0x62,0x83,0xFA,0xFE,0x74,0x2D};
    // Original call passes one argument: &pawn[0x15F8].
    static constexpr std::uint8_t caller[] {0x48,0x8D,0x8B,0xF8,0x15,0,0,0xE8,0xF0,0xCD,0xFF,0xFF};
    static constexpr std::uint8_t hud[] {0x41,0x8B,0x8E,0xF8,0x15,0,0};
    // void HUDConsumer::UpdateGloves(): resolve the observed Pawn handle from
    // this+0x520 before reading the native glove output at Pawn+0x15F8.
    static constexpr std::uint8_t hud_entry[] {
        0x48,0x8B,0xC4,0x41,0x55,0x48,0x81,0xEC,0,1,0,0,
        0x8B,0x91,0x20,0x05,0,0,0x4C,0x8B,0xE9,0x83,0xFA,0xFF};
    static const std::array<GloveVisualEvidenceWindow, 7> windows {{
        {kGloveVisualRefreshRva, entry}, {0xC6627D, output_lookup},
        {0xC662E3, consumer}, {0xC663CA, econ},
        {0xC69414, caller}, {kGloveHudConsumerRva, hud_entry},
        {0x10D5BB8, hud}}};
    return windows;
}
bool GloveVisualIdentityMatchesOutputHandle(
    const std::uint32_t output_handle,
    const std::uint32_t identity_handle) noexcept
{
    constexpr std::uint32_t index_mask = 0x7FFFu;
    constexpr std::uint32_t invalid_handle = 0xFFFFFFFFu;
    if (output_handle == 0 || output_handle == invalid_handle ||
        output_handle == 0xFFFFFFFEu || identity_handle == 0 ||
        identity_handle == invalid_handle || identity_handle == 0xFFFFFFFEu)
        return false;

    const auto output_index = output_handle & index_mask;
    if (output_index == 0 || output_index == index_mask) return false;
    // The native client-only lookup performs an exact 32-bit comparison before
    // it accepts this identity. A 0x7fff/0xffff slot marker is a freed output,
    // not a compatible client-only representation.
    return identity_handle == output_handle;
}
bool GloveVisualRefreshLatch::Begin(const GloveVisualRefreshKey& key, std::uint64_t now) noexcept
{
    if (!key.scene || !key.xuid || !key.pawn || !key.signature ||
        key.handle == 0xFFFFFFFFu || key.handle == 0xFFFFFFFEu || !key.handle || Matches(key)) return false;
    key_ = key; started_ = now; active_ = true; failed_ = complete_ = false;
    return true;
}
}
