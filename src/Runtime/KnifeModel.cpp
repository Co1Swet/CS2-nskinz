#include "Runtime/KnifeModel.hpp"

#include <algorithm>

namespace nskinzcs2::runtime
{
namespace
{
// Keep the VData/subclass name and graph animation class separate. A recorded
// source can initially use m_szName while the target VData selected by
// OnSubclassIDChanged publishes m_szAnimClass into weapon_type.
#define KNIFE_ASSET(id, name, stem, graph) \
    KnifeModelAsset{id, name, "weapons/models/knife/" stem "/weapon_" stem ".vmdl", \
        "animation/skeletons/weapons/" stem ".vnmskel", graph}
constexpr std::array<KnifeModelAsset, 22> kAssets {{
    KNIFE_ASSET(42, "weapon_knife", "knife_default_ct", "knife_default_ct"),
    KNIFE_ASSET(59, "weapon_knife_t", "knife_default_t", "knife_t"),
    KNIFE_ASSET(500, "weapon_bayonet", "knife_bayonet", "bayonet"),
    KNIFE_ASSET(503, "weapon_knife_css", "knife_css", "knife_css"),
    KNIFE_ASSET(505, "weapon_knife_flip", "knife_flip", "knife_flip"),
    KNIFE_ASSET(506, "weapon_knife_gut", "knife_gut", "knife_gut"),
    KNIFE_ASSET(507, "weapon_knife_karambit", "knife_karambit", "knife_karambit"),
    KNIFE_ASSET(508, "weapon_knife_m9_bayonet", "knife_m9", "knife_m9_bayonet"),
    KNIFE_ASSET(509, "weapon_knife_tactical", "knife_tactical", "knife_tactical"),
    KNIFE_ASSET(512, "weapon_knife_falchion", "knife_falchion", "knife_falchion"),
    KNIFE_ASSET(514, "weapon_knife_survival_bowie", "knife_bowie", "knife_survival_bowie"),
    KNIFE_ASSET(515, "weapon_knife_butterfly", "knife_butterfly", "knife_butterfly"),
    KNIFE_ASSET(516, "weapon_knife_push", "knife_push", "knife_push"),
    KNIFE_ASSET(517, "weapon_knife_cord", "knife_cord", "knife_cord"),
    KNIFE_ASSET(518, "weapon_knife_canis", "knife_canis", "knife_canis"),
    KNIFE_ASSET(519, "weapon_knife_ursus", "knife_ursus", "knife_ursus"),
    KNIFE_ASSET(520, "weapon_knife_gypsy_jackknife", "knife_navaja", "knife_gypsy_jackknife"),
    KNIFE_ASSET(521, "weapon_knife_outdoor", "knife_outdoor", "knife_outdoor"),
    KNIFE_ASSET(522, "weapon_knife_stiletto", "knife_stiletto", "knife_stiletto"),
    KNIFE_ASSET(523, "weapon_knife_widowmaker", "knife_talon", "knife_widowmaker"),
    KNIFE_ASSET(525, "weapon_knife_skeleton", "knife_skeleton", "knife_skeleton"),
    KNIFE_ASSET(526, "weapon_knife_kukri", "knife_kukri", "knife_kukri"),
}};
#undef KNIFE_ASSET
}

const std::array<KnifeModelAsset, 22>& KnifeModelAssets() noexcept { return kAssets; }

const KnifeModelAsset* FindKnifeModelAsset(const int definition) noexcept
{
    const auto found = std::find_if(kAssets.begin(), kAssets.end(),
        [definition](const auto& asset) { return asset.definition == definition; });
    return found == kAssets.end() ? nullptr : &*found;
}

const KnifeModelAsset* FindKnifeModelAssetByPath(std::string_view path) noexcept
{
    if (path.ends_with(".vmdl_c")) path.remove_suffix(2);
    const auto found = std::find_if(kAssets.begin(), kAssets.end(),
        [path](const auto& asset) { return asset.model == path; });
    return found == kAssets.end() ? nullptr : &*found;
}

const KnifeModelAsset* FindKnifeModelAssetByAnimationClass(
    const std::string_view animation_class) noexcept
{
    if (animation_class.empty()) return nullptr;
    const auto found = std::find_if(kAssets.begin(), kAssets.end(),
        [animation_class](const auto& asset)
        {
            return asset.animation_class == animation_class ||
                asset.graph_animation_class == animation_class;
        });
    return found == kAssets.end() ? nullptr : &*found;
}

KnifeModelRequest BuildKnifeModelRequest(const model::KnifeProfile& profile) noexcept
{
    if (!profile.enabled || profile.xuid == 0 ||
        profile.mode != model::KnifeProfileMode::ModelAndFinish)
    {
        return {};
    }
    // Preserve invalid targets so the gate reports WaitingBinding, not success.
    return {profile.xuid, profile.model_definition};
}

bool KnifeModelEntityIdentity::Valid() const noexcept
{
    return address != 0 && handle != 0xFFFFFFFFu && handle != 0xFFFFFFFEu &&
        creation_tick != std::numeric_limits<int>::min();
}

bool KnifeModelBinding::Valid() const noexcept
{
    const auto aliases_entity = [this](const std::uintptr_t address)
    {
        return address == world.address || address == hud_weapon.address ||
            address == pawn.address || address == arms.address;
    };
    return scene_epoch != 0 && xuid != 0 && pawn.Valid() && world.Valid() &&
        hud_weapon.Valid() && arms.Valid() &&
        world.address != hud_weapon.address && world.address != arms.address &&
        hud_weapon.address != arms.address && pawn.address != world.address &&
        pawn.address != hud_weapon.address && pawn.address != arms.address &&
        pawn.handle != world.handle && pawn.handle != hud_weapon.handle && pawn.handle != arms.handle &&
        world.handle != hud_weapon.handle && world.handle != arms.handle && hud_weapon.handle != arms.handle &&
        weapon_graph_controller != 0 && body_animation_controller != 0 && graph_instance != 0 &&
        weapon_graph_controller != body_animation_controller && graph_instance != weapon_graph_controller &&
        graph_instance != body_animation_controller && !aliases_entity(weapon_graph_controller) &&
        !aliases_entity(body_animation_controller) && !aliases_entity(graph_instance);
}

bool KnifeModelBinding::SameEntities(const KnifeModelBinding& other) const noexcept
{
    return scene_epoch == other.scene_epoch && xuid == other.xuid && pawn == other.pawn &&
        world == other.world && hud_weapon == other.hud_weapon && arms == other.arms;
}

KnifeModelPhase KnifeModelController::Fault() noexcept
{
    ++statistics_.faults;
    return phase_ = KnifeModelPhase::Faulted;
}

bool KnifeModelController::Retire(const KnifeModelObservation& observation, KnifeModelAdapter& adapter) noexcept
{
    if (!captured_)
    {
        return true;
    }
    // Never restore addresses from before a seek, even if allocator reuse makes
    // them look alive. Resource references are dropped without dereferencing entities.
    if (observation.seeking || binding_.scene_epoch != observation.binding.scene_epoch ||
        !adapter.BindingAlive(binding_))
    {
        adapter.Abandon();
        captured_ = false;
        restore_failed_ = false;
        ++statistics_.abandoned;
        return true;
    }
    if (restore_failed_)
    {
        phase_ = KnifeModelPhase::Faulted;
        return false;
    }
    if (!observation.safe_point)
    {
        phase_ = KnifeModelPhase::WaitingSafePoint;
        return false;
    }
    if (!adapter.Restore(binding_))
    {
        restore_failed_ = true;
        Fault();
        return false;
    }
    captured_ = false;
    ++statistics_.restores;
    return true;
}

KnifeModelPhase KnifeModelController::Tick(
    const KnifeModelRequest& request, const KnifeModelObservation& observation,
    KnifeModelAdapter& adapter) noexcept
{
    const bool enabled = request.Enabled() && observation.offline_demo && !observation.seeking;
    const bool binding_ready = enabled && observation.authoritative_active_knife &&
        observation.ownership_verified && observation.binding.Valid() &&
        request.xuid == observation.binding.xuid && FindKnifeModelAsset(observation.source_definition) != nullptr;
    const bool changed = request_ != request || binding_ != observation.binding;
    if (captured_ && (changed || !binding_ready))
    {
        if (!Retire(observation, adapter))
        {
            return phase_;
        }
        // Retire and apply occur in different ticks: never double-rebuild a graph.
        request_ = request;
        binding_ = observation.binding;
        return phase_ = enabled ? KnifeModelPhase::WaitingBinding : KnifeModelPhase::Disabled;
    }
    if (changed)
    {
        request_ = request;
        binding_ = observation.binding;
        phase_ = KnifeModelPhase::WaitingBinding;
    }
    if (!enabled)
    {
        return phase_ = KnifeModelPhase::Disabled;
    }
    // One failure per request/lifecycle; stable polls may not spin on native work.
    if (phase_ == KnifeModelPhase::Faulted)
    {
        return phase_;
    }
    const auto* asset = FindKnifeModelAsset(request.definition);
    if (!binding_ready || asset == nullptr)
    {
        return phase_ = KnifeModelPhase::WaitingBinding;
    }
    if (!captured_ && request.definition == observation.source_definition)
    {
        return phase_ = KnifeModelPhase::NativeModel;
    }
    if (!observation.native_pair_adapter_verified)
    {
        if (captured_ && !Retire(observation, adapter))
        {
            return phase_;
        }
        return phase_ = KnifeModelPhase::WaitingNativeAdapter;
    }
    if (captured_)
    {
        if (observation.now_millis < next_verify_millis_)
        {
            return phase_ = KnifeModelPhase::Applied;
        }
        if (!observation.safe_point)
        {
            return phase_ = KnifeModelPhase::WaitingSafePoint;
        }
        next_verify_millis_ = observation.now_millis + 500;
        if (adapter.BindingAlive(binding_) && adapter.VerifyPair(binding_, *asset))
        {
            return phase_ = KnifeModelPhase::Applied;
        }
        if (!Retire(observation, adapter))
        {
            return phase_;
        }
        return Fault();
    }
    if (!observation.safe_point)
    {
        return phase_ = KnifeModelPhase::WaitingSafePoint;
    }
    if (!adapter.BindingAlive(binding_) || !adapter.Capture(binding_))
    {
        return Fault();
    }
    captured_ = true;
    ++statistics_.apply_attempts;
    auto after = binding_;
    const bool applied = adapter.ApplyPair(binding_, *asset, after);
    if (!after.Valid() || !after.SameEntities(binding_))
    {
        // The native operation invalidated an entity, not merely its graph.
        // Do not replay the snapshot into the new occupant of that slot.
        adapter.Abandon();
        captured_ = false;
        ++statistics_.abandoned;
        return Fault();
    }
    binding_ = after;
    if (!applied || !adapter.VerifyPair(binding_, *asset))
    {
        if (!Retire(observation, adapter))
        {
            return phase_;
        }
        return Fault();
    }
    ++statistics_.successful_applies;
    next_verify_millis_ = observation.now_millis + 500;
    return phase_ = KnifeModelPhase::Applied;
}

const wchar_t* KnifeModelPhaseText(const KnifeModelPhase phase) noexcept
{
    switch (phase)
    {
    case KnifeModelPhase::Disabled: return L"Knife model disabled; finishes are independent.";
    case KnifeModelPhase::WaitingBinding: return L"Knife model: waiting for verified active knife / HUD / animation ownership.";
    case KnifeModelPhase::WaitingNativeAdapter: return L"Knife model: native model + AG2 pair adapter not yet verified; no model writes.";
    case KnifeModelPhase::WaitingSafePoint: return L"Knife model: waiting for game-thread safe point.";
    case KnifeModelPhase::NativeModel: return L"Knife model: target already equals the native knife type; no model writes.";
    case KnifeModelPhase::Applied: return L"Knife model: model + animation pair verified.";
    case KnifeModelPhase::WaitingHudRebuild: return L"Knife model: world prepared; waiting for the native replacement HUD.";
    case KnifeModelPhase::Faulted: return L"Knife model: transaction failed; retry blocked for this request/lifecycle.";
    default: return L"Knife model: unknown state.";
    }
}
}
