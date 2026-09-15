#include "Runtime/KnifeModel.hpp"
#include "Runtime/KnifeAppearance.hpp"
#include "Runtime/KnifeFinish.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>

namespace
{
using namespace nskinzcs2::runtime;
void Check(bool value, const char* message)
{
    if (!value) { throw std::runtime_error(message); }
}

struct Adapter final : KnifeModelAdapter
{
    bool alive = true;
    bool capture_ok = true;
    bool apply_ok = true;
    bool verify_ok = true;
    bool restore_ok = true;
    bool replace_graph = false;
    bool replace_entity = false;
    int captures = 0, applies = 0, verifies = 0, restores = 0, abandons = 0;
    KnifeModelBinding restored;
    bool BindingAlive(const KnifeModelBinding&) noexcept override { return alive; }
    bool Capture(const KnifeModelBinding&) noexcept override { ++captures; return capture_ok; }
    bool ApplyPair(const KnifeModelBinding&, const KnifeModelAsset&, KnifeModelBinding& after) noexcept override
    {
        ++applies;
        if (replace_graph) { after.graph_instance += 0x1000; }
        if (replace_entity) { after.hud_weapon.creation_tick += 1; }
        return apply_ok;
    }
    bool VerifyPair(const KnifeModelBinding&, const KnifeModelAsset&) noexcept override { ++verifies; return verify_ok; }
    bool Restore(const KnifeModelBinding& binding) noexcept override { ++restores; restored = binding; return restore_ok; }
    void Abandon() noexcept override { ++abandons; }
};

KnifeModelObservation Ready()
{
    KnifeModelObservation o;
    o.binding.scene_epoch = 1;
    o.binding.xuid = 76561198800578327ull;
    o.binding.pawn = {0x10000, 0x8101, 40};
    o.binding.world = {0x20000, 0x8201, 41};
    o.binding.hud_weapon = {0x30000, 0x8301, 42};
    o.binding.arms = {0x40000, 0x8401, 43};
    o.binding.weapon_graph_controller = 0x50000;
    o.binding.body_animation_controller = 0x60510;
    o.binding.graph_instance = 0x70000;
    o.source_definition = 525;
    o.offline_demo = true;
    o.authoritative_active_knife = true;
    o.ownership_verified = true;
    o.native_pair_adapter_verified = true;
    o.safe_point = true;
    return o;
}

void AssetsAndRequestIsolation()
{
    const auto& assets = KnifeModelAssets();
    Check(assets.size() == 22, "22 released knife types");
    for (std::size_t i = 0; i < assets.size(); ++i)
    {
        Check(!assets[i].animation_class.empty() && !assets[i].model.empty() && !assets[i].skeleton.empty() &&
            !assets[i].graph_animation_class.empty(), "complete asset mapping");
        for (std::size_t j = i + 1; j < assets.size(); ++j)
        {
            Check(assets[i].definition != assets[j].definition, "unique knife definitions");
        }
    }
    Check(FindKnifeModelAsset(508)->animation_class == "weapon_knife_m9_bayonet", "M9 animation name is not asset stem");
    Check(FindKnifeModelAsset(507)->graph_animation_class == "knife_karambit",
        "Karambit HUD graph animation class is distinct from its subclass name");
    Check(FindKnifeModelAsset(520)->skeleton.ends_with("knife_navaja.vnmskel"), "Navaja skeleton mapping");
    Check(FindKnifeModelAsset(526) != nullptr && FindKnifeModelAsset(524) == nullptr, "Kukri supported, unreleased alias rejected");
    Check(FindKnifeModelAsset(7) == nullptr, "gun is never a knife target");
    nskinzcs2::model::KnifeProfile profile;
    profile.enabled = true;
    profile.xuid = Ready().binding.xuid;
    profile.mode = nskinzcs2::model::KnifeProfileMode::ModelAndFinish;
    profile.model_definition = 508;
    const auto model_request = BuildKnifeModelRequest(profile);
    profile.paint_kit = 568;
    profile.seed = 777;
    profile.wear = 0.2f;
    profile.quality = 9;
    profile.custom_name = "测试";
    profile.explicit_vanilla_finish = true;
    Check(BuildKnifeModelRequest(profile) == model_request, "cosmetic edits must not change model request");
    const auto finish = BuildKnifeFinishRequest(profile);
    profile.model_definition = 526;
    const auto other_finish = BuildKnifeFinishRequest(profile);
    Check(BuildKnifeModelRequest(profile) != model_request, "target type changes model request");
    Check(BuildKnifeProfileApplySignature(finish, profile.xuid, 525, 1) ==
        BuildKnifeProfileApplySignature(other_finish, profile.xuid, 525, 1), "model selection must not change finish signature");
    Check(finish.model_definition == 0 && finish.mode == KnifeSlotMode::FinishOnly, "production finish chain owns no model");
    profile.mode = nskinzcs2::model::KnifeProfileMode::FinishOnly;
    Check(!BuildKnifeModelRequest(profile).Enabled(), "hidden retained target ignored in finish-only mode");
    profile.enabled = false;
    Check(BuildKnifeFinishRequest(profile).mode == KnifeSlotMode::None, "disabled profile stays disabled");
}

void BindingGates()
{
    for (int gate = 0; gate < 15; ++gate)
    {
        auto o = Ready();
        KnifeModelController state;
        Adapter adapter;
        const KnifeModelRequest request {o.binding.xuid, 508};
        switch (gate)
        {
        case 0: o.offline_demo = false; break;
        case 1: o.seeking = true; break;
        case 2: o.authoritative_active_knife = false; break;
        case 3: o.ownership_verified = false; break;
        case 4: o.binding.hud_weapon = o.binding.arms; break;
        case 5: o.binding.world.handle = 0xFFFFFFFFu; break;
        case 6: o.binding.graph_instance = 0; break;
        case 7: o.binding.xuid = 1; break;
        case 8: o.native_pair_adapter_verified = false; break;
        case 9: o.safe_point = false; break;
        case 10: o.binding.body_animation_controller = o.binding.arms.address; break;
        case 11: o.binding.graph_instance = o.binding.weapon_graph_controller; break;
        case 12: o.binding.graph_instance = o.binding.body_animation_controller; break;
        case 13: o.binding.hud_weapon.handle = o.binding.arms.handle; break;
        case 14: o.binding.arms.creation_tick = std::numeric_limits<int>::min(); break;
        }
        Check(state.Tick(request, o, adapter) != KnifeModelPhase::Applied, "unproven binding cannot apply");
        Check(adapter.captures == 0 && adapter.applies == 0, "gate performs no native work");
    }
}

void StableModelBudget()
{
    auto o = Ready();
    KnifeModelController state;
    Adapter adapter;
    const KnifeModelRequest request {o.binding.xuid, 508};
    const auto begin = std::chrono::steady_clock::now();
    for (int i = 0; i < 100000; ++i)
    {
        o.now_millis = static_cast<std::uint64_t>(i);
        Check(state.Tick(request, o, adapter) == KnifeModelPhase::Applied, "stable pair remains applied");
    }
    Check(adapter.applies == 1 && adapter.captures == 1 && adapter.restores == 0, "100k polls produce one transaction");
    Check(adapter.verifies == 200, "verification is bounded to two per second");
    std::cout << "knife_model_100k_polls_ms=" << std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - begin).count() << " applies=" << adapter.applies
        << " verifies=" << adapter.verifies << '\n';
}

