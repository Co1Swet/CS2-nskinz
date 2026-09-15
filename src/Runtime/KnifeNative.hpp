#pragma once

#include "Runtime/KnifePair.hpp"
#include "Runtime/KnifeResources.hpp"

namespace nskinzcs2::runtime
{
inline constexpr std::uintptr_t kKnifeSetModelRva = 0x939940;
inline constexpr std::uintptr_t kKnifeRecollectRva = 0x908D60;
inline constexpr std::uintptr_t kKnifeWorldModelNotifyRva = 0x7FB710;
const std::array<KnifeResourceEvidenceWindow, 7>& KnifeNativeEvidenceWindows() noexcept;
bool VerifyKnifeNativeEvidence(std::uintptr_t client, std::size_t size, KnifeGraphReader&) noexcept;

struct KnifeNativeContext
{
    KnifeModelBinding binding;
    KnifeResourceFields resources;
    // Schema CModelState::m_ModelName, not the item's definition or display name.
    std::array<std::uintptr_t, 3> model_names {};
    std::uintptr_t hud_body_controller = 0, slot_symbol = 0;
    KnifeGraphSymbols symbols;
    int source_definition = 0, target_definition = 0;
    // The holstered pre-arm path already changed the exact owned world item
    // definition/model to target before native equip. The active lifecycle
    // must adopt that write and switch the HUD skeleton + weapon_type; treating
    // the live target definition as a native no-op leaves source animations on
    // target geometry.
    bool world_prearmed = false;
    std::uintptr_t client = 0;
    std::size_t client_size = 0;
};

enum class KnifeBladePresence { Live, Gone, Replaced, Unreadable };
// A live entity is not necessarily still owned by the watched player. A
// dropped/picked knife can retain the same address, handle and creation tick
// while its owner changes. Retirement must discard the old ledger in that
// case instead of restoring the previous player's definition/model into the
// new owner's weapon.
enum class KnifeWorldOwnership { Owned, Transferred, Unreadable };
enum class KnifeRetireResult { Restored, Discarded, Waiting, Failed };
enum class KnifeRetirementGraph { Absent, Live, Unreadable };

// The lifecycle owner must validate serials/epoch, scene and controller ownership
// for this exact context. A successful read or a timing counter is NOT a lease.
// Default primitive implementations use the Windows engine ABI. Tests replace
// them with memory fixtures; neither the port nor its tests install a hook.
class KnifeNativeHost : public KnifeGraphReader
{
public:
    virtual bool BindingAlive(const KnifeNativeContext&) noexcept = 0;
    // Exact world/pawn/current-weapon ownership, independent of the HUD that
    // native world SetModel intentionally destroys. Read-only, no fallback scan.
    virtual bool WorldBindingAlive(const KnifeNativeContext&) noexcept { return false; }
    virtual bool AtSafePoint(const KnifeModelBinding&) noexcept = 0;
    virtual bool AttachmentsValid(const KnifeModelBinding&) noexcept = 0;
    // Retirement is a separate lifecycle lease. Default closed for read-only
    // hosts. Do not infer "gone" from an unreadable pointer or a missing graph.
    virtual std::uint64_t SceneEpoch() noexcept { return 0; }
    virtual bool AtRetirementPoint() noexcept { return false; }
    virtual KnifeBladePresence BladePresence(const KnifeNativeContext&, KnifeBladeRole) noexcept
    { return KnifeBladePresence::Unreadable; }
    // Unlike WorldBindingAlive, this deliberately does not require the knife
    // to be active. It proves the same pawn still owns a holstered world knife.
    virtual KnifeWorldOwnership WorldOwnership(const KnifeNativeContext&) noexcept
    { return KnifeWorldOwnership::Unreadable; }
    virtual KnifeRetirementGraph RetirementGraph(const KnifeNativeContext&, KnifeModelBinding&) noexcept
    { return KnifeRetirementGraph::Unreadable; }
    virtual bool ReadWorldItemDefinition(const KnifeNativeContext&, int&) noexcept { return false; }
    virtual bool CompareExchangeWorldItemDefinition(const KnifeNativeContext&, int, int) noexcept { return false; }
    virtual bool InvokeSetModel(std::uintptr_t entry, std::uintptr_t entity, const char* path) noexcept;
    virtual bool InvokeRecollect(std::uintptr_t entry, std::uintptr_t controller) noexcept;
    virtual bool CompareExchangeSymbol(std::uintptr_t address, std::uintptr_t expected, std::uintptr_t desired) noexcept;
};

// Production implementation of the paired port. Only canonical source/target
// knife models, the world item's source/target definition and the existing
// source/target weapon_type symbols are writable. Paint fields, materials,
// shared VData, arms model and arms skeleton remain outside this port.
class KnifeNativePairAccess final : public KnifePairAccess
{
public:
    KnifeNativePairAccess(const KnifeNativeContext& context, KnifeNativeHost& host) noexcept
        : context_(context), host_(host) {}
    bool EvidenceVerified() noexcept;
    bool RebindPreparedHud(const KnifeNativeContext&) noexcept;
    bool Alive(const KnifeModelBinding&) noexcept override;
    bool SafePoint(const KnifeModelBinding&) noexcept override;
    bool Read(const KnifeModelBinding&, KnifePairState&) noexcept override;
    std::uintptr_t ExistingAnimationType(const KnifeModelAsset&) noexcept override;
    bool ReadItemDefinition(const KnifeModelBinding&, int&) noexcept override;
    bool CompareItemDefinition(const KnifeModelBinding&, int expected, int desired) noexcept override;
    bool AdoptPreparedWorld(const KnifeModelBinding&, const KnifeModelAsset& source,
        const KnifeModelAsset& target, const KnifePairState& current) noexcept override;
    bool SetBladeModel(const KnifeModelBinding&, KnifeBladeRole, const KnifeModelAsset&) noexcept override;
    bool Recollect(const KnifeModelBinding&) noexcept override;
    bool CompareAnimationType(const KnifeModelBinding&, std::uintptr_t expected, std::uintptr_t desired) noexcept override;
    // Restores only blades actually touched by this port, even after the other
    // blade/graph is destroyed. One attempt per role; a failed cleanup is latched.
    KnifeRetireResult RetireDetached() noexcept;
    bool Maintain(const KnifeModelBinding&, const KnifePairState&, std::uintptr_t source, std::uintptr_t target) noexcept override;

private:
    bool ContextValid() const noexcept;
    bool ModelNames(std::array<int, 3>&) noexcept;
    bool ModelName(std::size_t role, int& definition) noexcept;
    bool Blade(std::size_t role, int& definition, std::uintptr_t& model) noexcept;
    KnifeGraphProbeResult Parameter(std::uintptr_t expected) noexcept;
    KnifeNativeContext context_;
    KnifeNativeHost& host_;
    std::array<std::uintptr_t, 2> owned_models_ {};
    std::array<bool, 2> touched_ {}, retire_attempted_ {};
    bool item_definition_touched_ = false, item_definition_retire_attempted_ = false;
    bool retire_failed_ = false, retire_models_changed_ = false, retire_discarded_ = false;
};
}
