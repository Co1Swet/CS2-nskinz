#pragma once

#include "Runtime/KnifeResources.hpp"

namespace nskinzcs2::runtime
{
// AnimAssetData describes the ORIGINAL demo's animation assets. Loading a new
// vmdl/vnmskel does not add a row to that table. Build 2000905 dereferences a
// null cache result when such a skeleton reaches the networked context builder.
// Addresses below are verified against ClientVersion 2000935 (1.41.9.2).
inline constexpr std::uintptr_t kKnifeCachedSerializationRva = 0x35EBC0;
inline constexpr std::uintptr_t kKnifeLocalSerializationRva = 0x12EBE70;
inline constexpr std::uintptr_t kKnifeSerializationCallerRva = 0x92DC86;
inline constexpr std::uintptr_t kKnifeAnimAssetDataPointerRva = 0x23BD818;

const std::array<KnifeResourceEvidenceWindow, 5>& KnifeSerializationEvidenceWindows() noexcept;
bool VerifyKnifeSerializationEvidence(std::uintptr_t client, std::size_t size, KnifeGraphReader&) noexcept;
std::uint32_t KnifeSerializationModelBit(int definition) noexcept;
bool NeedsLocalKnifeSerialization(bool offline_demo, std::uint32_t modified_models,
    std::string_view skeleton, bool recorded_asset_available) noexcept;

using KnifeCachedSerializationFn = void* (__fastcall*)(void*, void*, const void*, const void*, const void*, const void*);
using KnifeLocalSerializationFn = void* (__fastcall*)(void*, const void*, const void*, const void*, const void*);
void* DispatchKnifeSerialization(bool local, KnifeCachedSerializationFn cached, KnifeLocalSerializationFn native,
    void* manager, void* output, const void* graph, const void* child_graphs,
    const void* animation_sets, const void* skeletons);

// Call on the game thread BEFORE changing a knife model/definition. Installs
// a narrowly scoped, process-lifetime bridge, not a ResourceSystem path hook.
// It only handles a known modified knife skeleton absent from AnimAssetData;
// all complete/demo-native contexts retain the original native path.
bool ArmKnifeSerializationBridge(std::uintptr_t client, std::size_t size, void* engine,
    int definition, KnifeGraphReader&) noexcept;
std::uint64_t KnifeLocalSerializationCount() noexcept;
}
