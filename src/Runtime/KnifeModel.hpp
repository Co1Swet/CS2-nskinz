#pragma once

#include "Core/AppState.hpp"

#include <array>
#include <cstdint>
#include <string_view>

namespace nskinzcs2::runtime
{
// Deliberately contains no paint, wear, seed, quality, name or glove state.
struct KnifeModelRequest
{
    std::uint64_t xuid = 0;
    int definition = 0;
    bool operator==(const KnifeModelRequest&) const = default;
    bool Enabled() const noexcept { return xuid != 0 && definition != 0; }
};

struct KnifeModelAsset
{
    int definition;
    std::string_view animation_class;
    std::string_view model;
    std::string_view skeleton;
};

// Data baseline: CS2 2000899 / 1.41.7.8, scripts/weapons.vdata_c.
// Excludes unreleased definition 524 (its VData aliases the default T model).
const std::array<KnifeModelAsset, 22>& KnifeModelAssets() noexcept;
const KnifeModelAsset* FindKnifeModelAsset(int definition) noexcept;
KnifeModelRequest BuildKnifeModelRequest(const model::KnifeProfile& profile) noexcept;

struct KnifeModelEntityIdentity
{
    std::uintptr_t address = 0;
    std::uint32_t handle = 0xFFFFFFFFu; // Include serial, never only the slot index.
    int creation_tick = -1;
    bool operator==(const KnifeModelEntityIdentity&) const = default;
    bool Valid() const noexcept;
};

struct KnifeModelBinding
{
    // A scene/seek epoch, NOT the material refresh/config revision counter.
    std::uint64_t scene_epoch = 0;
    std::uint64_t xuid = 0;
    KnifeModelEntityIdentity pawn;
    KnifeModelEntityIdentity world;
    KnifeModelEntityIdentity hud_weapon;
    KnifeModelEntityIdentity arms;
    std::uintptr_t weapon_graph_controller = 0;
    std::uintptr_t body_animation_controller = 0;
    std::uintptr_t graph_instance = 0;
    bool operator==(const KnifeModelBinding&) const = default;
    bool Valid() const noexcept;
    bool SameEntities(const KnifeModelBinding&) const noexcept;
};

struct KnifeModelObservation
{
    KnifeModelBinding binding;
    int source_definition = 0;
    bool offline_demo = false;
    bool seeking = false;
    bool authoritative_active_knife = false;
    bool ownership_verified = false;
    bool native_pair_adapter_verified = false;
    bool safe_point = false;
    std::uint64_t now_millis = 0;
};

enum class KnifeModelPhase
{
    Disabled,
    WaitingBinding,
    WaitingNativeAdapter,
    WaitingSafePoint,
    NativeModel,
    Applied,
    Faulted,
};

// The native implementation owns resource references and the restore snapshot.
// Every mutating method must revalidate ALL binding identities on the game thread.
// Capture/Restore may not snapshot or overwrite paint attributes/material/glove
// data. If a native adapter needs the knife item definition, that identity field
// belongs to this transaction; do not restore an entire CEconItemView over a finish.
// ApplyPair must update world/HUD knife + matching native AG2 skeleton/parameters
// together; it must never SetModel(arms, knife), replace the arms' viewmodel skeleton
// with a weapon skeleton, reset sequence 0, or edit shared VData. Prefer selecting
// the existing native graph branch over recreating the entire arms graph.
class KnifeModelAdapter
{
public:
    virtual ~KnifeModelAdapter() = default;
    virtual bool BindingAlive(const KnifeModelBinding&) noexcept = 0;
    virtual bool Capture(const KnifeModelBinding&) noexcept = 0;
    // Always return the post-attempt binding (also after partial failure). A
    // successful graph rebuild can legitimately replace the graph instance.
    virtual bool ApplyPair(const KnifeModelBinding&, const KnifeModelAsset&, KnifeModelBinding& after) noexcept = 0;
    // Verify includes both models, graph/skeleton, animation class and attachments.
    virtual bool VerifyPair(const KnifeModelBinding&, const KnifeModelAsset&) noexcept = 0;
    virtual bool Restore(const KnifeModelBinding&) noexcept = 0;
    virtual void Abandon() noexcept = 0;
};

struct KnifeModelStatistics
{
    std::uint64_t apply_attempts = 0;
    std::uint64_t successful_applies = 0;
    std::uint64_t restores = 0;
    std::uint64_t abandoned = 0;
    std::uint64_t faults = 0;
};

class KnifeModelController
{
public:
    KnifeModelPhase Tick(const KnifeModelRequest&, const KnifeModelObservation&, KnifeModelAdapter&) noexcept;
    KnifeModelPhase Phase() const noexcept { return phase_; }
    const KnifeModelStatistics& Statistics() const noexcept { return statistics_; }

private:
    bool Retire(const KnifeModelObservation&, KnifeModelAdapter&) noexcept;
    KnifeModelPhase Fault() noexcept;
    KnifeModelRequest request_;
    KnifeModelBinding binding_;
    KnifeModelPhase phase_ = KnifeModelPhase::Disabled;
    KnifeModelStatistics statistics_;
    bool captured_ = false;
    bool restore_failed_ = false;
    std::uint64_t next_verify_millis_ = 0;
};

const wchar_t* KnifeModelPhaseText(KnifeModelPhase phase) noexcept;
}
