#pragma once

#include "Runtime/KnifeModel.hpp"
#include <cstddef>
#include <span>

namespace nskinzcs2::runtime
{
// Evidence for the installed 2000932 binary, not callable function addresses.
// Production must match these instruction windows AND the named Schema fields.
inline constexpr std::uintptr_t kKnifeWeaponTypeUpdateRva = 0x9122DA;
inline constexpr std::uintptr_t kKnifeGraphNodeDestructionRva = 0x390B69;
inline constexpr std::uintptr_t kKnifeIdNodeConstructionRva = 0x4613D0;
inline constexpr std::uintptr_t kKnifeIdNodeVtableRva = 0x6A4298;
inline constexpr std::size_t kKnifeWeaponTypeUpdateSize = 63;
inline constexpr std::size_t kKnifeGraphNodeDestructionSize = 48;
inline constexpr std::size_t kKnifeIdNodeConstructionSize = 55;
bool MatchesKnifeGraphLayoutEvidence(std::span<const std::uint8_t> weapon_update,
    std::span<const std::uint8_t> node_destruction, std::span<const std::uint8_t> id_construction) noexcept;
// Schema m_pGraphInstanceAG2 names an embedded wrapper, NOT a raw pointer.
// Its +0 owner is the body controller; +8 is the actual graph instance.
inline constexpr std::size_t kKnifeBodyGraphOwnerOffset = 0x448;
inline constexpr std::size_t kKnifeBodyGraphPointerOffset = 0x450;
inline constexpr std::uintptr_t kKnifeBodyGraphConstructionRva = 0x9272D0;
inline constexpr std::size_t kKnifeBodyGraphConstructionSize = 14;
inline constexpr std::uintptr_t kKnifeBodyGraphUseRva = 0x38A098;
inline constexpr std::size_t kKnifeBodyGraphUseSize = 15;
bool MatchesKnifeBodyGraphLayoutEvidence(std::span<const std::uint8_t> construction,
    std::span<const std::uint8_t> use) noexcept;
inline constexpr std::uintptr_t kKnifeSecondaryHandleAppendRva = 0x96563C;
inline constexpr std::size_t kKnifeSecondaryHandleAppendSize = 19;
inline constexpr std::uintptr_t kKnifeSecondarySlotAppendRva = 0x9656FA;
inline constexpr std::size_t kKnifeSecondarySlotAppendSize = 16;
bool MatchesKnifeSecondaryLayoutEvidence(std::span<const std::uint8_t> handle_append,
    std::span<const std::uint8_t> slot_append) noexcept;
inline constexpr std::size_t kKnifeWeaponTypeNameOffset = 0x120;
inline constexpr std::size_t kKnifeWeaponTypeGraphOffset = 0x128;
inline constexpr std::size_t kKnifeWeaponTypeIndexOffset = 0x130;

enum class KnifeGraphProbeStatus
{
    LayoutUnverified, BindingInvalid, SymbolsMissing, Unreadable,
    ParameterNameMismatch, GraphMismatch, InvalidNodeRange,
    WrongNodeType, WrongNodeDefinition, SourceTypeMismatch,
    ChangedDuringRead, SourceVerifiedTargetMissing, Ready,
};

struct KnifeGraphSymbols
{
    // All values come from the game's existing global-symbol table, never a
    // caller-owned C string or a symbol created in a different process.
    std::uintptr_t weapon_type_name = 0;
    std::uintptr_t source_type = 0;
    std::uintptr_t target_type = 0;
};

struct KnifeGraphParameter
{
    std::uintptr_t graph = 0;
    std::uintptr_t table = 0;
    std::uintptr_t node = 0;
    std::uintptr_t definition = 0;
    std::uintptr_t value = 0;
    std::int32_t node_count = 0;
    std::int16_t node_index = -1;
    bool operator==(const KnifeGraphParameter&) const = default;
};

struct KnifeGraphProbeResult
{
    KnifeGraphProbeStatus status = KnifeGraphProbeStatus::BindingInvalid;
    KnifeGraphParameter parameter;
    // Diagnostic-only scalar retained on a type mismatch. The writable node
    // and all other pointer-bearing fields remain cleared on rejected probes.
    std::uintptr_t observed_value = 0;
    bool SourceVerified() const noexcept
    {
        return status == KnifeGraphProbeStatus::Ready ||
            status == KnifeGraphProbeStatus::SourceVerifiedTargetMissing;
    }
};

// No Write/Call/Allocate interface: this module cannot alter models, animation,
// cosmetics or shared VData. Every probe reads the live graph anew.
class KnifeGraphReader
{
public:
    virtual ~KnifeGraphReader() = default;
    virtual bool Read(std::uintptr_t address, void* output, std::size_t size) noexcept = 0;
    virtual bool IsIdParameterNode(std::uintptr_t node) noexcept = 0;
};

// Revalidate wrapper ownership and pointer twice; never expose a stale graph.
bool ReadKnifeBodyGraph(std::uintptr_t controller, bool native_layout_verified,
    KnifeGraphReader&, std::uintptr_t& graph) noexcept;

struct KnifeSecondarySkeleton
{
    std::uintptr_t slots = 0, handles = 0, slot_symbol = 0;
    std::int32_t count = 0, index = -1;
    std::uint32_t hud_handle = 0xFFFFFFFFu;
    bool operator==(const KnifeSecondarySkeleton&) const = default;
};
// The active HUD blade is registered on the ARMS controller, not a second
// independent weapon animation graph. Symbols stride 8; entity handles stride 4.
// Caller must validate the HUD body-controller/entity correspondence as well.
bool ProbeKnifeSecondarySkeleton(const KnifeModelBinding&, std::uintptr_t hud_body_controller,
    std::uintptr_t native_slot_symbol, bool native_layout_verified, KnifeGraphReader&,
    KnifeSecondarySkeleton&) noexcept;

KnifeGraphProbeResult ProbeKnifeWeaponType(const KnifeModelBinding&, const KnifeGraphSymbols&,
    bool native_layout_verified, KnifeGraphReader&) noexcept;
const wchar_t* KnifeGraphProbeStatusText(KnifeGraphProbeStatus) noexcept;
}
