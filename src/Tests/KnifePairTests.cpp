#include "Runtime/KnifePair.hpp"

#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
using namespace nskinzcs2::runtime;
void Check(bool ok, const char* message) { if (!ok) { throw std::runtime_error(message); } }

KnifeModelBinding Binding()
{
    KnifeModelBinding b;
    b.scene_epoch = 1; b.xuid = 123;
    b.pawn = {0x10000, 0x8001, 10}; b.world = {0x20000, 0x8002, 11};
    b.hud_weapon = {0x30000, 0x8003, -1}; b.arms = {0x40000, 0x8004, -1};
    b.weapon_graph_controller = 0x50000; b.body_animation_controller = 0x60000; b.graph_instance = 0x70000;
    return b;
}

struct Port final : KnifePairAccess
{
    enum Event { World, Hud, Collect, Animation, Definition };
    KnifeModelBinding binding = Binding();
    KnifePairState state {525, 525, 1525, 2525, 9999, 3525, true, true, 525};
    bool alive = true, safe = true, readable = true, symbols = true;
    bool invalidate = false, fail_after_change = false;
    int fail_event = -1, fail_count = 0, mutations = 0;
    std::array<int, 5> counts {};
    std::vector<Event> events;
    // Sentinels deliberately outside the transaction port's interface.
    std::array<int, 6> cosmetics {568, 777, 12, 100, 200, 300};
    bool Alive(const KnifeModelBinding& b) noexcept override { return alive && b == binding; }
    bool SafePoint(const KnifeModelBinding& b) noexcept override { return safe && Alive(b); }
    bool Read(const KnifeModelBinding& b, KnifePairState& out) noexcept override
    { out = state; return readable && Alive(b); }
    std::uintptr_t ExistingAnimationType(const KnifeModelAsset& a) noexcept override
    { return symbols ? 3000 + a.definition : 0; }
    bool ReadItemDefinition(const KnifeModelBinding& b, int& definition) noexcept override
    { definition = state.item_definition; return Alive(b); }
    bool CompareItemDefinition(const KnifeModelBinding& b, int expected, int desired) noexcept override
    {
        if (!SafePoint(b) || state.item_definition != expected) return false;
        const bool ok = Begin(Definition);
        if (ok || fail_after_change) state.item_definition = desired;
        return ok;
    }
    bool Begin(Event event)
    {
        ++mutations; ++counts[event]; events.push_back(event);
        const bool fail = fail_event == event && fail_count++ == 0;
        if (fail && invalidate) { alive = false; }
        return !fail;
    }
    bool SetBladeModel(const KnifeModelBinding& b, KnifeBladeRole role, const KnifeModelAsset& a) noexcept override
    {
        if (!SafePoint(b)) { return false; }
        const bool ok = Begin(role == KnifeBladeRole::World ? World : Hud);
        if (ok || fail_after_change)
        {
            if (role == KnifeBladeRole::World) { state.world_definition = a.definition; state.world_model = 1000 + a.definition; }
            else { state.hud_definition = a.definition; state.hud_model = 2000 + a.definition; state.secondary_matches_hud = false; }
        }
        return ok;
    }
    bool Recollect(const KnifeModelBinding& b) noexcept override
    {
        if (!SafePoint(b)) { return false; }
        const bool ok = Begin(Collect);
        if (ok || fail_after_change) { state.secondary_matches_hud = true; }
        return ok;
    }
    bool CompareAnimationType(const KnifeModelBinding& b, std::uintptr_t expected, std::uintptr_t desired) noexcept override
    {
        if (!SafePoint(b) || state.animation_type != expected) { return false; }
        const bool ok = Begin(Animation);
        if (ok || fail_after_change) { state.animation_type = desired; }
        return ok;
    }
};

