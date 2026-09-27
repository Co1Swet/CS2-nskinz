#include "Runtime/KnifeGraph.hpp"
#include "Runtime/KnifeResources.hpp"
#include "Runtime/KnifeSerialization.hpp"
#include "Runtime/KnifeNative.hpp"
#include "Runtime/KnifeLifecycle.hpp"
#include "Runtime/KnifeFinish.hpp"
#include "Runtime/KnifeUpdateProbe.hpp"
#include "Runtime/GloveVisualRefresh.hpp"
#include "Cs2/Runtime.hpp"

#include <Windows.h>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <vector>

namespace
{
using namespace nskinzcs2::runtime;
using Status = KnifeGraphProbeStatus;
void Check(bool value, const char* message)
{
    if (!value) { throw std::runtime_error(message); }
}

struct Memory final : KnifeGraphReader
{
    std::map<std::uintptr_t, std::uint8_t> bytes;
    std::uintptr_t id_node = 0x80000;
    std::size_t reads = 0, table_reads = 0, name_reads = 0;
    std::function<void()> on_second_sample;
    std::size_t body_reads = 0, secondary_reads = 0;
    std::function<void()> on_second_body, on_second_secondary;
    std::size_t resource_reads = 0;
    std::function<void()> on_second_resource;
    template<class T> void Set(std::uintptr_t address, const T& value)
    {
        const auto* data = reinterpret_cast<const std::uint8_t*>(&value);
        for (std::size_t i = 0; i < sizeof(T); ++i) { bytes[address + i] = data[i]; }
    }
    bool Read(std::uintptr_t address, void* output, std::size_t size) noexcept override
    {
        ++reads;
        if (address == 0x21000 && ++resource_reads == 2 && on_second_resource) { on_second_resource(); }
        if (address == 0x50120 && ++name_reads == 2 && on_second_sample) { on_second_sample(); }
        if (address == 0x60448 && ++body_reads == 2 && on_second_body) { on_second_body(); }
        if (address == 0x60020 && ++secondary_reads == 2 && on_second_secondary) { on_second_secondary(); }
        if (address >= 0x71000 && address < 0x71100) { ++table_reads; }
        auto* data = static_cast<std::uint8_t*>(output);
        for (std::size_t i = 0; i < size; ++i)
        {
            const auto found = bytes.find(address + i);
            if (found == bytes.end()) { return false; }
            data[i] = found->second;
        }
        return true;
    }
    bool IsIdParameterNode(std::uintptr_t node) noexcept override { return node == id_node; }
};

struct Fixture
{
    KnifeModelBinding binding;
    KnifeGraphSymbols symbols {0xA0000, 0xA1000, 0xA2000};
    Memory memory;
    Fixture()
    {
        binding.scene_epoch = 1;
        binding.xuid = 123;
        binding.pawn = {0x10000, 0x8001, 30};
        binding.world = {0x20000, 0x10002, 31};
        binding.hud_weapon = {0x30000, 0x18003, -1};
        binding.arms = {0x40000, 0x20004, -1};
        binding.weapon_graph_controller = 0x50000;
        binding.body_animation_controller = 0x60000;
        binding.graph_instance = 0x70000;
        memory.Set<std::uintptr_t>(0x50120, symbols.weapon_type_name);
        memory.Set<std::uintptr_t>(0x50128, binding.graph_instance);
        memory.Set<std::int16_t>(0x50130, 2);
        memory.Set<std::uintptr_t>(0x60448, binding.body_animation_controller);
        memory.Set<std::uintptr_t>(0x60450, binding.graph_instance);
        memory.Set<std::int32_t>(0x70008, 3);
        memory.Set<std::uintptr_t>(0x70010, 0x71000);
        memory.Set<std::uintptr_t>(0x71010, memory.id_node);
        memory.Set<std::uintptr_t>(0x80008, 0x90000);
        memory.Set<std::int16_t>(0x90008, 2);
        memory.Set<std::uintptr_t>(0x80018, symbols.source_type);
    }
    KnifeGraphProbeResult Probe() { return ProbeKnifeWeaponType(binding, symbols, true, memory); }
    void Expect(Status expected)
    {
        const auto result = Probe();
        Check(result.status == expected, "unexpected graph probe status");
        if (!result.SourceVerified())
        { Check(result.parameter == KnifeGraphParameter {}, "rejected probe must expose no stale node"); }
    }
};

void ValidAndReadOnly()
{
    Fixture f;
    const auto before = f.memory.bytes;
    const auto result = f.Probe();
    Check(result.status == Status::Ready && result.SourceVerified(), "canonical source ID verified");
    Check(result.parameter.node_count == 3 && result.parameter.node_index == 2 &&
        result.parameter.node == f.memory.id_node, "reads exact node selected by parameter reference");
    Check(f.memory.table_reads == 2, "one indexed lookup per sample, no table scan");
    f.symbols.target_type = 0;
    f.Expect(Status::SourceVerifiedTargetMissing);
    Check(f.memory.bytes == before, "probe has no writes, allocation or symbol registration in game memory");
    // A later graph rebuild must not reuse the last successful node address.
    f.memory.id_node = 0x81000;
    f.memory.Set<std::uintptr_t>(0x71010, f.memory.id_node);
    f.memory.Set<std::uintptr_t>(0x81008, 0x90000);
    f.memory.Set<std::uintptr_t>(0x81018, f.symbols.source_type);
    Check(f.Probe().parameter.node == 0x81000, "each probe resolves live node, not a cached pointer");
}

void GatesBeforeMemory()
{
    Fixture f;
    Check(ProbeKnifeWeaponType(f.binding, f.symbols, false, f.memory).status == Status::LayoutUnverified,
        "unverified instructions cannot enable graph reads");
    auto bad = f.binding;
    bad.arms.creation_tick = std::numeric_limits<int>::min();
    Check(ProbeKnifeWeaponType(bad, f.symbols, true, f.memory).status == Status::BindingInvalid,
        "unread creation tick distinct from valid native -1");
    auto missing = f.symbols;
    missing.source_type = 0;
    Check(ProbeKnifeWeaponType(f.binding, missing, true, f.memory).status == Status::SymbolsMissing,
        "missing source symbol cannot enable reads");
    Check(f.memory.reads == 0, "early gate rejection does not touch graph memory");
    missing = f.symbols;
    missing.weapon_type_name = 0;
    Check(ProbeKnifeWeaponType(f.binding, missing, true, f.memory).status == Status::SymbolsMissing,
        "missing parameter name symbol rejected");
}

void BoundedNodeLookup()
{
    for (const auto count : {-1, 0, 2, 32769})
    {
        Fixture f;
        f.memory.Set<std::int32_t>(0x70008, count);
        f.Expect(Status::InvalidNodeRange);
        Check(f.memory.table_reads == 0, "invalid count rejected before indexed read");
    }
    for (const std::int16_t index : {std::int16_t(-1), std::int16_t(3), std::int16_t(32767)})
    {
        Fixture f;
        f.memory.Set(0x50130, index);
        f.Expect(Status::InvalidNodeRange);
        Check(f.memory.table_reads == 0, "invalid signed index rejected before indexed read");
    }
    Fixture overflow;
    overflow.memory.Set<std::uintptr_t>(0x70010, std::numeric_limits<std::uintptr_t>::max() - 7);
    overflow.Expect(Status::Unreadable);
    Fixture unreadable;
    unreadable.memory.bytes.erase(0x80018);
    unreadable.Expect(Status::Unreadable);
    Fixture maximum;
    maximum.memory.Set<std::int32_t>(0x70008, 32768);
    maximum.Expect(Status::Ready);
}

void OwnershipAndNativeId()
{
    for (int change = 0; change < 7; ++change)
    {
        Fixture f;
        Status expected = Status::Ready;
        switch (change)
        {
        case 0: f.memory.Set<std::uintptr_t>(0x50120, 0xBAD); expected = Status::ParameterNameMismatch; break;
        case 1: f.memory.Set<std::uintptr_t>(0x50128, 0xBAD); expected = Status::GraphMismatch; break;
        case 2: f.memory.Set<std::uintptr_t>(0x60448, 0xBAD); expected = Status::GraphMismatch; break;
        case 3: f.memory.id_node = 0xBAD; expected = Status::WrongNodeType; break;
        case 4: f.memory.Set<std::int16_t>(0x90008, 1); expected = Status::WrongNodeDefinition; break;
        case 5: f.memory.Set<std::uintptr_t>(0x80018, f.symbols.source_type + 8);
            expected = Status::SourceTypeMismatch; break; // Equal string content would not make it the same native symbol.
        case 6: f.memory.Set<std::uintptr_t>(0x60450, 0xBAD); expected = Status::GraphMismatch; break;
        }
        f.Expect(expected);
    }
}

void ConcurrentRebuildRejected()
{
    for (int change = 0; change < 6; ++change)
    {
        Fixture f;
        f.memory.on_second_sample = [&]
        {
            switch (change)
            {
            case 0: f.memory.Set<std::uintptr_t>(0x50128, 0xBAD); break;
            case 1: f.memory.Set<std::int32_t>(0x70008, 4); break;
            case 2: f.memory.Set<std::uintptr_t>(0x80018, f.symbols.target_type); break;
            case 3: f.memory.bytes.erase(0x90008); break;
            case 4: f.memory.Set<std::uintptr_t>(0x60448, 0xBAD); break;
            case 5: f.memory.Set<std::uintptr_t>(0x60450, 0xBAD); break;
            }
        };
        f.Expect(Status::ChangedDuringRead);
    }
}

void BodyGraphWrapper()
{
    Fixture f;
    std::uintptr_t graph = 0xBAD;
    Check(!ReadKnifeBodyGraph(f.binding.body_animation_controller, false, f.memory, graph) && graph == 0 &&
        f.memory.reads == 0, "wrapper layout gate precedes memory access");
    Check(ReadKnifeBodyGraph(f.binding.body_animation_controller, true, f.memory, graph) &&
        graph == f.binding.graph_instance, "Schema wrapper must resolve its +8 value, not its owner");
    for (const auto bad : {std::uintptr_t(0), std::uintptr_t(0xBAD), f.binding.graph_instance})
    {
        f.memory.Set<std::uintptr_t>(0x60448, bad);
        Check(!ReadKnifeBodyGraph(f.binding.body_animation_controller, true, f.memory, graph) && graph == 0,
            "foreign wrapper owner cannot resolve an instance");
    }
    f.memory.Set<std::uintptr_t>(0x60448, f.binding.body_animation_controller);
    for (const auto bad : {std::uintptr_t(0), f.binding.body_animation_controller})
    {
        f.memory.Set<std::uintptr_t>(0x60450, bad);
        Check(!ReadKnifeBodyGraph(f.binding.body_animation_controller, true, f.memory, graph) && graph == 0,
            "null or self-referential graph rejected");
    }
    const auto reads = f.memory.reads;
    Check(!ReadKnifeBodyGraph(std::numeric_limits<std::uintptr_t>::max() - 10, true, f.memory, graph) &&
        graph == 0 && f.memory.reads == reads, "wrapper pointer overflow rejected before reads");
    Fixture drift;
    drift.memory.on_second_body = [&] { drift.memory.Set<std::uintptr_t>(0x60450, 0x80000); };
    Check(!ReadKnifeBodyGraph(drift.binding.body_animation_controller, true, drift.memory, graph) && graph == 0,
        "graph replacement during wrapper read rejected");
}

void SecondarySkeletonBinding()
{
    const auto seed = [](Fixture& f)
    {
        f.memory.Set<std::int32_t>(0x60020, 2);
        f.memory.Set<std::uintptr_t>(0x60028, 0xB0000);
        f.memory.Set<std::int32_t>(0x60030, 4);
        f.memory.Set<std::int32_t>(0x60038, 2);
        f.memory.Set<std::uintptr_t>(0x60040, 0xB1000);
        f.memory.Set<std::int32_t>(0x60048, 8);
        f.memory.Set<std::int32_t>(0xC0050, 1);
        f.memory.Set<std::uintptr_t>(0xC0448, 0xC0000);
        f.memory.Set<std::uintptr_t>(0xB0000, 0xD1000);
        f.memory.Set<std::uintptr_t>(0xB0008, 0xD0000); // target at index 1 tests differing strides
        f.memory.Set<std::uint32_t>(0xB1000, 0x8123);
        f.memory.Set<std::uint32_t>(0xB1004, f.binding.hud_weapon.handle);
    };
    Fixture f; seed(f);
    KnifeSecondarySkeleton result;
    Check(!ProbeKnifeSecondarySkeleton(f.binding, 0xC0000, 0xD0000, false, f.memory, result) && f.memory.reads == 0,
        "secondary layout gate precedes reads");
    const auto before = f.memory.bytes;
    Check(ProbeKnifeSecondarySkeleton(f.binding, 0xC0000, 0xD0000, true, f.memory, result) &&
        result.index == 1 && result.hud_handle == f.binding.hud_weapon.handle,
        "8-byte slot symbol / 4-byte full HUD handle pairing");
    Check(before == f.memory.bytes, "secondary probe never changes graph or counters");
    for (int change = 0; change < 15; ++change)
    {
        Fixture bad; seed(bad);
        std::uintptr_t controller = 0xC0000;
        switch (change)
        {
        case 0: bad.memory.Set<int>(0x60020, 0); break;
        case 1: bad.memory.Set<int>(0x60020, 65); break;
        case 2: bad.memory.Set<int>(0x60038, 1); break;
        case 3: bad.memory.Set<int>(0x60030, 1); break;
        case 4: bad.memory.Set<int>(0x60048, 1); break;
        case 5: bad.memory.Set<int>(0xC0050, 0); break;
        case 6: bad.memory.Set<int>(0xC0050, 2); break;
        case 7: bad.memory.Set<std::uintptr_t>(0xC0448, 0x60000); break;
        case 8: bad.memory.Set<std::uintptr_t>(0xB0000, 0xD0000); break; // duplicate slot
        case 9: bad.memory.Set<std::uint32_t>(0xB1000, bad.binding.hud_weapon.handle); break; // duplicate owner
        case 10: bad.memory.Set<std::uint32_t>(0xB1004, bad.binding.hud_weapon.handle + 0x8000); break; // same slot/new serial
        case 11: bad.memory.Set<std::uintptr_t>(0x60028, std::numeric_limits<std::uintptr_t>::max() - 3); break;
        case 12: bad.memory.bytes.erase(0xB1004); break;
        case 13: controller = bad.binding.arms.address; break;
        case 14: bad.memory.Set<int>(0x60048, 32769); break;
        }
        Check(!ProbeKnifeSecondarySkeleton(bad.binding, controller, 0xD0000, true, bad.memory, result) &&
            result == KnifeSecondarySkeleton {}, "invalid secondary association must clear previous output");
    }
    Fixture drift; seed(drift);
    drift.memory.on_second_secondary = [&] { drift.memory.Set<std::uint32_t>(0xB1004, 0xDEAD); };
    Check(!ProbeKnifeSecondarySkeleton(drift.binding, 0xC0000, 0xD0000, true, drift.memory, result),
        "secondary rebind during read rejected");
}

void ResourcePairOwnership()
{
    const KnifeResourceFields fields {0x21000, 0x31000, 0x41000};
    const auto seed = [](Fixture& f)
    {
        f.memory.Set<std::uintptr_t>(0x21000, 0xE0000);
        f.memory.Set<std::uintptr_t>(0x31000, 0xE1000);
        f.memory.Set<std::uintptr_t>(0x41000, 0xE2000);
        f.memory.Set<std::uintptr_t>(0xE0000, 0xF0000);
        f.memory.Set<std::uintptr_t>(0xE1000, 0xF1000);
        f.memory.Set<std::uintptr_t>(0xE2000, 0xF2000);
        f.memory.Set<int>(0xF0440, 1);
        f.memory.Set<int>(0xF1440, 1);
        f.memory.Set<std::uintptr_t>(0xF0448, 0xF3000);
        f.memory.Set<std::uintptr_t>(0xF1448, 0xF4000);
        f.memory.Set<std::uintptr_t>(0xF3000, 0xF5000);
        f.memory.Set<std::uintptr_t>(0xF4000, 0xF5000);
        f.memory.Set<int>(0x70338, 2);
        f.memory.Set<std::uintptr_t>(0x70340, 0xF6000);
        f.memory.Set<std::uintptr_t>(0xF6000, 0xF7000);
        f.memory.Set<std::uintptr_t>(0xF6008, 0xF5000);
    };
    Fixture f; seed(f);
    KnifeResourcePair pair;
    Check(!ProbeKnifeResourcePair(f.binding, fields, false, f.memory, pair) && !f.memory.reads,
        "unverified resource layout cannot read game memory");
    const auto before = f.memory.bytes;
    Check(ProbeKnifeResourcePair(f.binding, fields, true, f.memory, pair) && pair.blade_skeletons_match &&
        pair.hud_skeleton_in_graph && pair.graph_count == 2, "actual model references must reach the current arms graph");
    Check(f.memory.bytes == before, "resource probe has no writes or reference-count side effects");
    // Resource binding equality is NOT required for world/HUD loaded model data.
    f.memory.Set<std::uintptr_t>(0xE1000, 0xF0000);
    Check(ProbeKnifeResourcePair(f.binding, fields, true, f.memory, pair) && pair.hud_model == pair.world_model,
        "world and HUD may share a model resource");
    for (int change = 0; change < 17; ++change)
    {
        Fixture bad; seed(bad);
        auto input = fields;
        switch (change)
        {
        case 0: input.world = input.hud; break;
        case 1: bad.memory.Set<std::uintptr_t>(0x21000, 0); break;
        case 2: bad.memory.Set<std::uintptr_t>(0xE1000, 0); break;
        case 3: bad.memory.Set<std::uintptr_t>(0xE0000, 0xE0000); break;
        case 4: bad.memory.Set<std::uintptr_t>(0xE2000, 0xF0000); break;
        case 5: bad.memory.Set<int>(0xF0440, 0); break;
        case 6: bad.memory.Set<int>(0xF1440, 2); break;
        case 7: bad.memory.Set<std::uintptr_t>(0xF4000, 0); break;
        case 8: bad.memory.Set<int>(0x70338, -1); break;
        case 9: bad.memory.Set<int>(0x70338, 65); break;
        case 10: bad.memory.Set<std::uintptr_t>(0x70340, 0); break;
        case 11: bad.memory.Set<std::uintptr_t>(0xF6000, 0xF5000); break;
        case 12: bad.memory.Set<std::uintptr_t>(0xF6008, 0); break;
        case 13: bad.memory.Set<std::uintptr_t>(0x60450, 0x71000); break;
        case 14: input.world = std::numeric_limits<std::uintptr_t>::max() - 4; break;
        case 15: bad.memory.Set<std::uintptr_t>(0x70340, std::numeric_limits<std::uintptr_t>::max() - 4); break;
        case 16: bad.memory.bytes.erase(0xF1440); break;
        }
        Check(!ProbeKnifeResourcePair(bad.binding, input, true, bad.memory, pair) && pair == KnifeResourcePair {},
            "invalid resources clear outputs and cannot qualify for a model transaction");
    }
    Fixture pending; seed(pending);
    pending.memory.Set<std::uintptr_t>(0xF3000, 0xFA000);
    pending.memory.Set<int>(0x70338, 0);
    pending.memory.Set<std::uintptr_t>(0x70340, 0);
    Check(ProbeKnifeResourcePair(pending.binding, fields, true, pending.memory, pair) &&
        !pair.blade_skeletons_match && !pair.hud_skeleton_in_graph,
        "partial model commit or pending recollection is readable but not pair-ready");
    Fixture drift; seed(drift);
    drift.memory.on_second_resource = [&] { drift.memory.Set<std::uintptr_t>(0xF6008, 0xFA000); };
    Check(!ProbeKnifeResourcePair(drift.binding, fields, true, drift.memory, pair) && pair == KnifeResourcePair {},
        "concurrent graph reference change rejected instead of caching a stale pointer");
    for (const auto& asset : KnifeModelAssets())
    {
        Check(FindKnifeModelAssetByPath(asset.model) == &asset &&
            FindKnifeModelAssetByPath(std::string(asset.model) + "_c") == &asset,
            "source definition is derived from canonical model path, not Econ identity");
        Check(FindKnifeModelAssetByAnimationClass(asset.animation_class) == &asset &&
            FindKnifeModelAssetByAnimationClass(asset.graph_animation_class) == &asset,
            "source definition is recoverable from either native knife animation class");
    }
    Check(!FindKnifeModelAssetByPath("weapons/models/rifles/ak47/weapon_ak47.vmdl") &&
        !FindKnifeModelAssetByPath("../knife_m9.vmdl") && !FindKnifeModelAssetByPath("weapon_knife_m9_bayonet"),
        "animation class and arbitrary model strings are not model path evidence");
    Check(FindKnifeModelAssetByAnimationClass("weapon_knife_t")->definition == 59 &&
        !FindKnifeModelAssetByAnimationClass("weapon_deagle") &&
        !FindKnifeModelAssetByAnimationClass(""),
        "mixed target geometry can recover the recorded T knife animation without accepting gun classes");
}

struct NativeFixture final : KnifeNativeHost
{
    Fixture f;
    KnifeNativeContext context;
    bool alive = true, safe = true, attached = true, fail_after_change = false;
    bool world_alive = true;
    KnifeWorldOwnership world_ownership = KnifeWorldOwnership::Owned;
    int item_definition = 0;
    KnifeBladePresence old_hud_presence = KnifeBladePresence::Gone;
    bool retirement_safe = true;
    std::uint64_t epoch = 1;
    KnifeRetirementGraph retirement_graph = KnifeRetirementGraph::Live;
    std::array<KnifeBladePresence, 2> presence {KnifeBladePresence::Live, KnifeBladePresence::Live};
    std::size_t model_name_reads = 0;
    std::function<void(int)> after_set;
    int fail_event = -1;
    std::array<int, 5> calls {};
    std::vector<int> events;
    NativeFixture(int source_definition = 525, int target_definition = 508)
    {
        context.binding = f.binding; context.resources = {0x21000, 0x31000, 0x41000};
        context.model_names = {0x21008, 0x31008, 0x41008};
        context.client = 0x10000000; context.client_size = 0x3000000;
        context.source_definition = source_definition; context.target_definition = target_definition;
        item_definition = source_definition;
        context.symbols = f.symbols; context.hud_body_controller = 0xC0000; context.slot_symbol = 0xD0000;
        for (const auto windows : {std::span<const KnifeResourceEvidenceWindow>(KnifeNativeEvidenceWindows()),
            std::span<const KnifeResourceEvidenceWindow>(KnifeResourceEvidenceWindows())})
            for (const auto& window : windows)
                for (std::size_t i = 0; i < window.bytes.size(); ++i)
                    f.memory.Set(context.client + window.rva + i, window.bytes[i]);
        Set<std::uintptr_t>(0x21000, 0xE0000); Set<std::uintptr_t>(0x31000, 0xE1000);
        Set<std::uintptr_t>(0x41000, 0xE2000);
        Set<std::uintptr_t>(0xE0000, 0xF0000); Set<std::uintptr_t>(0xE1000, 0xF0000); Set<std::uintptr_t>(0xE2000, 0xF2000);
        Set<int>(0xF0440, 1); Set<int>(0xF1440, 1);
        Set<std::uintptr_t>(0xF0448, 0xF3000); Set<std::uintptr_t>(0xF1448, 0xF4000);
        Set<std::uintptr_t>(0xF3000, 0xF5000); Set<std::uintptr_t>(0xF4000, 0xFA000);
        Set<int>(0x70338, 1); Set<std::uintptr_t>(0x70340, 0xF6000); Set<std::uintptr_t>(0xF6000, 0xF5000);
        Set<int>(0x60020, 1); Set<std::uintptr_t>(0x60028, 0xB0000); Set<int>(0x60030, 4);
        Set<int>(0x60038, 1); Set<std::uintptr_t>(0x60040, 0xB1000); Set<int>(0x60048, 8);
        Set<std::uintptr_t>(0xB0000, context.slot_symbol); Set<std::uint32_t>(0xB1000, f.binding.hud_weapon.handle);
        Set<int>(0xC0050, 1); Set<std::uintptr_t>(0xC0448, 0xC0000);
        Name(0, FindKnifeModelAsset(source_definition)->model); Name(1, FindKnifeModelAsset(source_definition)->model);
        Name(2, "characters/models/test/arms.vmdl");
    }
    template<class T> void Set(std::uintptr_t address, T value) { f.memory.Set(address, value); }
    void Name(std::size_t i, std::string_view path)
    {
        const auto address = 0x110000 + i * 0x1000;
        Set<std::uintptr_t>(context.model_names[i], address);
        for (std::size_t j = 0; j < path.size(); ++j) Set<char>(address + j, path[j]);
        Set<char>(address + path.size(), 0);
    }
    bool Read(std::uintptr_t a, void* out, std::size_t n) noexcept override
    { if (a >= 0x110000 && a < 0x113000) ++model_name_reads; return f.memory.Read(a, out, n); }
    bool IsIdParameterNode(std::uintptr_t n) noexcept override { return f.memory.IsIdParameterNode(n); }
    bool BindingAlive(const KnifeNativeContext& c) noexcept override { return alive && c.binding == f.binding; }
    bool WorldBindingAlive(const KnifeNativeContext& c) noexcept override
    { return world_alive && epoch == c.binding.scene_epoch && c.binding.world == f.binding.world && c.binding.pawn == f.binding.pawn; }
    KnifeWorldOwnership WorldOwnership(const KnifeNativeContext& c) noexcept override
    { return epoch == c.binding.scene_epoch ? world_ownership : KnifeWorldOwnership::Unreadable; }
    bool ReadWorldItemDefinition(const KnifeNativeContext& c, int& out) noexcept override
    {
        if (epoch != c.binding.scene_epoch || presence[0] != KnifeBladePresence::Live) return false;
        out = item_definition;
        return out == c.source_definition || out == c.target_definition;
    }
    bool CompareExchangeWorldItemDefinition(const KnifeNativeContext& c, int expected, int desired) noexcept override
    {
        if ((!safe && !retirement_safe) || epoch != c.binding.scene_epoch || world_ownership != KnifeWorldOwnership::Owned ||
            presence[0] != KnifeBladePresence::Live || item_definition != expected ||
            (desired != c.source_definition && desired != c.target_definition)) return false;
        const bool ok = Begin(4);
        if (ok || fail_after_change) item_definition = desired;
        return ok;
    }
    bool AtSafePoint(const KnifeModelBinding& b) noexcept override { return safe && alive && b == f.binding; }
    bool AttachmentsValid(const KnifeModelBinding& b) noexcept override { return attached && alive && b == f.binding; }
    std::uint64_t SceneEpoch() noexcept override { return epoch; }
    bool AtRetirementPoint() noexcept override { return retirement_safe; }
    KnifeBladePresence BladePresence(const KnifeNativeContext& c, KnifeBladeRole role) noexcept override
    {
        if (role == KnifeBladeRole::Hud && c.binding.hud_weapon != f.binding.hud_weapon) return old_hud_presence;
        return presence[role == KnifeBladeRole::World ? 0 : 1];
    }
    KnifeRetirementGraph RetirementGraph(const KnifeNativeContext&, KnifeModelBinding& out) noexcept override
    { out = f.binding; return retirement_graph; }
    void NewGraph()
    {
        f.binding.graph_instance = 0x72000;
        Set<std::uintptr_t>(0x60450, f.binding.graph_instance); Set<std::uintptr_t>(0x50128, f.binding.graph_instance);
        Set<int>(0x72008, 3); Set<std::uintptr_t>(0x72010, 0x73000);
        f.memory.id_node = 0x81000;
        Set<std::uintptr_t>(0x73010, f.memory.id_node); Set<std::uintptr_t>(0x81008, 0x90000);
        Set<std::uintptr_t>(0x81018, context.symbols.target_type);
        Set<int>(0x72338, 1); Set<std::uintptr_t>(0x72340, 0xF6000);
    }
    void ReplaceHud(bool inherits_target = false)
    {
        // Simulate the NEXT-frame consequence of world SetModel's +0x780:
        // old HUD removed, new identity registered with the source weapon data.
        f.binding.hud_weapon.address += 0x100000;
        f.binding.hud_weapon.handle += 0x8007;
        Check(f.binding.Valid(), "replacement fixture uses a distinct valid HUD identity");
        context.binding = f.binding;
        context.resources.hud += 0x100000;
        context.model_names[1] = context.resources.hud + 8;
        Set<std::uintptr_t>(context.resources.hud, 0xE1000);
        Set<std::uintptr_t>(0xE1000, inherits_target ? 0xF1000 : 0xF0000);
        Name(1, FindKnifeModelAsset(inherits_target ? context.target_definition : context.source_definition)->model);
        Set<std::uint32_t>(0xB1000, f.binding.hud_weapon.handle);
        Set<std::uintptr_t>(0xF6000, inherits_target ? 0xFA000 : 0xF5000);
        Set<std::uintptr_t>(f.memory.id_node + 0x18, inherits_target ? context.symbols.target_type : context.symbols.source_type);
        alive = true; presence[1] = KnifeBladePresence::Live;
    }
    bool Begin(int event)
    {
        ++calls[event]; events.push_back(event);
        if (fail_event != event) return true;
        fail_event = -1;
        return false;
    }
    bool InvokeSetModel(std::uintptr_t entry, std::uintptr_t entity, const char* path) noexcept override
    {
        const auto* asset = FindKnifeModelAssetByPath(path);
        const auto role = entity == f.binding.world.address ? 0 : 1;
        if (entry != context.client + kKnifeSetModelRva || !asset || (!safe && !retirement_safe) ||
            (role == 0 && world_ownership != KnifeWorldOwnership::Owned) ||
            (entity != f.binding.world.address && entity != f.binding.hud_weapon.address)) return false;
        const bool ok = Begin(role);
        if (ok || fail_after_change)
        {
            Name(role, asset->model);
            Set<std::uintptr_t>(role ? 0xE1000 : 0xE0000, asset->definition == context.source_definition ? 0xF0000 : 0xF1000);
        }
        if (after_set) after_set(role);
        return ok;
    }
    bool InvokeRecollect(std::uintptr_t entry, std::uintptr_t controller) noexcept override
    {
        if (entry != context.client + kKnifeRecollectRva || controller != f.binding.body_animation_controller || (!safe && !retirement_safe)) return false;
        const bool ok = Begin(2);
        if (ok || fail_after_change)
        {
            std::uintptr_t model = 0; Read(0xE1000, &model, sizeof(model));
            Set<std::uintptr_t>(0xF6000, model == 0xF0000 ? 0xF5000 : 0xFA000);
        }
        return ok;
    }
    bool CompareExchangeSymbol(std::uintptr_t address, std::uintptr_t expected, std::uintptr_t desired) noexcept override
    {
        std::uintptr_t current = 0;
        if (address != f.memory.id_node + 0x18 || (!safe && !retirement_safe) || !Read(address, &current, sizeof(current)) || current != expected) return false;
        const bool ok = Begin(3);
        if (ok || fail_after_change) Set<std::uintptr_t>(address, desired);
        return ok;
    }
};

void StockKnifeModelHandoff()
{
    for (int source : {42, 59})
        for (int target : {508, 515, 525})
            for (bool inherited : {false, true})
            {
                NativeFixture f(source, target);
                KnifeLifecycle lifecycle(f);
                const KnifeModelRequest request {f.context.binding.xuid, target};
                const auto tick = [&](std::uint64_t now)
                {
                    const auto& b = f.f.binding;
                    lifecycle.OnUpdate({KnifeUpdatePhase::BeforeArms, b.arms.address, b.weapon_graph_controller, b.world.address}, now);
                    lifecycle.OnUpdate({KnifeUpdatePhase::AfterWeapon, b.arms.address, b.weapon_graph_controller, b.world.address}, now);
                };
                lifecycle.Stage(request, &f.context, 100); tick(100);
                Check(lifecycle.Phase() == KnifeModelPhase::WaitingHudRebuild, "CT/T stock knife enters the same native world handoff");
                f.ReplaceHud(inherited);
                lifecycle.Stage(request, &f.context, 101); tick(101);
                Check(lifecycle.Owns(request, f.context.binding) && lifecycle.Statistics().applied == 1,
                    "CT/T stock knife completes premium world/HUD/skeleton replacement");
                Check(f.context.source_definition == source, "stock source weapon identity is preserved");
                lifecycle.Request({}); tick(102);
                KnifeNativePairAccess port(f.context, f);
                KnifePairState restored;
                Check(port.Read(f.context.binding, restored) && restored.world_definition == source && restored.hud_definition == source,
                    "disable restores the correct CT/T source model, not the other team's knife");
            }
}

void NativePrearmedHandoff()
{
    Check(ShouldWaitForHolsteredKnifePrearm(true, false, false, 525, 508),
        "an unprepared active source knife waits for holstered pre-arm");
    Check(!ShouldWaitForHolsteredKnifePrearm(true, true, false, 525, 508) &&
        !ShouldWaitForHolsteredKnifePrearm(true, false, true, 525, 508) &&
        !ShouldWaitForHolsteredKnifePrearm(true, false, false, 508, 508),
        "prearmed, already-owned and native-target pairs do not enter the active rebuild wait");
    Check(!ShouldWaitForHolsteredKnifePrearm(true, false, false, 59, 507, true),
        "a fully verified active knife is no longer stranded once its native animation cache bridge is armed");
    for (int source : {42, 59, 507, 508, 515, 525})
        for (int target : {42, 59, 507, 508, 515, 525})
            for (bool bridge : {false, true})
                Check(ShouldWaitForHolsteredKnifePrearm(true, false, false, source, target, bridge) ==
                    (source != target && !bridge), "active-source gate requires verified bridge for every knife pair");
    const KnifeModelRequest leased {76561199840349579ull, 507};
    Check(ShouldPreserveKnifePrearmOnRefresh(true, true, leased, leased),
        "repeated Update and unrelated finish changes keep an owned same-model prearm");
    Check(!ShouldPreserveKnifePrearmOnRefresh(false, true, leased, leased) &&
        !ShouldPreserveKnifePrearmOnRefresh(true, false, leased, leased) &&
        !ShouldPreserveKnifePrearmOnRefresh(true, true, leased, {}) &&
        !ShouldPreserveKnifePrearmOnRefresh(true, true, leased, {leased.xuid, 508}) &&
        !ShouldPreserveKnifePrearmOnRefresh(true, true, leased, {leased.xuid + 1, 507}) &&
        !ShouldPreserveKnifePrearmOnRefresh(true, true, {}, {}),
        "seek, ownership transfer, disable and changed model/player never inherit a prearm lease");
    Check(ShouldKeepKnifeUpdateListener(true, true, false, true, false, true, false) &&
        ShouldKeepKnifeUpdateListener(true, true, false, true, false, false, true) &&
        ShouldKeepKnifeUpdateListener(true, true, false, true, true, false, false),
        "owned, prearmed and freshly read knife bindings retain the exact native callback listener");
    Check(!ShouldKeepKnifeUpdateListener(false, true, false, true, true, true, true) &&
        !ShouldKeepKnifeUpdateListener(true, false, false, true, true, true, true) &&
        !ShouldKeepKnifeUpdateListener(true, true, true, true, true, true, true) &&
        !ShouldKeepKnifeUpdateListener(true, true, false, false, true, true, true) &&
        !ShouldKeepKnifeUpdateListener(true, true, false, true, false, false, false),
        "disabled, online, seeking, unauthorized and unowned gaps clear the knife callback listener");
    NativeFixture f(42, 515);
    f.context.world_prearmed = true;
    f.item_definition = f.context.target_definition;
    f.Name(0, FindKnifeModelAsset(f.context.target_definition)->model);
    f.Set<std::uintptr_t>(0xE0000, 0xF1000);

    KnifeLifecycle lifecycle(f);
    const KnifeModelRequest request {f.context.binding.xuid, f.context.target_definition};
    const auto event = [&](const KnifeUpdatePhase phase)
    {
        const auto& b = f.f.binding;
        return KnifeUpdateEvent {phase, b.arms.address, b.weapon_graph_controller, b.world.address};
    };
    lifecycle.Stage(request, &f.context, 100);
    lifecycle.OnUpdate(event(KnifeUpdatePhase::BeforeArms), 100);
    Check(lifecycle.Phase() == KnifeModelPhase::WaitingBinding &&
        !lifecycle.Owns(request, f.context.binding) && f.events.empty(),
        "prearmed world never adopts against the outgoing pistol graph at BeforeArms");
    lifecycle.OnUpdate(event(KnifeUpdatePhase::BeforeWeapon), 100);
    lifecycle.OnUpdate(event(KnifeUpdatePhase::AfterWeapon), 100);
    Check(lifecycle.Phase() == KnifeModelPhase::Applied && lifecycle.Owns(request, f.context.binding),
        "prearmed world becomes an owned target pair at the exact knife weapon callback");
    Check(lifecycle.Statistics().attempts == 1 && lifecycle.Statistics().prearmed_adoptions == 1 &&
        lifecycle.Statistics().applied == 1 && f.events == std::vector<int>{1,2,3},
        "prearmed handoff synchronizes HUD identity/model/recollection/weapon_type before arms update and never rebuilds the world again");

    NativeFixture after_arms(42, 515);
    after_arms.context.world_prearmed = true;
    after_arms.item_definition = after_arms.context.target_definition;
    after_arms.Name(0, FindKnifeModelAsset(after_arms.context.target_definition)->model);
    after_arms.Set<std::uintptr_t>(0xE0000, 0xF1000);
    KnifeLifecycle after_arms_lifecycle(after_arms);
    const KnifeModelRequest after_arms_request {
        after_arms.context.binding.xuid, after_arms.context.target_definition};
    after_arms_lifecycle.Stage(after_arms_request, &after_arms.context, 100);
    const auto& after_arms_binding = after_arms.context.binding;
    after_arms_lifecycle.OnUpdate({KnifeUpdatePhase::AfterArms,
        after_arms_binding.arms.address, 0, 0}, 100);
    Check(after_arms_lifecycle.Owns(after_arms_request, after_arms.context.binding) &&
        after_arms_lifecycle.Statistics().prearmed_adoptions == 1,
        "prearmed target graph can be adopted immediately after the native arms updater rebuilt it");

    KnifeNativePairAccess reader(f.context, f);
    KnifePairState target;
    Check(reader.Read(f.context.binding, target) &&
        target.world_definition == f.context.target_definition &&
        target.hud_definition == f.context.target_definition &&
        target.animation_type == f.context.symbols.target_type && target.secondary_matches_hud,
        "prearmed handoff removes the source HUD skeleton and source animation branch");

    lifecycle.Request({});
    lifecycle.OnUpdate(event(KnifeUpdatePhase::BeforeArms), 101);
    KnifePairState restored;
    Check(reader.Read(f.context.binding, restored) &&
        restored.world_definition == f.context.source_definition &&
        restored.hud_definition == f.context.source_definition &&
        restored.item_definition == f.context.source_definition &&
        restored.animation_type == f.context.symbols.source_type,
        "prearmed adoption keeps the original CT/T knife as a complete restore identity");

    NativeFixture inherited(525, 508);
    inherited.context.world_prearmed = true;
    inherited.item_definition = inherited.context.target_definition;
    inherited.Name(0, FindKnifeModelAsset(inherited.context.target_definition)->model);
    inherited.Name(1, FindKnifeModelAsset(inherited.context.target_definition)->model);
    inherited.Set<std::uintptr_t>(0xE0000, 0xF1000);
    inherited.Set<std::uintptr_t>(0xE1000, 0xF1000);
    inherited.Set<std::uintptr_t>(0xF6000, 0xFA000);
    KnifeLifecycle inherited_lifecycle(inherited);
    const KnifeModelRequest inherited_request {
        inherited.context.binding.xuid, inherited.context.target_definition};
    inherited_lifecycle.Stage(inherited_request, &inherited.context, 200);
    const auto& inherited_binding = inherited.context.binding;
    inherited_lifecycle.OnUpdate({KnifeUpdatePhase::BeforeWeapon,
        inherited_binding.arms.address, inherited_binding.weapon_graph_controller,
        inherited_binding.world.address}, 200);
    Check(inherited_lifecycle.Owns(inherited_request, inherited.context.binding) &&
        inherited.calls[0] == 0 && inherited.calls[1] == 0 && inherited.calls[2] == 0 &&
        inherited.events == std::vector<int>{3},
        "native target M9 HUD/skeleton skips SetModel and recollection, changing only weapon_type");
    std::cout << "knife_prearmed_atomic_handoff=PASS; native_game_calls=0\n";
}

void NativeHudHandoff()
{
    NativeFixture f;
    KnifeNativePairAccess port(f.context, f);
    KnifePairAdapter pair(port);
    const auto* target = FindKnifeModelAsset(508);
    Check(pair.Capture(f.context.binding) && pair.PrepareWorld(f.context.binding, *target), "prepare only the owned world");
    const auto calls = f.calls;
    f.ReplaceHud();
    for (const auto presence : {KnifeBladePresence::Live, KnifeBladePresence::Unreadable})
    {
        f.old_hud_presence = presence;
        Check(!port.RebindPreparedHud(f.context), "readable or unproven old HUD must not be adopted as gone");
    }
    f.old_hud_presence = KnifeBladePresence::Gone;
    for (int mutation = 0; mutation < 10; ++mutation)
    {
        auto bad = f.context;
        switch (mutation)
        {
        case 0: ++bad.binding.scene_epoch; break;
        case 1: ++bad.binding.xuid; break;
        case 2: ++bad.binding.world.handle; break;
        case 3: ++bad.binding.pawn.handle; break;
        case 4: ++bad.binding.arms.address; break;
        case 5: ++bad.binding.body_animation_controller; break;
        case 6: ++bad.resources.world; break;
        case 7: ++bad.symbols.target_type; break;
        case 8: ++bad.hud_body_controller; break;
        case 9: ++bad.model_names[1]; break;
        }
        Check(!port.RebindPreparedHud(bad), "unrelated identity, resource, body or symbol rejects handoff");
    }
    f.world_alive = false;
    Check(!port.RebindPreparedHud(f.context), "inactive or unowned prepared world cannot adopt a HUD");
    f.world_alive = true;
    Check(f.calls == calls && port.RebindPreparedHud(f.context) &&
        pair.CompletePreparedWorld(f.context.binding, *target) && pair.VerifyPair(f.context.binding, *target),
        "rejected handoffs preserve original world ledger for the correct successor");
    Check(f.calls[0] == 1 && port.RetireDetached() == KnifeRetireResult::Restored && f.calls[0] == 2,
        "adopted HUD cleanup still restores the original source world exactly once");
    std::cout << "knife_hud_handoff_guards=PASS; native_game_calls=0\n";
}

void NativePairedPort()
{
    NativeFixture f;
    KnifeNativePairAccess port(f.context, f);
    KnifePairAdapter pair(port);
    auto binding = f.context.binding;
    KnifePairState original, actual;
    const auto memory_before = f.f.memory.bytes;
    Check(port.EvidenceVerified() && port.Read(binding, original) && original.attachments_valid &&
        original.secondary_matches_hud && original.world_definition == 525, "native port reads actual model resources/paths/graph");
    Check(memory_before == f.f.memory.bytes && f.events.empty(), "native port inspection remains read-only");
    f.safe = false;
    Check(!pair.Capture(binding) && !port.SetBladeModel(binding, KnifeBladeRole::World, *FindKnifeModelAsset(508)) &&
        !port.Recollect(binding) && !port.CompareAnimationType(binding, f.context.symbols.source_type, f.context.symbols.target_type) &&
        f.events.empty(), "a valid native snapshot does not authorize writes without a lifecycle safe point");
    f.safe = true;
    Check(pair.Capture(binding) && pair.ApplyPair(binding, *FindKnifeModelAsset(508), binding) &&
        pair.VerifyPair(binding, *FindKnifeModelAsset(508)), "real paired port commits fixture models, skeleton and ID node");
    Check(f.events == std::vector<int>{4,0,1,2,3},
        "native port invokes exact world-item/world/HUD/recollect/ID order");
    const auto name_reads_before = f.model_name_reads;
    for (int i = 0; i < 128; ++i)
    {
        f.Set<std::uintptr_t>(0x80018, f.context.symbols.source_type);
        Check(pair.AfterNativeUpdate({123, 508}, binding), "native source reset is maintained on same proven node");
    }
    Check(f.calls[0] == 1 && f.calls[1] == 1 && f.calls[2] == 1,
        "native port animation maintenance does not reload model resources");
    Check(f.model_name_reads == name_reads_before, "native maintenance avoids per-frame model path reads");
    Check(pair.Restore(binding) && port.Read(binding, actual) && actual == original,
        "native port restores original canonical models and skeleton without item/finish snapshot");
    for (int event = 0; event < 5; ++event)
    {
        for (bool changed : {false, true})
        {
            NativeFixture bad;
            KnifeNativePairAccess access(bad.context, bad); KnifePairAdapter adapter(access);
            auto b = bad.context.binding;
            Check(access.Read(b, original) && adapter.Capture(b), "native failure capture");
            bad.fail_event = event; bad.fail_after_change = changed;
            Check(!adapter.ApplyPair(b, *FindKnifeModelAsset(508), b), "native primitive failure rejected");
            Check(adapter.Restore(b) && access.Read(b, actual) && actual == original, "native partial commit restored");
        }
    }
    for (int change = 0; change < 11; ++change)
    {
        NativeFixture bad;
        switch (change)
        {
        case 0: bad.alive = false; break;
        case 1: bad.attached = false; break;
        case 2: bad.Name(0, FindKnifeModelAsset(507)->model); break;
        case 3: bad.Name(2, FindKnifeModelAsset(508)->model); break;
        case 4: bad.Set<std::uintptr_t>(0x80018, 0xBAD); break;
        case 5: bad.Set<std::uint32_t>(0xB1000, bad.context.binding.hud_weapon.handle + 0x8000); break;
        case 6: bad.Set<std::uintptr_t>(0x60450, 0xBAD); break;
        case 7: bad.context.model_names[0] += 8; break;
        case 8: bad.context.resources.arms = bad.context.resources.world; break;
        case 9: bad.context.symbols.target_type = 0; break;
        case 10: bad.context.binding.scene_epoch++; break;
        }
        KnifeNativePairAccess access(bad.context, bad); KnifePairAdapter adapter(access);
        Check(!adapter.Capture(bad.context.binding) && bad.events.empty(), "native ownership/symbol/path rejection before writes");
    }
    NativeFixture corrupt;
    KnifeNativePairAccess reject(corrupt.context, corrupt);
    corrupt.Set<std::uint8_t>(corrupt.context.client + kKnifeSetModelRva, 0x90);
    Check(!reject.EvidenceVerified() && !reject.SetBladeModel(corrupt.context.binding, KnifeBladeRole::World,
        *FindKnifeModelAsset(508)) && corrupt.events.empty(), "unverified native call bytes cannot execute");
    NativeFixture competitor;
    KnifeNativePairAccess contested(competitor.context, competitor);
    competitor.Set<std::uintptr_t>(0x80018, competitor.context.symbols.target_type);
    Check(!contested.CompareAnimationType(competitor.context.binding, competitor.context.symbols.source_type,
        competitor.context.symbols.target_type) && competitor.events.empty(), "mismatched expected parameter is not overwritten");
    std::cout << "knife_native_port_fixture=PASS; native_game_calls=0; live_model_test=0\n";
}

void NativeRetirement()
{
    const auto apply = [](NativeFixture& f, KnifePairAdapter& pair)
    {
        auto binding = f.context.binding;
        Check(pair.Capture(binding) && pair.ApplyPair(binding, *FindKnifeModelAsset(508), binding), "retirement setup");
    };
    for (int scenario = 0; scenario < 9; ++scenario)
    {
        NativeFixture f; KnifeNativePairAccess access(f.context, f); KnifePairAdapter pair(access);
        apply(f, pair);
        if (scenario == 1) { f.alive = false; f.presence[1] = KnifeBladePresence::Gone; f.retirement_graph = KnifeRetirementGraph::Absent; }
        if (scenario == 2) { f.alive = false; f.presence[0] = KnifeBladePresence::Gone; f.f.memory.bytes.erase(0x21000); }
        if (scenario == 3) f.NewGraph();
        if (scenario == 4) { f.presence[0] = KnifeBladePresence::Replaced; f.retirement_graph = KnifeRetirementGraph::Absent; }
        if (scenario == 5) { f.Name(0, FindKnifeModelAsset(507)->model); }
        if (scenario == 6) { f.retirement_graph = KnifeRetirementGraph::Absent; f.Set<std::uintptr_t>(0x80018, 0xDEAD); }
        if (scenario == 7) f.Set<std::uintptr_t>(0x80018, 0xDEAD);
        if (scenario == 8) { f.Set<std::uintptr_t>(0xE0000, 0xF9000); }
        const auto result = access.RetireDetached();
        const bool discarded = scenario == 1 || scenario == 2 || scenario == 4 || scenario == 5 || scenario == 8;
        Check(result == (discarded ? KnifeRetireResult::Discarded : KnifeRetireResult::Restored), "detached lifecycle restores only still-owned blades");
        Check(f.calls[0] == (scenario == 2 || scenario == 4 || scenario == 5 || scenario == 8 ? 1 : 2) &&
            f.calls[1] == (scenario == 1 ? 1 : 2), "detached cleanup independent world/HUD calls");
        Check(f.calls[2] == (scenario == 1 || scenario == 4 || scenario == 6 ? 1 : 2), "recollect only an existing HUD graph registration");
        Check(f.calls[3] == (scenario == 1 || scenario == 4 || scenario == 6 || scenario == 7 ? 1 : 2), "native foreign animation branch preserved");
        const auto calls = f.calls;
        Check(access.RetireDetached() == result && calls == f.calls, "completed retirement is idempotent");
    }
    for (int scenario = 0; scenario < 5; ++scenario)
    {
        NativeFixture f; KnifeNativePairAccess access(f.context, f); KnifePairAdapter pair(access);
        apply(f, pair);
        if (scenario == 0) f.presence[1] = KnifeBladePresence::Unreadable;
        if (scenario == 1) f.retirement_graph = KnifeRetirementGraph::Unreadable;
        if (scenario == 2) f.fail_event = 0;
        if (scenario == 3) f.fail_event = 2;
        if (scenario == 4) f.fail_event = 3;
        Check(access.RetireDetached() == KnifeRetireResult::Failed,
            "unreadable/failing retirement is not reported as gone or restored");
        Check(f.calls[0] == 2, "HUD failure does not prevent world restoration attempt");
        const auto calls = f.calls;
        for (int i = 0; i < 128; ++i) Check(access.RetireDetached() == KnifeRetireResult::Failed, "failed cleanup latched");
        Check(calls == f.calls, "failed cleanup never repeats native writes per frame");
        ++f.epoch;
        const auto reads = f.f.memory.reads;
        Check(access.RetireDetached() == KnifeRetireResult::Discarded && reads == f.f.memory.reads && calls == f.calls,
            "new epoch discards even a failed completed-role ledger without old pointer reads");
    }
    NativeFixture f; KnifeNativePairAccess access(f.context, f); KnifePairAdapter pair(access);
    apply(f, pair);
    f.retirement_safe = false;
    Check(access.RetireDetached() == KnifeRetireResult::Waiting && f.calls[0] == 1, "no restoration without retirement lease");
    f.retirement_safe = true;
    f.after_set = [&](int role) { if (role == 0) f.retirement_safe = false; };
    Check(access.RetireDetached() == KnifeRetireResult::Waiting && f.calls[0] == 2 && f.calls[1] == 1,
        "lease expiration stops between blades");
    f.after_set = {}; f.retirement_safe = true;
    Check(access.RetireDetached() == KnifeRetireResult::Restored && f.calls[0] == 2 && f.calls[1] == 2 && f.calls[2] == 2,
        "resuming retirement retains graph-dirty state without repeating restored model");

    NativeFixture transferred;
    KnifeNativePairAccess transferred_access(transferred.context, transferred);
    KnifePairAdapter transferred_pair(transferred_access);
    apply(transferred, transferred_pair);
    transferred.world_ownership = KnifeWorldOwnership::Transferred;
    Check(transferred_access.RetireDetached() == KnifeRetireResult::Discarded,
        "dropped world knife discards the old player's definition/model restore ledger");
    Check(transferred.item_definition == transferred.context.target_definition &&
        transferred.calls[0] == 1 && transferred.calls[4] == 1 &&
        transferred.calls[1] == 2 && transferred.calls[2] == 2 && transferred.calls[3] == 2,
        "drop retirement never writes world definition/model into the new owner but still cleans the old HUD graph");
    const auto transferred_calls = transferred.calls;
    Check(transferred_access.RetireDetached() == KnifeRetireResult::Discarded &&
        transferred.calls == transferred_calls,
        "transferred-world discard is idempotent");

    NativeFixture unreadable_owner;
    KnifeNativePairAccess unreadable_access(unreadable_owner.context, unreadable_owner);
    KnifePairAdapter unreadable_pair(unreadable_access);
    apply(unreadable_owner, unreadable_pair);
    const auto before_unreadable = unreadable_owner.calls;
    unreadable_owner.world_ownership = KnifeWorldOwnership::Unreadable;
    Check(unreadable_access.RetireDetached() == KnifeRetireResult::Waiting &&
        unreadable_owner.calls == before_unreadable,
        "temporarily unreadable ownership waits without any model/definition write");
    unreadable_owner.world_ownership = KnifeWorldOwnership::Owned;
    Check(unreadable_access.RetireDetached() == KnifeRetireResult::Restored,
        "ownership proof can recover and finish retirement on a later boundary");
    std::cout << "knife_retirement_fixture=PASS; native_game_calls=0\n";
}

void NativeLifecycle()
{
    const KnifeModelRequest request {123, 508};
    const auto event = [](const NativeFixture& f, KnifeUpdatePhase phase)
    { return KnifeUpdateEvent {phase, f.f.binding.arms.address, f.f.binding.weapon_graph_controller, f.f.binding.world.address}; };
    NativeFixture f; KnifeLifecycle session(f);
    session.Stage(request, &f.context, 100);
    auto wrong = event(f, KnifeUpdatePhase::AfterWeapon); ++wrong.world;
    session.OnUpdate(wrong, 101);
    session.OnUpdate(event(f, KnifeUpdatePhase::AfterArms), 101);
    Check(f.events.empty(), "wrong native callback and after-evaluation callback never commit");
    session.OnUpdate(event(f, KnifeUpdatePhase::BeforeArms), 102);
    session.OnUpdate(event(f, KnifeUpdatePhase::AfterWeapon), 102);
    Check(session.Phase() == KnifeModelPhase::WaitingHudRebuild && f.events == std::vector<int>{4,0} &&
        session.Statistics().applied == 0, "world change must not commit the HUD it has queued for deletion");
    for (int i = 0; i < 4; ++i)
    {
        session.OnUpdate(event(f, KnifeUpdatePhase::BeforeArms), 103 + i);
        session.OnUpdate(event(f, KnifeUpdatePhase::AfterWeapon), 103 + i);
    }
    Check(f.events == std::vector<int>{4,0}, "old HUD still readable is not treated as a replacement");
    f.alive = false; f.presence[1] = KnifeBladePresence::Gone;
    session.Stage(request, nullptr, 108);
    session.OnUpdate(event(f, KnifeUpdatePhase::BeforeArms), 108);
    Check(session.HasTransaction() && f.calls[0] == 1, "expected HUD deletion preserves the prepared world model");
    f.ReplaceHud();
    session.Stage(request, &f.context, 109);
    session.OnUpdate(event(f, KnifeUpdatePhase::BeforeArms), 109);
    session.OnUpdate(event(f, KnifeUpdatePhase::AfterWeapon), 109);
    Check(session.Phase() == KnifeModelPhase::Applied && session.Owns(request, f.context.binding), "fresh matching native callback commits paired model");
    Check(session.Statistics().hud_handoffs == 1 && f.events == std::vector<int>{4,0,1,2,3},
        "handoff synchronizes the HUD model and never repeats world SetModel");
    for (int i = 0; i < 128; ++i)
    {
        session.Stage(request, &f.context, 110 + i);
        session.OnUpdate(event(f, KnifeUpdatePhase::BeforeArms), 110 + i);
        f.Set<std::uintptr_t>(0x80018, f.context.symbols.source_type);
        session.OnUpdate(event(f, KnifeUpdatePhase::AfterWeapon), 110 + i);
    }
    Check(session.Statistics().attempts == 1 && session.Statistics().maintenance == 128 && f.calls[0] == 1 && f.calls[2] == 1,
        "same model request, including unrelated finish edits, does not reload model");

    {
        NativeFixture rebuilt_generation;
        KnifeLifecycle rebuilt_session(rebuilt_generation);
        rebuilt_session.Stage(request, &rebuilt_generation.context, 200);
        rebuilt_session.OnUpdate(event(rebuilt_generation, KnifeUpdatePhase::BeforeArms), 200);
        rebuilt_session.OnUpdate(event(rebuilt_generation, KnifeUpdatePhase::AfterWeapon), 200);
        Check(rebuilt_session.HasTransaction() &&
            rebuilt_session.Phase() == KnifeModelPhase::WaitingHudRebuild,
            "entity-rebuild fixture owns a pointer-bearing pending transaction before reset");
        const auto calls_before_rebuild = rebuilt_generation.calls;
        rebuilt_session.ResetForEntityRebuild();
        Check(!rebuilt_session.HasTransaction() &&
            rebuilt_session.Active() == nullptr &&
            rebuilt_session.Phase() == KnifeModelPhase::WaitingBinding &&
            rebuilt_generation.calls == calls_before_rebuild,
            "entity rebuild abandons stale knife pointers without writing to retiring entities");
    }

    session.Request({});
    session.OnUpdate(event(f, KnifeUpdatePhase::BeforeArms), 250);
    Check(!session.HasTransaction() && session.Phase() == KnifeModelPhase::Disabled && session.Statistics().restored == 1,
        "disabled model restores before native update without an old-knife inner callback");
    session.Stage(request, &f.context, 251);
    session.OnUpdate(event(f, KnifeUpdatePhase::AfterWeapon), 251);
    Check(session.Statistics().attempts == 1, "retirement and new model load never share one boundary");
    session.OnUpdate(event(f, KnifeUpdatePhase::BeforeArms), 252);
    session.OnUpdate(event(f, KnifeUpdatePhase::AfterWeapon), 252);
    f.ReplaceHud(); session.Stage(request, &f.context, 253);
    session.OnUpdate(event(f, KnifeUpdatePhase::BeforeArms), 253);
    session.OnUpdate(event(f, KnifeUpdatePhase::AfterWeapon), 253);
    Check(session.Statistics().attempts == 2 && session.Phase() == KnifeModelPhase::Applied, "next fresh boundary can reapply");
    f.alive = false; f.presence[1] = KnifeBladePresence::Gone; f.retirement_graph = KnifeRetirementGraph::Absent;
    session.OnUpdate(event(f, KnifeUpdatePhase::BeforeArms), 260);
    Check(!session.HasTransaction() && session.Statistics().discarded == 1 && f.calls[0] == 4 && f.calls[1] == 3,
        "holster restores surviving world even though entire captured binding is no longer alive");
    for (int fail = 0; fail < 4; ++fail)
    {
        NativeFixture bad; KnifeLifecycle failed(bad);
        bad.fail_event = fail; bad.fail_after_change = true;
        failed.Stage(request, &bad.context, 100);
        failed.OnUpdate(event(bad, KnifeUpdatePhase::BeforeArms), 100);
        failed.OnUpdate(event(bad, KnifeUpdatePhase::AfterWeapon), 100);
        if (fail != 0)
        {
            bad.ReplaceHud(); failed.Stage(request, &bad.context, 101);
            failed.OnUpdate(event(bad, KnifeUpdatePhase::BeforeArms), 101);
            failed.OnUpdate(event(bad, KnifeUpdatePhase::AfterWeapon), 101);
        }
        Check(failed.Phase() == KnifeModelPhase::Faulted && failed.Statistics().applied == 0 && !failed.HasTransaction(), "partial model failure rolled back by lifecycle");
        const auto calls = bad.calls;
        for (int i = 0; i < 128; ++i)
        {
            failed.Stage(request, &bad.context, 101 + i);
            failed.OnUpdate(event(bad, KnifeUpdatePhase::BeforeArms), 101 + i);
            failed.OnUpdate(event(bad, KnifeUpdatePhase::AfterWeapon), 101 + i);
        }
        Check(bad.calls == calls && failed.Statistics().attempts == 1, "same failed binding is not retried per frame/poll");
        ++bad.epoch; ++bad.context.binding.scene_epoch; ++bad.f.binding.scene_epoch;
        failed.Stage(request, &bad.context, 300);
        failed.OnUpdate(event(bad, KnifeUpdatePhase::BeforeArms), 300);
        failed.OnUpdate(event(bad, KnifeUpdatePhase::AfterWeapon), 300);
        bad.ReplaceHud(); failed.Stage(request, &bad.context, 301);
        failed.OnUpdate(event(bad, KnifeUpdatePhase::BeforeArms), 301);
        failed.OnUpdate(event(bad, KnifeUpdatePhase::AfterWeapon), 301);
        Check(failed.Phase() == KnifeModelPhase::Applied, "new scene identity permits a fresh verified attempt");
    }
    NativeFixture stale; KnifeLifecycle expired(stale);
    expired.Stage(request, &stale.context, 100);
    expired.OnUpdate(event(stale, KnifeUpdatePhase::AfterWeapon), 1101);
    Check(stale.events.empty(), "expired discovery snapshot cannot enable model writes");
    for (bool inherited : {false, true})
    {
        NativeFixture rebuilt; KnifeLifecycle next(rebuilt);
        next.Stage(request, &rebuilt.context, 100);
        next.OnUpdate(event(rebuilt, KnifeUpdatePhase::AfterWeapon), 100);
        rebuilt.NewGraph(); rebuilt.ReplaceHud(inherited);
        next.Stage(request, &rebuilt.context, 101);
        next.OnUpdate(event(rebuilt, KnifeUpdatePhase::BeforeArms), 101);
        next.OnUpdate(event(rebuilt, KnifeUpdatePhase::AfterWeapon), 101);
        Check(next.Phase() == KnifeModelPhase::Applied && rebuilt.calls[0] == 1 && rebuilt.calls[1] == (inherited ? 0 : 1),
            "replacement HUD and rebuilt graph are adopted without resetting world model");
        next.Request({}); next.OnUpdate(event(rebuilt, KnifeUpdatePhase::BeforeArms), 102);
        Check(!next.HasTransaction() && rebuilt.calls[0] == 2 && rebuilt.calls[1] == (inherited ? 1 : 2),
            "native-inherited target HUD is restored even without an explicit HUD apply call");
    }
    NativeFixture delayed; KnifeLifecycle paused(delayed);
    paused.Stage(request, &delayed.context, 100);
    paused.OnUpdate(event(delayed, KnifeUpdatePhase::AfterWeapon), 100);
    paused.OnUpdate(event(delayed, KnifeUpdatePhase::AfterWeapon), 10100);
    Check(paused.Phase() == KnifeModelPhase::WaitingHudRebuild && delayed.calls[0] == 1,
        "wall-clock pause without native updates cannot fault a pending HUD handoff");
    delayed.ReplaceHud(); paused.Stage(request, &delayed.context, 10101);
    paused.OnUpdate(event(delayed, KnifeUpdatePhase::AfterWeapon), 10101);
    Check(paused.Phase() == KnifeModelPhase::Applied && paused.Statistics().faults == 0,
        "late dropped-knife HUD is adopted on the next verified callback");

    NativeFixture timeout; KnifeLifecycle waiting(timeout);
    std::uint64_t base = 100;
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        waiting.Stage(request, &timeout.context, base);
        waiting.OnUpdate(event(timeout, KnifeUpdatePhase::BeforeArms), base);
        waiting.OnUpdate(event(timeout, KnifeUpdatePhase::AfterWeapon), base);
        for (int i = 0; i < 32; ++i)
            waiting.OnUpdate(event(timeout, KnifeUpdatePhase::AfterWeapon), base + 4000 + i);
        if (attempt < 2)
        {
            Check(waiting.Phase() == KnifeModelPhase::WaitingBinding &&
                waiting.Statistics().handoff_retries == static_cast<std::uint64_t>(attempt + 1),
                "missing successor gets only a bounded recoverable retry");
            base += 5000;
        }
    }
    Check(waiting.Phase() == KnifeModelPhase::Faulted && waiting.Statistics().attempts == 3 &&
        waiting.Statistics().handoff_retries == 2 && waiting.Statistics().faults == 1 &&
        timeout.calls[0] == 6 && timeout.calls[1] == 0,
        "persistently missing successor stops after two recoveries and restores only the touched world");
    const auto calls = timeout.calls;
    timeout.ReplaceHud();
    for (int i = 0; i < 128; ++i)
    {
        waiting.Stage(request, &timeout.context, base + 5000 + i);
        waiting.OnUpdate(event(timeout, KnifeUpdatePhase::BeforeArms), base + 5000 + i);
        waiting.OnUpdate(event(timeout, KnifeUpdatePhase::AfterWeapon), base + 5000 + i);
    }
    Check(calls == timeout.calls, "hard-failed handoff does not restart a world/HUD destruction loop on every new HUD");
    for (bool disabled : {false, true})
    {
        NativeFixture interrupted; KnifeLifecycle pending(interrupted);
        pending.Stage(request, &interrupted.context, 100);
        pending.OnUpdate(event(interrupted, KnifeUpdatePhase::AfterWeapon), 100);
        if (disabled) pending.Request({});
        else interrupted.world_alive = false;
        interrupted.alive = false; interrupted.retirement_graph = KnifeRetirementGraph::Absent;
        pending.OnUpdate(event(interrupted, KnifeUpdatePhase::BeforeArms), 101);
        Check(!pending.HasTransaction() && interrupted.calls[0] == 2 && interrupted.calls[1] == 0,
            "disable or holster during handoff restores world without touching the retired HUD");
    }
    std::cout << "knife_lifecycle_fixture=PASS; native_game_calls=0; live_animation_test=0\n";
}

