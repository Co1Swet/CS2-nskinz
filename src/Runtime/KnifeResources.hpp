#pragma once

#include "Runtime/KnifeGraph.hpp"
#include <array>

namespace nskinzcs2::runtime
{
inline constexpr std::uintptr_t kKnifeEntityModelDereferenceRva = 0x21CFC1;
inline constexpr std::uintptr_t kKnifeModelSkeletonCountRva = 0x1655A80;
inline constexpr std::uintptr_t kKnifeModelSkeletonReferenceRva = 0x16559E0;
inline constexpr std::uintptr_t kKnifeGraphSkeletonHeaderRva = 0x12628AF;
inline constexpr std::uintptr_t kKnifeGraphSkeletonCopyRva = 0x12628DE;

struct KnifeResourceEvidenceWindow
{
    std::uintptr_t rva;
    std::span<const std::uint8_t> bytes;
};
const std::array<KnifeResourceEvidenceWindow, 5>& KnifeResourceEvidenceWindows() noexcept;
bool VerifyKnifeResourceEvidence(std::uintptr_t client, std::size_t image_size, KnifeGraphReader&) noexcept;

struct KnifeResourceFields
{
    // Addresses of Schema-resolved CModelState::m_hModel fields, NOT raw model
    // pointers supplied by a caller. Re-read the binding and loaded data twice.
    std::uintptr_t world = 0, hud = 0, arms = 0;
};

struct KnifeResourcePair
{
    std::uintptr_t world_binding = 0, hud_binding = 0, arms_binding = 0;
    std::uintptr_t world_model = 0, hud_model = 0, arms_model = 0;
    std::uintptr_t world_references = 0, hud_references = 0;
    std::uintptr_t world_skeleton = 0, hud_skeleton = 0;
    std::uintptr_t graph_references = 0;
    std::int32_t graph_count = 0;
    std::array<std::uintptr_t, 64> graph_skeletons {};
    // These can be false during a paired transaction. A stable snapshot is not
    // proof that recollection is complete or that a mutation time is safe.
    bool blade_skeletons_match = false, hud_skeleton_in_graph = false;
    bool operator==(const KnifeResourcePair&) const = default;
};

// Read-only native resource verification for the paired port. All 22 supported
// knife models have one m_vecNmSkeletonRefs entry; model entries stride 0x50,
// while the graph's deduplicated reference list strides 8. No reference-count,
// resource loading, model-setting, graph or cosmetic writes are exposed here.
bool ProbeKnifeResourcePair(const KnifeModelBinding&, const KnifeResourceFields&,
    bool native_layout_verified, KnifeGraphReader&, KnifeResourcePair&) noexcept;
// Retirement may occur after the world entity disappears. Do not dereference
// its old model field just to verify the surviving HUD's graph recollection.
bool ProbeKnifeHudSkeleton(const KnifeModelBinding&, std::uintptr_t hud_model_field,
    bool native_layout_verified, KnifeGraphReader&) noexcept;
}