void EveryDirectedPair()
{
    int pairs = 0;
    for (const auto& source : KnifeModelAssets())
    {
        for (const auto& target : KnifeModelAssets())
        {
            Port p;
            p.state = {source.definition, source.definition, static_cast<std::uintptr_t>(1000 + source.definition),
                static_cast<std::uintptr_t>(2000 + source.definition), 9999,
                p.ExistingAnimationType(source), true, true, source.definition};
            const auto before = p.state;
            const auto cosmetics = p.cosmetics;
            KnifePairAdapter a(p);
            Check(a.Capture(p.binding), "capture source pair");
            auto after = p.binding;
            Check(a.ApplyPair(p.binding, target, after) && a.VerifyPair(after, target), "all canonical directed pairs can commit");
            if (source.definition != target.definition)
            {
                Check(p.events == std::vector<Port::Event>{Port::Definition, Port::World, Port::Hud, Port::Collect, Port::Animation},
                    "item/VData + world + HUD + explicit recollect BEFORE animation");
            }
            else { Check(p.mutations == 0, "same type is a no-op"); }
            Check(a.Restore(after) && !a.Captured() && p.state == before, "restore source pair");
            Check(p.cosmetics == cosmetics, "cosmetics never part of model restore");
            ++pairs;
        }
    }
    std::cout << "knife_pair_simulated_directed_pairs=" << pairs << " (not live animation validation)\n";
}

void PartialFailures()
{
    for (int event = 0; event != 5; ++event)
    {
        for (bool changed : {false, true})
        {
            Port p;
            const auto before = p.state;
            KnifePairAdapter a(p);
            Check(a.Capture(p.binding), "capture before injected failure");
            p.fail_event = event; p.fail_after_change = changed;
            auto after = p.binding;
            Check(!a.ApplyPair(p.binding, *FindKnifeModelAsset(508), after), "every partial native failure rejected");
            const auto calls = p.mutations;
            Check(!a.ApplyPair(p.binding, *FindKnifeModelAsset(508), after) && calls == p.mutations, "apply cannot retry partial transaction");
            Check(a.Restore(after) && p.state == before, "failure after a write still restores without finish snapshot");
        }
    }
}

void GatesAndCanonicalAssets()
{
    for (int gate = 0; gate < 11; ++gate)
    {
        Port p;
        switch (gate)
        {
        case 0: p.safe = false; break;
        case 1: p.alive = false; break;
        case 2: p.readable = false; break;
        case 3: p.symbols = false; break;
        case 4: p.state.world_definition = 7; break;
        case 5: p.state.hud_definition = 508; break;
        case 6: p.state.secondary_matches_hud = false; break;
        case 7: p.state.attachments_valid = false; break;
        case 8: p.state.arms_model = p.state.hud_model; break;
        case 9: p.state.animation_type = 999; break;
        case 10: p.state.item_definition = 508; break;
        }
        KnifePairAdapter a(p);
        Check(!a.Capture(p.binding) && !a.Captured() && !p.mutations, "invalid capture performs no native writes");
    }
    Port p;
    KnifePairAdapter a(p);
    Check(a.Capture(p.binding), "capture for forged asset test");
    auto forged = *FindKnifeModelAsset(508);
    forged.model = "untrusted.vmdl";
    auto after = p.binding;
    Check(!a.ApplyPair(p.binding, forged, after) && !p.mutations, "arbitrary model path not accepted");
    Check(!a.Capture(p.binding), "capture cannot discard restore point");
    p.symbols = false;
    Check(!a.ApplyPair(p.binding, *FindKnifeModelAsset(508), after) && !p.mutations, "missing target symbol cannot set models");
}