std::vector<std::uint8_t> ReadPeRva(const wchar_t* path, std::uintptr_t rva, std::size_t size)
{
    std::ifstream file(std::filesystem::path(path), std::ios::binary);
    const auto read = [&](auto& object)
    {
        file.read(reinterpret_cast<char*>(&object), sizeof(object));
        Check(static_cast<bool>(file), "cannot read PE header");
    };
    IMAGE_DOS_HEADER dos {}; read(dos);
    Check(dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew > 0, "DOS header");
    file.seekg(dos.e_lfanew);
    IMAGE_NT_HEADERS64 nt {}; read(nt);
    Check(nt.Signature == IMAGE_NT_SIGNATURE && nt.FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
        nt.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC, "x64 PE required");
    file.seekg(dos.e_lfanew + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) + nt.FileHeader.SizeOfOptionalHeader);
    for (WORD i = 0; i < nt.FileHeader.NumberOfSections; ++i)
    {
        IMAGE_SECTION_HEADER section {}; read(section);
        if (rva < section.VirtualAddress || rva - section.VirtualAddress > section.SizeOfRawData ||
            size > section.SizeOfRawData - (rva - section.VirtualAddress)) { continue; }
        Check((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0, "evidence must be executable code");
        file.seekg(section.PointerToRawData + rva - section.VirtualAddress);
        std::vector<std::uint8_t> bytes(size);
        file.read(reinterpret_cast<char*>(bytes.data()), size);
        Check(static_cast<bool>(file), "cannot read evidence bytes");
        return bytes;
    }
    throw std::runtime_error("evidence RVA not in PE section");
}

