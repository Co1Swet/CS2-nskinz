#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace nskinzcs2::runtime
{
// client.dll PE timestamp 0x6AC7FFFA (2026-10-09): the native pawn glove
// component's output is consumed by HUD. This is NOT m_hMyWearables or a
// fabricated CEconWearable.
inline constexpr std::uintptr_t kGloveVisualRefreshRva = 0xC64990;
// The continuously reached HUD glove assembly consumer. It
// resolves the observed Pawn from this+0x520, then consumes Pawn+0x15F8.
inline constexpr std::uintptr_t kGloveHudConsumerRva = 0x10CE1A0;
inline constexpr std::ptrdiff_t kGloveHudConsumerPawnHandleOffset = 0x520;
// The native glove updater resolves its output handle through this client-only
// identity chunk root, not the networked client entity system.
inline constexpr std::uintptr_t kGloveVisualEntityChunksRva = 0x2366770;
inline constexpr std::ptrdiff_t kGloveVisualComponentOffset = 0x15F8;
inline constexpr std::ptrdiff_t kGloveVisualComponentOwnerOffset = 0xB0;
struct GloveVisualEvidenceWindow
{
    std::uintptr_t rva;
    std::span<const std::uint8_t> bytes;
};
const std::array<GloveVisualEvidenceWindow, 7>& GloveVisualEvidenceWindows() noexcept;
// Mirrors the native client-only identity lookup: a live glove output requires
// an exact full-handle match. Sentinel index values represent a freed slot.
bool GloveVisualIdentityMatchesOutputHandle(
    std::uint32_t output_handle, std::uint32_t identity_handle) noexcept;
struct GloveVisualRefreshKey
{
    std::uint64_t scene = 0, xuid = 0, signature = 0;
    std::uintptr_t pawn = 0;
    std::uint32_t handle = 0xFFFFFFFFu;
    int creation_tick = 0;
    bool operator==(const GloveVisualRefreshKey&) const = default;
};
// Field repairs must not rebuild native model/material state each poll. Keep
// the latch across no-op Update; only entity/request changes start a new call.
class GloveVisualRefreshLatch
{
public:
    bool Begin(const GloveVisualRefreshKey& key, std::uint64_t now) noexcept;
    bool Matches(const GloveVisualRefreshKey& key) const noexcept { return active_ && key_ == key; }
    bool TimedOut(std::uint64_t now) const noexcept { return active_ && now >= started_ && now - started_ > 3000; }
    bool Complete() noexcept { const bool changed = !complete_; complete_ = true; return changed; }
    void Fail() noexcept { failed_ = true; }
    bool Failed() const noexcept { return failed_; }
    bool Completed() const noexcept { return complete_; }
private:
    GloveVisualRefreshKey key_;
    std::uint64_t started_ = 0;
    bool active_ = false, failed_ = false, complete_ = false;
};
}
