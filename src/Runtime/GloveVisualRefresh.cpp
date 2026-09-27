#include "Runtime/GloveVisualRefresh.hpp"

namespace nskinzcs2::runtime
{
const std::array<GloveVisualEvidenceWindow, 7>& GloveVisualEvidenceWindows() noexcept
{
    static constexpr std::uint8_t entry[] {0x40,0x55,0x56,0x57,0x48,0x8D,0xAC,0x24,0x30,0xFD,0xFF,0xFF,
        0x48,0x81,0xEC,0xD0,0x03,0,0,0x48,0x8B,0xB9,0xB0,0,0,0,0x48,0x8B,0xF1};
    static constexpr std::uint8_t consumer[] {0x44,0x88,0xAF,0x6D,0x17,0,0,0x48,0x8D,0x95,0xF0,0x02,0,0};
    static constexpr std::uint8_t econ[] {0x4C,0x8D,0xB7,0x70,0x17,0,0};
    // mov r8,[client+0x236A460]; the following identity lookup masks the full
    // output handle and indexes 0x70-byte entries in 512-entry chunks.
    static constexpr std::uint8_t output_lookup[] {
        0x4C,0x8B,0x05,0xEE,0x49,0x70,0x01,0x4D,0x85,0xC0,0x74,0x75,0x83,0xFA,0xFE,0x74,0x2D};
    // Original call passes one argument: &pawn[0x15F8].
    static constexpr std::uint8_t caller[] {0x48,0x8D,0x8B,0xF8,0x15,0,0,0xE8,0xC0,0xD0,0xFF,0xFF};
    static constexpr std::uint8_t hud[] {0x41,0x8B,0x8E,0xF8,0x15,0,0};
    // void HUDConsumer::UpdateGloves(): resolve the observed Pawn handle from
    // this+0x520 before reading the native glove output at Pawn+0x15F8.
    static constexpr std::uint8_t hud_entry[] {
        0x48,0x8B,0xC4,0x41,0x55,0x48,0x81,0xEC,0,1,0,0,
        0x8B,0x91,0x20,0x05,0,0,0x4C,0x8B,0xE9,0x83,0xFA,0xFF};
    static const std::array<GloveVisualEvidenceWindow, 7> windows {{
        {kGloveVisualRefreshRva, entry}, {0xC65A6B, output_lookup},
        {0xC65AEC, consumer}, {0xC65B0A, econ},
        {0xC68904, caller}, {kGloveHudConsumerRva, hud_entry},
        {0x10CCE08, hud}}};
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
    // client+0xC13520 performs an exact 32-bit comparison before it accepts
    // this identity. A 0x7fff/0xffff slot marker is a freed output, not a
    // compatible client-only representation.
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