std::array<const void*, 6> serialization_arguments {};
int serialization_cached_calls = 0, serialization_local_calls = 0;
void* __fastcall CachedSerializationFixture(void* manager, void* output, const void* graph,
    const void* children, const void* sets, const void* skeletons)
{
    ++serialization_cached_calls;
    serialization_arguments = {manager, output, graph, children, sets, skeletons};
    return output;
}
void* __fastcall LocalSerializationFixture(void* output, const void* graph,
    const void* children, const void* sets, const void* skeletons)
{
    ++serialization_local_calls;
    serialization_arguments = {nullptr, output, graph, children, sets, skeletons};
    return output;
}

void KnifeSerializationRouting()
{
    std::uint32_t all = 0;
    for (const auto& changed : KnifeModelAssets())
    {
        const auto bit = KnifeSerializationModelBit(changed.definition);
        Check(bit && !(all & bit), "each knife needs a unique serialization scope");
        all |= bit;
        for (const auto& queried : KnifeModelAssets())
        {
            Check(NeedsLocalKnifeSerialization(true, bit, queried.skeleton, false) ==
                (queried.definition == changed.definition), "only modified skeleton with absent demo row uses local context");
            Check(!NeedsLocalKnifeSerialization(true, bit, queried.skeleton, true), "recorded skeleton remains native cached");
            Check(!NeedsLocalKnifeSerialization(false, bit, queried.skeleton, false), "no online serialization routing");
        }
    }
    Check(!KnifeSerializationModelBit(524) && !KnifeSerializationModelBit(0), "unsupported models cannot arm bridge");
    Check(!NeedsLocalKnifeSerialization(true, 0, KnifeModelAssets()[0].skeleton, false), "unmodified state unchanged");
    for (const auto* path : {"", "animation/skeletons/weapons/weapon_ak47.vnmskel",
             "animation/skeletons/arms.vnmskel", "../knife_karambit.vnmskel",
             "animation/skeletons/weapons/knife_karambit.vnmskel.extra"})
        Check(!NeedsLocalKnifeSerialization(true, all, path, false), "unknown/arms/gun paths outside knife bridge");
    int storage[6] {};
    auto* output = &storage[1];
    Check(DispatchKnifeSerialization(false, CachedSerializationFixture, LocalSerializationFixture,
        &storage[0], output, &storage[2], &storage[3], &storage[4], &storage[5]) == output &&
        serialization_arguments == std::array<const void*, 6>{&storage[0], output, &storage[2], &storage[3], &storage[4], &storage[5]} &&
        serialization_cached_calls == 1 && serialization_local_calls == 0, "recorded ABI forwards all six original arguments");
    Check(DispatchKnifeSerialization(true, CachedSerializationFixture, LocalSerializationFixture,
        &storage[0], output, &storage[2], &storage[3], &storage[4], &storage[5]) == output &&
        serialization_arguments == std::array<const void*, 6>{nullptr, output, &storage[2], &storage[3], &storage[4], &storage[5]} &&
        serialization_cached_calls == 1 && serialization_local_calls == 1, "local ABI shifts arguments without changing output/graph/skeletons");
    Memory memory;
    constexpr std::uintptr_t base = 0x10000000, size = 0x3000000;
    for (const auto& window : KnifeSerializationEvidenceWindows())
        for (std::size_t i = 0; i < window.bytes.size(); ++i) memory.Set(base + window.rva + i, window.bytes[i]);
    Check(VerifyKnifeSerializationEvidence(base, size, memory), "serialization evidence accepts exact fixtures");
    for (const auto& window : KnifeSerializationEvidenceWindows())
    {
        for (std::size_t i = 0; i < window.bytes.size(); ++i)
        {
            auto& byte = memory.bytes.at(base + window.rva + i);
            byte ^= 1;
            Check(!VerifyKnifeSerializationEvidence(base, size, memory), "all changed cache/local/table ABI bytes rejected");
            byte ^= 1;
        }
        const auto address = base + window.rva + window.bytes.size() - 1;
        const auto saved = memory.bytes.at(address);
        memory.bytes.erase(address);
        Check(!VerifyKnifeSerializationEvidence(base, size, memory), "truncated serialization ABI rejected");
        memory.Set(address, saved);
    }
    Check(!VerifyKnifeSerializationEvidence(base, 0x2239700, memory) &&
        !VerifyKnifeSerializationEvidence(std::numeric_limits<std::uintptr_t>::max()-4, size, memory),
        "serialization global pointer and image bounds checked");
    std::cout << "knife_serialization_routing=PASS; 484 scoped pairs; offline_only=1; native_arguments_preserved=1\n";
}