void TargetChangeAndDisable()
{
    auto o = Ready();
    KnifeModelController state;
    Adapter adapter;
    KnifeModelRequest request {o.binding.xuid, 508};
    state.Tick(request, o, adapter);
    request.definition = 515;
    o.safe_point = false;
    Check(state.Tick(request, o, adapter) == KnifeModelPhase::WaitingSafePoint, "restore waits for safe point");
    Check(adapter.restores == 0 && adapter.applies == 1, "no half switch before safe point");
    o.safe_point = true;
    state.Tick(request, o, adapter);
    Check(adapter.restores == 1 && adapter.applies == 1, "retire and apply on different ticks");
    Check(state.Tick(request, o, adapter) == KnifeModelPhase::Applied && adapter.applies == 2, "new target applies next tick");
    Check(state.Tick({}, o, adapter) == KnifeModelPhase::Disabled && adapter.restores == 2, "finish-only/disable restores old model pair");
}

void LifecycleAndSeek()
{
    for (int change = 0; change < 5; ++change)
    {
        auto o = Ready();
        const auto original = o.binding;
        KnifeModelController state;
        Adapter adapter;
        KnifeModelRequest request {o.binding.xuid, 508};
        state.Tick(request, o, adapter);
        switch (change)
        {
        case 0: o.binding.xuid += 1; request.xuid = o.binding.xuid; break;
        case 1: o.binding.world.handle += 0x8000; adapter.alive = false; break;
        case 2: o.binding.world.creation_tick += 1; adapter.alive = false; break;
        case 3: o.seeking = true; o.binding.scene_epoch += 1; break;
        case 4: o.binding.graph_instance += 0x1000; adapter.alive = false; break;
        }
        state.Tick(request, o, adapter);
        if (change == 0)
        {
            Check(adapter.restores == 1 && adapter.restored == original, "restore original player, never incoming player");
        }
        else
        {
            Check(adapter.restores == 0 && adapter.abandons == 1, "no stale writes across reuse/seek/graph replacement");
        }
    }
}

