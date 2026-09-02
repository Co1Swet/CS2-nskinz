#include "Runtime/KnifeGraph.hpp"
#include <algorithm>
#include <array>
#include <limits>

namespace nskinzcs2::runtime
{
bool MatchesKnifeSecondaryLayoutEvidence(const std::span<const std::uint8_t> handles,
    const std::span<const std::uint8_t> slots) noexcept
{
    constexpr std::array<std::uint8_t, kKnifeSecondaryHandleAppendSize> expected_handles {
        0xFF,0x46,0x38,0x48,0x8B,0x46,0x40,0x42,0x89,0x1C,0xB8,
        0x4C,0x63,0x76,0x20,0x44,0x3B,0x76,0x30 };
    constexpr std::array<std::uint8_t, kKnifeSecondarySlotAppendSize> expected_slots {
        0xFF,0x46,0x20,0x80,0x7C,0x24,0x20,0x02,0x48,0x8B,0x46,0x28,0x4A,0x89,0x3C,0xF0 };
    return std::ranges::equal(handles, expected_handles) && std::ranges::equal(slots, expected_slots);
}

bool ProbeKnifeSecondarySkeleton(const KnifeModelBinding& binding, const std::uintptr_t hud_controller,
    const std::uintptr_t slot_symbol, const bool layout_verified, KnifeGraphReader& reader,
    KnifeSecondarySkeleton& output) noexcept
{
    output = {};
    if (!layout_verified || !binding.Valid() || !slot_symbol || !hud_controller ||
        hud_controller == binding.body_animation_controller || hud_controller == binding.weapon_graph_controller ||
        hud_controller == binding.graph_instance || hud_controller == binding.pawn.address ||
        hud_controller == binding.world.address || hud_controller == binding.hud_weapon.address ||
        hud_controller == binding.arms.address) { return false; }
    const auto field = [&]<typename T>(std::uintptr_t base, std::size_t offset, T& value)
    {
        return base != 0 && base <= std::numeric_limits<std::uintptr_t>::max() - offset - sizeof(T) &&
            reader.Read(base + offset, &value, sizeof(T));
    };
    const auto sample = [&](KnifeSecondarySkeleton& result)
    {
        const auto arms = binding.body_animation_controller;
        std::int32_t handle_count = 0, slot_capacity = 0, handle_capacity = 0, masters = 0;
        std::uintptr_t hud_owner = 0;
        if (!field(arms, 0x20, result.count) || !field(arms, 0x28, result.slots) ||
            !field(arms, 0x30, slot_capacity) || !field(arms, 0x38, handle_count) ||
            !field(arms, 0x40, result.handles) || !field(arms, 0x48, handle_capacity) ||
            !field(hud_controller, 0x50, masters) ||
            !field(hud_controller, kKnifeBodyGraphOwnerOffset, hud_owner)) { return false; }
        if (result.count <= 0 || result.count > 64 || handle_count != result.count ||
            slot_capacity < result.count || handle_capacity < handle_count ||
            slot_capacity > 32768 || handle_capacity > 32768 || masters != 1 || hud_owner != hud_controller)
        { return false; }
        int matches = 0, hud_matches = 0;
        for (std::int32_t i = 0; i < result.count; ++i)
        {
            std::uintptr_t symbol = 0;
            std::uint32_t handle = 0xFFFFFFFFu;
            if (!field(result.slots, static_cast<std::size_t>(i) * 8, symbol) ||
                !field(result.handles, static_cast<std::size_t>(i) * 4, handle)) { return false; }
            if (handle == binding.hud_weapon.handle) { ++hud_matches; }
            if (symbol == slot_symbol)
            {
                ++matches;
                result.index = i; result.slot_symbol = symbol; result.hud_handle = handle;
            }
        }
        return matches == 1 && hud_matches == 1 && result.hud_handle == binding.hud_weapon.handle;
    };
    KnifeSecondarySkeleton first, second;
    if (!sample(first) || !sample(second) || first != second) { return false; }
    output = first;
    return true;
}

bool MatchesKnifeBodyGraphLayoutEvidence(const std::span<const std::uint8_t> construction,
    const std::span<const std::uint8_t> use) noexcept
{
    // CBaseAnimGraphController constructor saves self at +448 and null at +450.
    constexpr std::array<std::uint8_t, kKnifeBodyGraphConstructionSize> expected_construction {
        0x48,0x89,0x9B,0x48,0x04,0,0,0x48,0x89,0xAB,0x50,0x04,0,0 };
    // Native controller getter, then graph at +450 used by the AG2 update path.
    constexpr std::array<std::uint8_t, kKnifeBodyGraphUseSize> expected_use {
        0xE8,0x23,0x53,0x59,0,0x48,0x8B,0x98,0x50,0x04,0,0,0x48,0x85,0xDB };
    return std::ranges::equal(construction, expected_construction) && std::ranges::equal(use, expected_use);
}

bool ReadKnifeBodyGraph(const std::uintptr_t controller, const bool native_layout_verified,
    KnifeGraphReader& reader, std::uintptr_t& graph) noexcept
{
    graph = 0;
    if (!native_layout_verified || controller == 0 ||
        controller > std::numeric_limits<std::uintptr_t>::max() - kKnifeBodyGraphPointerOffset - sizeof(std::uintptr_t))
    { return false; }
    std::array<std::uintptr_t, 2> first {}, second {};
    if (!reader.Read(controller + kKnifeBodyGraphOwnerOffset, first.data(), sizeof(first)) ||
        !reader.Read(controller + kKnifeBodyGraphOwnerOffset, second.data(), sizeof(second)) || first != second ||
        first[0] != controller || first[1] == 0 || first[1] == controller)
    { return false; }
    graph = first[1];
    return true;
}

// Constructor LEA ends at +39; the checked signed displacement is 0x2240F1.
static_assert(kKnifeIdNodeConstructionRva + 39 + 0x2240F1 == kKnifeIdNodeVtableRva);
bool MatchesKnifeGraphLayoutEvidence(const std::span<const std::uint8_t> update,
    const std::span<const std::uint8_t> destruction, const std::span<const std::uint8_t> construction) noexcept
{
    // Native weapon VData AnimClass/Name -> ParamRef graph/index -> ID value.
    constexpr std::array<std::uint8_t, kKnifeWeaponTypeUpdateSize> expected_update {
        0x4D,0x8B,0xAF,0x88,0x03,0,0,0x49,0x8B,0x95,0x70,0x08,0,0,
        0x48,0x85,0xD2,0x75,0x07,0x49,0x8B,0x95,0x20,0x07,0,0,
        0x48,0x8B,0x86,0xF0,0,0,0,0x48,0x85,0xC0,0x74,0x19,
        0x48,0x0F,0xBF,0x8E,0xF8,0,0,0,0x66,0x85,0xC9,0x78,0x0C,
        0x48,0x8B,0x40,0x10,0x48,0x8B,0x0C,0xC8,0x48,0x89,0x51,0x18 };
    // CNmGraphInstance destructor: exactly count pointer-sized nodes, then count=0.
    constexpr std::array<std::uint8_t, kKnifeGraphNodeDestructionSize> expected_destruction {
        0x48,0x8B,0x7B,0x10,0x48,0x63,0x43,0x08,0x48,0x8D,0x34,0xC7,
        0x48,0x3B,0xFE,0x74,0x1B,0x0F,0x1F,0x84,0,0,0,0,0,
        0x48,0x8B,0x0F,0x33,0xD2,0x48,0x8B,0x01,0xFF,0x10,
        0x48,0x83,0xC7,0x08,0x48,0x3B,0xFE,0x75,0xED,0x44,0x89,0x73,0x08 };
    // ID node construction: definition's int16 index +8, definition pointer +8,
    // native symbol value +0x18. Includes the verified local vtable LEA.
    constexpr std::array<std::uint8_t, kKnifeIdNodeConstructionSize> expected_construction {
        0x48,0x8B,0x42,0x08,0x4C,0x0F,0xBF,0x49,0x08,
        0x48,0x8B,0x50,0x08,0x4A,0x8B,0x04,0xCA,0x45,0x85,0xC0,0x75,0x20,
        0x45,0x33,0xC0,0xC7,0x40,0x10,0xFF,0xFF,0xFF,0xFF,
        0x48,0x8D,0x15,0xF1,0x40,0x22,0,0x4C,0x89,0x40,0x18,
        0x48,0x89,0x48,0x08,0x44,0x89,0x40,0x14,0x48,0x89,0x10,0xC3 };
    return std::ranges::equal(update, expected_update) &&
        std::ranges::equal(destruction, expected_destruction) &&
        std::ranges::equal(construction, expected_construction);
}

KnifeGraphProbeResult ProbeKnifeWeaponType(const KnifeModelBinding& binding,
    const KnifeGraphSymbols& symbols, const bool native_layout_verified, KnifeGraphReader& reader) noexcept
{
    using Status = KnifeGraphProbeStatus;
    const auto fail = [](Status status) { return KnifeGraphProbeResult {status, {}}; };
    if (!native_layout_verified) { return fail(Status::LayoutUnverified); }
    if (!binding.Valid()) { return fail(Status::BindingInvalid); }
    if (symbols.weapon_type_name == 0 || symbols.source_type == 0) { return fail(Status::SymbolsMissing); }
    const auto field = [&]<typename T>(std::uintptr_t base, std::size_t offset, T& output)
    {
        constexpr auto maximum = std::numeric_limits<std::uintptr_t>::max();
        return base != 0 && base <= maximum - offset && base + offset <= maximum - sizeof(T) &&
            reader.Read(base + offset, &output, sizeof(T));
    };
    const auto sample = [&](KnifeGraphParameter& result) -> Status
    {
        std::uintptr_t name = 0, body_graph = 0, body_owner = 0;
        if (!field(binding.weapon_graph_controller, 0xE8, name) ||
            !field(binding.weapon_graph_controller, 0xF0, result.graph) ||
            !field(binding.weapon_graph_controller, 0xF8, result.node_index) ||
            !field(binding.body_animation_controller, kKnifeBodyGraphOwnerOffset, body_owner) ||
            !field(binding.body_animation_controller, kKnifeBodyGraphPointerOffset, body_graph)) { return Status::Unreadable; }
        if (name != symbols.weapon_type_name) { return Status::ParameterNameMismatch; }
        if (body_owner != binding.body_animation_controller || result.graph != binding.graph_instance ||
            body_graph != result.graph) { return Status::GraphMismatch; }
        if (!field(result.graph, 8, result.node_count) ||
            !field(result.graph, 0x10, result.table)) { return Status::Unreadable; }
        // Native indices are signed 16-bit. No full-table scan or guessed size.
        if (result.node_count <= 0 || result.node_count > 32768 || result.node_index < 0 ||
            result.node_index >= result.node_count) { return Status::InvalidNodeRange; }
        if (!field(result.table, static_cast<std::size_t>(result.node_index) * sizeof(std::uintptr_t), result.node) ||
            result.node == 0) { return Status::Unreadable; }
        if (!reader.IsIdParameterNode(result.node)) { return Status::WrongNodeType; }
        std::int16_t definition_index = -1;
        if (!field(result.node, 8, result.definition) || !field(result.definition, 8, definition_index) ||
            !field(result.node, 0x18, result.value)) { return Status::Unreadable; }
        if (definition_index != result.node_index) { return Status::WrongNodeDefinition; }
        if (result.value != symbols.source_type) { return Status::SourceTypeMismatch; }
        return Status::Ready;
    };
    KnifeGraphParameter first, second;
    const auto status = sample(first);
    if (status != Status::Ready) { return fail(status); }
    // Never retain a cached node address across graph rebuilds. This catches
    // observable drift during a probe; it does not create a thread-safe write point.
    if (sample(second) != Status::Ready || first != second) { return fail(Status::ChangedDuringRead); }
    return {symbols.target_type != 0 ? Status::Ready : Status::SourceVerifiedTargetMissing, first};
}

const wchar_t* KnifeGraphProbeStatusText(const KnifeGraphProbeStatus status) noexcept
{
    switch (status)
    {
    case KnifeGraphProbeStatus::LayoutUnverified: return L"native layout unverified / 原生布局未验证";
    case KnifeGraphProbeStatus::BindingInvalid: return L"entity binding incomplete / 实体绑定不完整";
    case KnifeGraphProbeStatus::SymbolsMissing: return L"source/name native symbol missing / 源刀型或参数符号缺失";
    case KnifeGraphProbeStatus::Unreadable: return L"parameter memory unavailable / 参数内存不可读";
    case KnifeGraphProbeStatus::ParameterNameMismatch: return L"not weapon_type / 参数名不匹配";
    case KnifeGraphProbeStatus::GraphMismatch: return L"parameter/body graph mismatch / 参数与身体控制器不属同一图";
    case KnifeGraphProbeStatus::InvalidNodeRange: return L"node index outside verified count / 节点索引越界";
    case KnifeGraphProbeStatus::WrongNodeType: return L"not native ID parameter node / 非原生 ID 参数节点";
    case KnifeGraphProbeStatus::WrongNodeDefinition: return L"node definition index mismatch / 节点定义索引不一致";
    case KnifeGraphProbeStatus::SourceTypeMismatch: return L"source knife symbol mismatch / 源刀型符号不匹配";
    case KnifeGraphProbeStatus::ChangedDuringRead: return L"graph changed during read / 读取时图发生变化";
    case KnifeGraphProbeStatus::SourceVerifiedTargetMissing: return L"source verified; target symbol not loaded / 源刀型已验证，目标符号未加载";
    case KnifeGraphProbeStatus::Ready: return L"weapon_type source verified (read-only) / 源刀型参数只读验证通过";
    default: return L"unknown knife graph state / 未知刀模图状态";
    }
}
}