void InstalledBinaryEvidence()
{
    const auto* client = _wgetenv(L"NSKINZCS2_KNIFE_CLIENT_DLL");
    const auto* animation = _wgetenv(L"NSKINZCS2_KNIFE_ANIMATION_DLL");
    if (client == nullptr && animation == nullptr)
    { std::cout << "knife_graph_installed_evidence=SKIPPED (set both PE paths)\n"; return; }
    Check(client != nullptr && animation != nullptr, "both binary paths required");
    Memory resources;
    constexpr std::uintptr_t image_base = 0x10000000, image_size = 0x3000000;
    for (const auto& window : KnifeSerializationEvidenceWindows())
    {
        const auto bytes = ReadPeRva(client, window.rva, window.bytes.size());
        for (std::size_t i = 0; i < bytes.size(); ++i) resources.Set(image_base + window.rva + i, bytes[i]);
    }
    Check(VerifyKnifeSerializationEvidence(image_base, image_size, resources), "installed recorded/local animation serialization ABI");
    for (const auto& window : KnifeFinishEvidenceWindows())
    {
        const auto bytes = ReadPeRva(client, window.rva, window.bytes.size());
        for (std::size_t i = 0; i < bytes.size(); ++i) resources.Set(image_base + window.rva + i, bytes[i]);
    }
    Check(VerifyKnifeFinishEvidence(image_base, image_size, resources), "actual target-model generator and native HUD material transfer ABI");
    for (const auto& window : KnifeFinishEvidenceWindows())
    {
        for (std::size_t i = 0; i < window.bytes.size(); ++i)
        {
            auto& byte = resources.bytes.at(image_base + window.rva + i);
            byte ^= 1;
            Check(!VerifyKnifeFinishEvidence(image_base, image_size, resources), "changed finish/transfer instruction rejected");
            byte ^= 1;
        }
        const auto address = image_base + window.rva + window.bytes.size() - 1;
        const auto saved = resources.bytes.at(address);
        resources.bytes.erase(address);
        Check(!VerifyKnifeFinishEvidence(image_base, image_size, resources), "truncated material ABI rejected");
        resources.Set(address, saved);
    }
    for (const auto& window : KnifeNativeEvidenceWindows())
    {
        const auto bytes = ReadPeRva(client, window.rva, window.bytes.size());
        for (std::size_t i = 0; i < bytes.size(); ++i) resources.Set(image_base + window.rva + i, bytes[i]);
    }
    Check(VerifyKnifeNativeEvidence(image_base, image_size, resources), "actual native SetModel/notify/recollect ABI evidence");
    for (const auto& window : KnifeNativeEvidenceWindows())
    {
        for (std::size_t i = 0; i < window.bytes.size(); ++i)
        {
            auto& byte = resources.bytes.at(image_base + window.rva + i);
            byte ^= 1;
            Check(!VerifyKnifeNativeEvidence(image_base, image_size, resources), "changed native call instruction rejected");
            byte ^= 1;
        }
        const auto last = image_base + window.rva + window.bytes.size() - 1;
        const auto saved = resources.bytes.at(last);
        resources.bytes.erase(last);
        Check(!VerifyKnifeNativeEvidence(image_base, image_size, resources), "truncated native call instruction rejected");
        resources.Set(last, saved);
    }
    for (const auto& window : KnifeResourceEvidenceWindows())
    {
        const auto bytes = ReadPeRva(client, window.rva, window.bytes.size());
        for (std::size_t i = 0; i < bytes.size(); ++i) resources.Set(image_base + window.rva + i, bytes[i]);
    }
    Check(VerifyKnifeResourceEvidence(image_base, image_size, resources), "actual model/graph resource layout evidence");
    for (const auto& window : KnifeResourceEvidenceWindows())
    {
        for (std::size_t i = 0; i < window.bytes.size(); ++i)
        {
            auto& byte = resources.bytes.at(image_base + window.rva + i);
            byte ^= 1;
            Check(!VerifyKnifeResourceEvidence(image_base, image_size, resources), "changed resource instructions rejected");
            byte ^= 1;
        }
        const auto last_address = image_base + window.rva + window.bytes.size() - 1;
        const auto saved = resources.bytes.at(last_address);
        resources.bytes.erase(last_address);
        Check(!VerifyKnifeResourceEvidence(image_base, image_size, resources), "truncated resource instructions rejected");
        resources.Set(last_address, saved);
    }
    Check(!VerifyKnifeResourceEvidence(image_base, 0x1000, resources) &&
        !VerifyKnifeResourceEvidence(std::numeric_limits<std::uintptr_t>::max() - 4, image_size, resources),
        "invalid module bounds reject resource evidence");
    auto timing_entry = ReadPeRva(client, kKnifeNativeUpdateRva, 19);
    auto timing_call1 = ReadPeRva(client, kKnifeNativeUpdateCall1Rva, 11);
    auto timing_call2 = ReadPeRva(client, kKnifeNativeUpdateCall2Rva, 11);
    auto timing_evaluation = ReadPeRva(client, kKnifeNativeGraphEvaluationCallRva, 5);
    const auto timing_matches = [&] { return MatchesKnifeUpdateCallEvidence(timing_entry, timing_call1, timing_call2, timing_evaluation); };
    Check(timing_matches(), "installed native updater entry + both callers + later graph evaluation");
    auto outer_entry = ReadPeRva(client, kKnifeArmsUpdateRva, 12);
    auto outer_call = ReadPeRva(client, kKnifeArmsUpdateCallRva, 5);
    const auto outer_matches = [&] { return MatchesKnifeArmsUpdateEvidence(outer_entry, outer_call); };
    Check(outer_matches(), "installed native arms lifecycle scope entry and caller");
    for (auto* bytes : {&outer_entry, &outer_call})
    {
        for (auto& byte : *bytes)
        { byte ^= 1; Check(!outer_matches(), "changed lifecycle scope instruction rejected"); byte ^= 1; }
        const auto last = bytes->back(); bytes->pop_back();
        Check(!outer_matches(), "truncated lifecycle scope instruction rejected"); bytes->push_back(last);
    }
    for (auto* bytes : {&timing_entry, &timing_call1, &timing_call2, &timing_evaluation})
    {
        for (auto& byte : *bytes)
        {
            byte ^= 1;
            Check(!timing_matches(), "every changed update-call evidence byte rejected");
            byte ^= 1;
        }
        const auto last = bytes->back();
        bytes->pop_back();
        Check(!timing_matches(), "every truncated update-call window rejected");
        bytes->push_back(last);
    }
    auto update = ReadPeRva(client, kKnifeWeaponTypeUpdateRva, kKnifeWeaponTypeUpdateSize);
    auto destruction = ReadPeRva(animation, kKnifeGraphNodeDestructionRva, kKnifeGraphNodeDestructionSize);
    auto construction = ReadPeRva(animation, kKnifeIdNodeConstructionRva, kKnifeIdNodeConstructionSize);
    auto body_construction = ReadPeRva(client, kKnifeBodyGraphConstructionRva, kKnifeBodyGraphConstructionSize);
    auto body_use = ReadPeRva(client, kKnifeBodyGraphUseRva, kKnifeBodyGraphUseSize);
    auto secondary_handles = ReadPeRva(client, kKnifeSecondaryHandleAppendRva, kKnifeSecondaryHandleAppendSize);
    auto secondary_slots = ReadPeRva(client, kKnifeSecondarySlotAppendRva, kKnifeSecondarySlotAppendSize);
    Check(MatchesKnifeSecondaryLayoutEvidence(secondary_handles, secondary_slots), "installed secondary skeleton layout");
    for (auto* bytes : {&secondary_handles, &secondary_slots})
    {
        for (auto& byte : *bytes)
        {
            byte ^= 1;
            Check(!MatchesKnifeSecondaryLayoutEvidence(secondary_handles, secondary_slots), "changed secondary evidence rejected");
            byte ^= 1;
        }
    }
    secondary_handles.pop_back();
    Check(!MatchesKnifeSecondaryLayoutEvidence(secondary_handles, secondary_slots), "truncated secondary evidence rejected");
    Check(MatchesKnifeBodyGraphLayoutEvidence(body_construction, body_use), "installed body graph wrapper evidence");
    for (auto* bytes : {&body_construction, &body_use})
    {
        for (auto& byte : *bytes)
        {
            byte ^= 1;
            Check(!MatchesKnifeBodyGraphLayoutEvidence(body_construction, body_use), "changed wrapper evidence rejected");
            byte ^= 1;
        }
    }
    body_use.pop_back();
    Check(!MatchesKnifeBodyGraphLayoutEvidence(body_construction, body_use), "truncated wrapper evidence rejected");
    Check(MatchesKnifeGraphLayoutEvidence(update, destruction, construction), "installed binary must match actual production gate");
    for (auto* bytes : {&update, &destruction, &construction})
    {
        for (auto& byte : *bytes)
        {
            byte ^= 1;
            Check(!MatchesKnifeGraphLayoutEvidence(update, destruction, construction), "every changed evidence byte rejected");
            byte ^= 1;
        }
    }
    update.pop_back();
    Check(!MatchesKnifeGraphLayoutEvidence(update, destruction, construction), "truncated evidence rejected");
    for (const auto& window : GloveVisualEvidenceWindows())
    {
        const auto bytes = ReadPeRva(client, window.rva, window.bytes.size());
        Check(std::equal(bytes.begin(), bytes.end(), window.bytes.begin(), window.bytes.end()),
            "installed default-glove native/HUD instruction window");
    }
    std::cout << "knife_graph_installed_evidence=PASS; glove_visual_installed_evidence=PASS; file_only=1; game_not_loaded=1\n";
}
}

void RunKnifeGraphTests()
{
    ValidAndReadOnly();
    GatesBeforeMemory();
    BoundedNodeLookup();
    OwnershipAndNativeId();
    ConcurrentRebuildRejected();
    BodyGraphWrapper();
    SecondarySkeletonBinding();
    ResourcePairOwnership();
    NativePairedPort();
    NativeHudHandoff();
    StockKnifeModelHandoff();
    NativePrearmedHandoff();
    NativeRetirement();
    NativeLifecycle();
    KnifeSerializationRouting();
    Check(nskinzcs2::runtime::ProbeIndependentKnifeHudBindingForTests(), "independent knife native HUD binding regression");
    InstalledBinaryEvidence();
    std::cout << "independent knife graph: 17 test groups passed\n";
}