void FailureIsLatched()
{
    for (int failure = 0; failure < 3; ++failure)
    {
        auto o = Ready();
        KnifeModelController state;
        Adapter adapter;
        const KnifeModelRequest request {o.binding.xuid, 508};
        if (failure == 0) { adapter.capture_ok = false; }
        if (failure == 1) { adapter.apply_ok = false; }
        if (failure == 2) { adapter.verify_ok = false; }
        Check(state.Tick(request, o, adapter) == KnifeModelPhase::Faulted, "partial transaction fails");
        for (int i = 0; i < 1000; ++i) { state.Tick(request, o, adapter); }
        Check(adapter.captures == 1 && adapter.applies <= 1, "failure cannot cause per-frame retries");
        Check(adapter.restores == (failure == 0 ? 0 : 1), "rollback after partial model/animation failure");
    }
}

void FailedRestoreCannotBeBypassed()
{
    auto o = Ready();
    KnifeModelController state;
    Adapter adapter;
    KnifeModelRequest request {o.binding.xuid, 508};
    adapter.apply_ok = false;
    adapter.restore_ok = false;
    state.Tick(request, o, adapter);
    request.definition = 515;
    for (int i = 0; i < 1000; ++i) { state.Tick(request, o, adapter); }
    Check(adapter.restores == 1 && adapter.applies == 1, "changing target does not retry failed restore");
    o.binding.scene_epoch += 1;
    state.Tick(request, o, adapter);
    Check(adapter.abandons == 1, "scene invalidation drops faulted snapshot without entity writes");
}

void HolsterAndCapabilityLoss()
{
    for (int loss = 0; loss < 2; ++loss)
    {
        auto o = Ready();
        KnifeModelController state;
        Adapter adapter;
        const KnifeModelRequest request {o.binding.xuid, 508};
        state.Tick(request, o, adapter);
        if (loss == 0) { o.authoritative_active_knife = false; }
        else { o.native_pair_adapter_verified = false; }
        Check(state.Tick(request, o, adapter) != KnifeModelPhase::Applied, "holster/unverified adapter stops replacement");
        Check(adapter.restores == 1, "restore before leaving active model chain");
    }
}