void LifecycleAndCompetingWrites()
{
    for (int change = 0; change < 9; ++change)
    {
        Port p;
        KnifePairAdapter a(p);
        Check(a.Capture(p.binding), "capture lifecycle");
        auto after = p.binding;
        Check(a.ApplyPair(p.binding, *FindKnifeModelAsset(508), after), "apply lifecycle");
        switch (change)
        {
        case 0: p.binding.scene_epoch++; break;
        case 1: p.binding.hud_weapon.handle += 0x8000; break;
        case 2: p.binding.hud_weapon.creation_tick++; break;
        case 3: p.binding.graph_instance++; break;
        case 4: p.state.arms_model++; break;
        case 5: p.state.world_definition = 515; break;
        case 6: p.state.animation_type = 3515; break;
        case 7: p.safe = false; break;
        case 8: p.state.item_definition = 515; break;
        }
        const auto count = p.mutations;
        Check(!a.Restore(after) && p.mutations == count, "no stale/competing writes during rollback");
        a.Abandon();
        Check(!a.Captured() && p.mutations == count, "abandon drops values only");
    }
    Port p;
    KnifePairAdapter a(p);
    Check(a.Capture(p.binding), "capture before native entity destruction");
    p.fail_event = Port::Hud; p.invalidate = true; p.fail_after_change = true;
    auto after = p.binding;
    Check(!a.ApplyPair(p.binding, *FindKnifeModelAsset(508), after), "native destruction fails transaction");
    const auto count = p.mutations;
    Check(!a.Restore(after) && count == p.mutations, "no restoration into destroyed entity");
}

void SteadyAnimationBudgetAndDrift()
{
    Port p;
    KnifePairAdapter a(p);
    const KnifeModelRequest request {p.binding.xuid, 508};
    Check(a.Capture(p.binding), "capture animation maintenance");
    auto after = p.binding;
    Check(a.ApplyPair(p.binding, *FindKnifeModelAsset(508), after), "apply animation maintenance");
    p.events.clear(); p.events.reserve(100001);
    for (int i = 0; i < 100000; ++i)
    {
        p.state.item_definition = 525; // Native updater reasserts source VData/definition.
        p.state.animation_type = 3525; // Native updater reasserts source VData.
        Check(a.AfterNativeUpdate(request, after) && p.state.item_definition == 508 &&
            p.state.animation_type == 3508, "target definition/animation survives native source rewrite");
    }
    Check(p.counts[Port::World] == 1 && p.counts[Port::Hud] == 1 && p.counts[Port::Collect] == 1,
        "100k animation updates cannot reload models/recollect skeletons");
    p.state.hud_model++;
    const auto count = p.mutations;
    Check(!a.AfterNativeUpdate(request, after) && a.MaintenanceFaulted(), "unexpected resource identity latches maintenance off");
    p.state.hud_model--;
    Check(!a.AfterNativeUpdate(request, after) && p.mutations == count, "no automatic per-frame recovery loop");
    Check(!a.VerifyPair(after, *FindKnifeModelAsset(508)), "controller can observe maintenance failure");
    Check(a.Restore(after), "latched maintenance still permits explicit retirement");
}

void MaintenanceGates()
{
    for (int loss = 0; loss < 8; ++loss)
    {
        Port p;
        KnifePairAdapter a(p);
        auto after = p.binding;
        KnifeModelRequest request {p.binding.xuid, 508};
        Check(a.Capture(after) && a.ApplyPair(after, *FindKnifeModelAsset(508), after), "prepare maintenance gate");
        switch (loss)
        {
        case 0: request = {}; break;
        case 1: request.xuid++; break;
        case 2: request.definition = 515; break;
        case 3: p.safe = false; break;
        case 4: p.state.secondary_matches_hud = false; break;
        case 5: p.state.attachments_valid = false; break;
        case 6: p.state.animation_type = 3515; break;
        case 7: p.state.item_definition = 515; break;
        }
        const auto count = p.mutations;
        Check(!a.AfterNativeUpdate(request, after) && count == p.mutations, "unsafe or foreign callback performs no writes");
    }
}
}

void RunKnifePairTests()
{
    EveryDirectedPair(); PartialFailures(); GatesAndCanonicalAssets();
    LifecycleAndCompetingWrites(); SteadyAnimationBudgetAndDrift(); MaintenanceGates();
    std::cout << "independent knife pair: 6 test groups passed\n";
}
