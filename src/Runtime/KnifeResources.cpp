#include "Runtime/KnifeResources.hpp"
#include <algorithm>
#include <limits>

namespace nskinzcs2::runtime
{
namespace
{
constexpr std::uint8_t kEntity[] {0x48,0x8B,0x80,0xE0,0x01,0,0,0x48,0x85,0xC0,0x74,0x03,0x48,0x8B,0x18};
constexpr std::uint8_t kCount[] {0x8B,0x81,0x40,0x04,0,0,0xC3};
constexpr std::uint8_t kReference[] {
    0x4C,0x8B,0xC9,0x49,0x63,0xC0,0x48,0x8D,0x0C,0x80,0x49,0x8B,0x81,0x48,0x04,0,0,
    0x48,0x03,0xC9,0x48,0x8B,0x0C,0xC8,0x48,0x8B,0xC2,0x48,0x89,0x0A,0xC3};
constexpr std::uint8_t kHeader[] {
    0x48,0x8D,0x99,0x38,0x03,0,0,0x48,0x8B,0xF2,0x48,0x8B,0xF9,0x48,0x3B,0xD3,
    0x74,0x3E,0x8B,0x0A,0x8B,0xC1,0x8B,0x13,0x2B,0xC2};
constexpr std::uint8_t kCopy[] {
    0x48,0x63,0x06,0x48,0x8B,0x56,0x08,0x4C,0x8D,0x04,0xC5,0,0,0,0,
    0x4A,0x8D,0x04,0x02,0x48,0x3B,0xC2,0x76,0x09,0x48,0x8B,0x4B,0x08,
    0xE8,0xC1,0xEF,0x76,0};
const std::array<KnifeResourceEvidenceWindow, 5> kWindows {{
    {kKnifeEntityModelDereferenceRva, kEntity}, {kKnifeModelSkeletonCountRva, kCount},
    {kKnifeModelSkeletonReferenceRva, kReference}, {kKnifeGraphSkeletonHeaderRva, kHeader},
    {kKnifeGraphSkeletonCopyRva, kCopy}
}};

template<class T> bool Field(KnifeGraphReader& reader, std::uintptr_t base, std::size_t offset, T& value) noexcept
{
    constexpr auto limit = std::numeric_limits<std::uintptr_t>::max();
    return base >= 0x10000 && offset <= limit - sizeof(T) && base <= limit - offset - sizeof(T) &&
        reader.Read(base + offset, &value, sizeof(T));
}

bool LoadedModel(KnifeGraphReader& reader, std::uintptr_t field, std::uintptr_t& binding, std::uintptr_t& model) noexcept
{
    return Field(reader, field, 0, binding) && binding && Field(reader, binding, 0, model) &&
        model && model != binding && model != field;
}
}

const std::array<KnifeResourceEvidenceWindow, 5>& KnifeResourceEvidenceWindows() noexcept { return kWindows; }

bool VerifyKnifeResourceEvidence(std::uintptr_t client, std::size_t size, KnifeGraphReader& reader) noexcept
{
    constexpr auto limit = std::numeric_limits<std::uintptr_t>::max();
    if (client < 0x10000 || size == 0 || client > limit - size) return false;
    std::array<std::uint8_t, 64> buffer {};
    for (const auto& window : kWindows)
    {
        if (window.rva >= size || window.bytes.size() > size - window.rva ||
            window.bytes.size() > buffer.size() ||
            !reader.Read(client + window.rva, buffer.data(), window.bytes.size()) ||
            !std::equal(window.bytes.begin(), window.bytes.end(), buffer.begin())) return false;
    }
    return true;
}

bool ProbeKnifeHudSkeleton(const KnifeModelBinding& binding, std::uintptr_t field,
    bool verified, KnifeGraphReader& reader) noexcept
{
    if (!verified || !binding.Valid() || !field) return false;
    const auto sample = [&](KnifeResourcePair& state)
    {
        std::uintptr_t graph = 0;
        int count = 0;
        if (!ReadKnifeBodyGraph(binding.body_animation_controller, true, reader, graph) || graph != binding.graph_instance ||
            !LoadedModel(reader, field, state.hud_binding, state.hud_model) ||
            !Field(reader, state.hud_model, kKnifeModelSkeletonCountOffset, count) || count != 1 ||
            !Field(reader, state.hud_model, kKnifeModelSkeletonReferencesOffset, state.hud_references) ||
            !Field(reader, state.hud_references, 0, state.hud_skeleton) || !state.hud_skeleton ||
            !Field(reader, graph, 0x338, state.graph_count) || state.graph_count < 1 || state.graph_count > 64 ||
            !Field(reader, graph, 0x340, state.graph_references)) return false;
        int matches = 0;
        for (int i = 0; i < state.graph_count; ++i)
        {
            auto& reference = state.graph_skeletons[static_cast<std::size_t>(i)];
            if (!Field(reader, state.graph_references, static_cast<std::size_t>(i) * 8, reference) || !reference) return false;
            for (int j = 0; j < i; ++j)
                if (state.graph_skeletons[static_cast<std::size_t>(j)] == reference) return false;
            if (reference == state.hud_skeleton) ++matches;
        }
        return matches == 1;
    };
    KnifeResourcePair first, second;
    std::uintptr_t graph = 0;
    return sample(first) && sample(second) && first == second &&
        ReadKnifeBodyGraph(binding.body_animation_controller, true, reader, graph) && graph == binding.graph_instance;
}

bool ProbeKnifeResourcePair(const KnifeModelBinding& binding, const KnifeResourceFields& fields,
    bool verified, KnifeGraphReader& reader, KnifeResourcePair& output) noexcept
{
    output = {};
    if (!verified || !binding.Valid() || !fields.world || !fields.hud || !fields.arms ||
        fields.world == fields.hud || fields.world == fields.arms || fields.hud == fields.arms) return false;
    const auto sample = [&](KnifeResourcePair& state)
    {
        std::uintptr_t graph = 0;
        if (!ReadKnifeBodyGraph(binding.body_animation_controller, true, reader, graph) || graph != binding.graph_instance ||
            !LoadedModel(reader, fields.world, state.world_binding, state.world_model) ||
            !LoadedModel(reader, fields.hud, state.hud_binding, state.hud_model) ||
            !LoadedModel(reader, fields.arms, state.arms_binding, state.arms_model) ||
            state.arms_binding == state.world_binding || state.arms_binding == state.hud_binding ||
            state.arms_model == state.world_model || state.arms_model == state.hud_model) return false;
        std::int32_t world_count = 0, hud_count = 0;
        if (!Field(reader, state.world_model, kKnifeModelSkeletonCountOffset, world_count) || world_count != 1 ||
            !Field(reader, state.hud_model, kKnifeModelSkeletonCountOffset, hud_count) || hud_count != 1 ||
            !Field(reader, state.world_model, kKnifeModelSkeletonReferencesOffset, state.world_references) ||
            !Field(reader, state.hud_model, kKnifeModelSkeletonReferencesOffset, state.hud_references) ||
            !Field(reader, state.world_references, 0, state.world_skeleton) || !state.world_skeleton ||
            !Field(reader, state.hud_references, 0, state.hud_skeleton) || !state.hud_skeleton ||
            !Field(reader, graph, 0x338, state.graph_count) || state.graph_count < 0 || state.graph_count > 64 ||
            !Field(reader, graph, 0x340, state.graph_references)) return false;
        int matches = 0;
        for (int i = 0; i < state.graph_count; ++i)
        {
            auto& reference = state.graph_skeletons[static_cast<std::size_t>(i)];
            if (!Field(reader, state.graph_references, static_cast<std::size_t>(i) * 8, reference) || !reference) return false;
            for (int j = 0; j < i; ++j)
                if (state.graph_skeletons[static_cast<std::size_t>(j)] == reference) return false;
            if (reference == state.hud_skeleton) ++matches;
        }
        state.blade_skeletons_match = state.world_skeleton == state.hud_skeleton;
        state.hud_skeleton_in_graph = matches == 1;
        return true;
    };
    KnifeResourcePair first, second;
    if (!sample(first) || !sample(second) || first != second) return false;
    std::uintptr_t graph = 0;
    if (!ReadKnifeBodyGraph(binding.body_animation_controller, true, reader, graph) || graph != binding.graph_instance) return false;
    output = first;
    return true;
}
}