void DriftAndNativeNoOp()
{
    auto o = Ready();
    KnifeModelController state;
    Adapter adapter;
    const KnifeModelRequest request {o.binding.xuid, 508};
    state.Tick(request, o, adapter);
    adapter.verify_ok = false;
    o.now_millis = 500;
    Check(state.Tick(request, o, adapter) == KnifeModelPhase::Faulted && adapter.restores == 1, "pair drift restores instead of rewriting every frame");
    KnifeModelController native;
    Adapter no_op;
    Check(native.Tick({o.binding.xuid, o.source_definition}, o, no_op) == KnifeModelPhase::NativeModel, "same model needs no replacement");
    Check(no_op.applies == 0 && no_op.captures == 0, "same-model native no-op");
}

void FinishGenerationBudget()
{
    const auto o = Ready();
    KnifeFinishKey key {o.binding, 508, 1, 568};
    KnifeFinishLatch latch;
    Check(!latch.Begin(false, key, 100) && latch.Attempts() == 0, "do not paint the source or incomplete HUD");
    Check(latch.Begin(true, key, 100) && latch.Pending(key), "first target-model finish initializes once and waits");
    for (int i = 0; i < 10000; ++i)
        Check(!latch.Begin(true, key, 100 + i) && latch.Pending(key), "pending finish never regenerates materials");
    Check(!latch.TimedOut(key, 3100, 3000) && latch.TimedOut(key, 3101, 3000), "pending finish has one bounded timeout");
    latch.Complete();
    for (int i = 0; i < 10000; ++i)
        Check(!latch.Begin(true, key, 5000 + i) && latch.Applied(key), "steady finish never regenerates materials");
    ++key.appearance_signature;
    Check(latch.Begin(true, key, 20000), "paint/seed/wear edit initializes without changing the model generation");
    latch.Fail();
    for (int i = 0; i < 10000; ++i)
        Check(!latch.Begin(true, key, 20001 + i) && latch.Failed(key), "native failure is latched, not a frame-time loop");
    ++key.model_generation;
    Check(latch.Begin(true, key, 40000), "same paint on a new model generation cannot reuse the old finish cache");
    ++key.binding.hud_weapon.handle;
    Check(latch.Begin(true, key, 40001), "recreated HUD needs a new material transfer");
    ++key.binding.xuid;
    Check(latch.Begin(true, key, 40002), "same paint on another player is independent");
    key.target_definition = 515;
    Check(latch.Begin(true, key, 40003), "same paint on another model is independent");
    latch.Reset();
    Check(latch.Begin(true, key, 40004), "explicit disable discards only the finish latch");
}

void GraphRebuildIsNotEntityReuse()
{
    auto o = Ready();
    KnifeModelController state;
    Adapter adapter;
    adapter.replace_graph = true;
    const KnifeModelRequest request {o.binding.xuid, 508};
    Check(state.Tick(request, o, adapter) == KnifeModelPhase::Applied, "graph rebuild can commit a new graph identity");
    o.binding.graph_instance += 0x1000;
    Check(state.Tick(request, o, adapter) == KnifeModelPhase::Applied, "post-commit graph is stable, not an external entity change");
    Check(adapter.restores == 0 && adapter.applies == 1, "native graph rebuild cannot cause reapply loop");
    KnifeModelController reused;
    Adapter bad;
    bad.replace_entity = true;
    Check(reused.Tick(request, o, bad) == KnifeModelPhase::Faulted, "unexpected entity replacement during transaction rejected");
    Check(bad.abandons == 1 && bad.restores == 0, "invalidated entity must not receive snapshot writes");
}
}

void RunKnifeModelTests()
{
    AssetsAndRequestIsolation();
    BindingGates();
    StableModelBudget();
    TargetChangeAndDisable();
    LifecycleAndSeek();
    FailureIsLatched();
    FailedRestoreCannotBeBypassed();
    HolsterAndCapabilityLoss();
    DriftAndNativeNoOp();
    GraphRebuildIsNotEntityReuse();
    FinishGenerationBudget();
    std::cout << "independent knife model: 11 test groups passed\n";
}
