// External, read-only exercise of the SAME graph parser linked into the DLL.
// This executable never obtains write/operation/create-thread process rights.
#include "Runtime/KnifeGraph.hpp"
#include "Runtime/KnifeResources.hpp"
#include "Runtime/KnifeNative.hpp"
#include <Windows.h>
#include <json.hpp>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace nskinzcs2::runtime;
using nlohmann::json;

class ExternalReader final : public KnifeNativeHost
{
public:
    HANDLE process = nullptr;
    std::uintptr_t id_vtable = 0;
    // Even if a test accidentally tries to mutate, foreign addresses must NEVER
    // be invoked as functions in this helper. It owns VM_READ rights only.
    bool AtSafePoint(const KnifeModelBinding&) noexcept override { return false; }
    bool InvokeSetModel(std::uintptr_t, std::uintptr_t, const char*) noexcept override { return false; }
    bool InvokeRecollect(std::uintptr_t, std::uintptr_t) noexcept override { return false; }
    bool CompareExchangeSymbol(std::uintptr_t, std::uintptr_t, std::uintptr_t) noexcept override { return false; }
    ~ExternalReader() override { if (process) { CloseHandle(process); } }
    bool Read(std::uintptr_t address, void* output, std::size_t size) noexcept override
    {
        SIZE_T read = 0;
        return address >= 0x10000 && size != 0 && size <= 0x10000 &&
            ReadProcessMemory(process, reinterpret_cast<const void*>(address), output, size, &read) && read == size;
    }
    bool IsIdParameterNode(std::uintptr_t node) noexcept override
    {
        std::uintptr_t table = 0;
        return Read(node, &table, sizeof(table)) && table == id_vtable;
    }
    template<typename T> T Value(std::uintptr_t address)
    {
        T value {};
        if (!Read(address, &value, sizeof(value))) { throw std::runtime_error("live read failed"); }
        return value;
    }
    bool Alive(const KnifeModelEntityIdentity& entity)
    {
        const auto identity = Value<std::uintptr_t>(entity.address + 0x10);
        return Value<std::uintptr_t>(identity) == entity.address &&
            Value<std::uint32_t>(identity + 0x10) == entity.handle &&
            Value<int>(entity.address + 0x5D0) == entity.creation_tick;
    }
    bool AttachmentsValid(const KnifeModelBinding& b) noexcept override
    {
        try
        {
            const auto arms = Value<std::uintptr_t>(b.arms.address + 0x330);
            auto scene = Value<std::uintptr_t>(b.hud_weapon.address + 0x330);
            std::array<std::uintptr_t, 64> visited {};
            for (std::size_t i = 0; i < visited.size(); ++i)
            {
                if (!scene) return false;
                if (scene == arms) return i != 0 && Value<std::uintptr_t>(scene + 0x30) == b.arms.address;
                if (std::find(visited.begin(), visited.begin() + i, scene) != visited.begin() + i) return false;
                visited[i] = scene;
                scene = Value<std::uintptr_t>(scene + 0x38); // Current Schema m_pParent.
            }
        }
        catch (...) {}
        return false;
    }
    bool BindingAlive(const KnifeNativeContext& c) noexcept override
    {
        try
        {
            const auto& b = c.binding;
            if (!Alive(b.pawn) || !Alive(b.world) || !Alive(b.hud_weapon) || !Alive(b.arms) ||
                Value<std::uint32_t>(b.world.address + 0x520) != b.pawn.handle ||
                Value<std::uint32_t>(b.arms.address + 0x520) != b.pawn.handle ||
                Value<std::uint32_t>(b.hud_weapon.address + 0x520) != b.world.handle ||
                Value<std::uint32_t>(Value<std::uintptr_t>(b.pawn.address + 0x1208) + 0x60) != b.world.handle)
                return false;
            const std::array entities {b.world.address, b.hud_weapon.address, b.arms.address};
            const std::array fields {c.resources.world, c.resources.hud, c.resources.arms};
            for (std::size_t i = 0; i < entities.size(); ++i)
            {
                const auto scene = Value<std::uintptr_t>(entities[i] + 0x330);
                if (Value<std::uintptr_t>(scene + 0x30) != entities[i] || fields[i] != scene + 0x1E0 ||
                    c.model_names[i] != scene + 0x1E8) return false;
            }
            const auto arms_body = Value<std::uintptr_t>(b.arms.address + 0x30);
            const auto hud_body = Value<std::uintptr_t>(b.hud_weapon.address + 0x30);
            return arms_body + 0x510 == b.body_animation_controller && hud_body + 0x510 == c.hud_body_controller &&
                Value<std::uintptr_t>(arms_body + 8) == Value<std::uintptr_t>(b.arms.address + 0x330) &&
                Value<std::uintptr_t>(hud_body + 8) == Value<std::uintptr_t>(b.hud_weapon.address + 0x330);
        }
        catch (...) { return false; }
    }
};

KnifeModelEntityIdentity Identity(const json& value)
{
    return {value.at("address"), value.at("handle"), value.at("creation_tick")};
}

int wmain(int argc, wchar_t** argv)
{
    try
    {
        if (argc != 2 && argc != 3) { throw std::runtime_error("usage: KnifeLiveProbe <fresh discovery.json> [output.json]"); }
        std::ifstream input{std::filesystem::path(argv[1])};
        const auto report = json::parse(input);
        ExternalReader reader;
        reader.process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, report.at("pid").get<DWORD>());
        if (!reader.process) { throw std::runtime_error("OpenProcess read-only failed"); }
        const std::uintptr_t client = report.at("modules").at("client.dll").at("base");
        const std::uintptr_t animation = report.at("modules").at("animationsystem.dll").at("base");
        reader.id_vtable = animation + kKnifeIdNodeVtableRva;
        std::array<std::uint8_t, kKnifeWeaponTypeUpdateSize> update {};
        std::array<std::uint8_t, kKnifeGraphNodeDestructionSize> destruction {};
        std::array<std::uint8_t, kKnifeIdNodeConstructionSize> construction {};
        std::array<std::uint8_t, kKnifeBodyGraphConstructionSize> body_construction {};
        std::array<std::uint8_t, kKnifeBodyGraphUseSize> body_use {};
        std::array<std::uint8_t, kKnifeSecondaryHandleAppendSize> secondary_handles {};
        std::array<std::uint8_t, kKnifeSecondarySlotAppendSize> secondary_slots {};
        const bool layout = reader.Read(client + kKnifeWeaponTypeUpdateRva, update.data(), update.size()) &&
            reader.Read(animation + kKnifeGraphNodeDestructionRva, destruction.data(), destruction.size()) &&
            reader.Read(animation + kKnifeIdNodeConstructionRva, construction.data(), construction.size()) &&
            reader.Read(client + kKnifeBodyGraphConstructionRva, body_construction.data(), body_construction.size()) &&
            reader.Read(client + kKnifeBodyGraphUseRva, body_use.data(), body_use.size()) &&
            MatchesKnifeGraphLayoutEvidence(update, destruction, construction) &&
            MatchesKnifeBodyGraphLayoutEvidence(body_construction, body_use) &&
            reader.Read(client + kKnifeSecondaryHandleAppendRva, secondary_handles.data(), secondary_handles.size()) &&
            reader.Read(client + kKnifeSecondarySlotAppendRva, secondary_slots.data(), secondary_slots.size()) &&
            MatchesKnifeSecondaryLayoutEvidence(secondary_handles, secondary_slots);
        if (!layout) { throw std::runtime_error("live image/layout no longer matches discovery"); }
        const bool resource_layout = VerifyKnifeResourceEvidence(client,
            report.at("modules").at("client.dll").at("size").get<std::size_t>(), reader);
        if (!resource_layout) { throw std::runtime_error("live model/graph resource layout changed"); }
        int verified = 0, examined = 0;
        json results = {{"pid", report.at("pid")}, {"read_only", true}, {"production_parser", true},
            {"model_replacement_tested", false}, {"rows", json::array()}};
        for (const auto& row : report.at("observation").at("players"))
        {
            if (!row.contains("parameter") || !row.contains("children")) { continue; }
            KnifeModelBinding binding;
            binding.scene_epoch = 1;
            binding.xuid = row.at("xuid");
            binding.pawn = Identity(row.at("pawn"));
            binding.world = Identity(row.at("world"));
            binding.arms = Identity(row.at("arms"));
            int matches = 0;
            for (const auto& child : row.at("children"))
            {
                if (child.at("owner_handle") == binding.world.handle && child.at("type") == ".?AVC_CS2HudModelWeapon@@")
                { binding.hud_weapon = Identity(child.at("entity")); ++matches; }
            }
            if (matches != 1) { throw std::runtime_error("ambiguous HUD ownership"); }
            const auto alive = [&]
            {
                return reader.Alive(binding.pawn) && reader.Alive(binding.world) && reader.Alive(binding.arms) &&
                    reader.Alive(binding.hud_weapon) &&
                    reader.Value<std::uint32_t>(binding.world.address + 0x520) == binding.pawn.handle &&
                    reader.Value<std::uint32_t>(binding.arms.address + 0x520) == binding.pawn.handle &&
                    reader.Value<std::uint32_t>(binding.hud_weapon.address + 0x520) == binding.world.handle &&
                    reader.Value<std::uint32_t>(reader.Value<std::uintptr_t>(binding.pawn.address + 0x1208) + 0x60) == binding.world.handle;
            };
            if (!alive()) { throw std::runtime_error("entity or active weapon changed; rerun discovery"); }
            binding.body_animation_controller = row.at("body_animation_controller");
            binding.weapon_graph_controller = row.at("weapon_graph_controller");
            if (!ReadKnifeBodyGraph(binding.body_animation_controller, layout, reader, binding.graph_instance))
            { throw std::runtime_error("body graph wrapper unavailable"); }
            const auto vdata = reader.Value<std::uintptr_t>(binding.world.address + 0x388);
            KnifeGraphSymbols symbols;
            symbols.weapon_type_name = row.at("parameter").at("name_symbol");
            symbols.source_type = reader.Value<std::uintptr_t>(vdata + 0x870);
            if (!symbols.source_type) { symbols.source_type = reader.Value<std::uintptr_t>(vdata + 0x720); }
            const auto result = ProbeKnifeWeaponType(binding, symbols, layout, reader);
            KnifeSecondarySkeleton secondary;
            bool secondary_verified = false;
            if (result.SourceVerified())
            {
                const auto hud_body = reader.Value<std::uintptr_t>(binding.hud_weapon.address + 0x30);
                const auto scene = reader.Value<std::uintptr_t>(binding.hud_weapon.address + 0x330);
                if (reader.Value<std::uintptr_t>(hud_body + 8) != scene ||
                    reader.Value<std::uintptr_t>(scene + 0x30) != binding.hud_weapon.address)
                { throw std::runtime_error("HUD body scene mismatch"); }
                const auto slot_symbol = row.at("arms_body").at("secondary_slots").at(0).at("symbol").get<std::uintptr_t>();
                if (row.at("arms_body").at("secondary_slots").at(0).at("name") != "active_hud_model_weapon")
                { throw std::runtime_error("discovery active slot name mismatch"); }
                secondary_verified = ProbeKnifeSecondarySkeleton(binding, hud_body + 0x510,
                    slot_symbol, layout, reader, secondary);
                if (!secondary_verified) { throw std::runtime_error("production secondary skeleton probe rejected"); }
            }
            KnifeResourcePair resources;
            bool resources_read = false, resources_ready = false;
            bool native_evidence = false, native_read = false;
            KnifePairState native_state;
            if (result.SourceVerified() && secondary_verified)
            {
                const auto model_field = [&](const KnifeModelEntityIdentity& entity)
                {
                    const auto scene = reader.Value<std::uintptr_t>(entity.address + 0x330);
                    if (reader.Value<std::uintptr_t>(scene + 0x30) != entity.address)
                    { throw std::runtime_error("model scene owner changed"); }
                    // Schema: CSkeletonInstance +140, CModelState::m_hModel +A0;
                    // native model getter independently validates +1E0/dereference.
                    return scene + 0x1E0;
                };
                const KnifeResourceFields fields {model_field(binding.world), model_field(binding.hud_weapon), model_field(binding.arms)};
                resources_read = ProbeKnifeResourcePair(binding, fields, resource_layout, reader, resources);
                resources_ready = resources_read && resources.blade_skeletons_match && resources.hud_skeleton_in_graph;
                KnifeNativeContext context;
                context.binding = binding; context.resources = fields;
                context.model_names = {fields.world + 8, fields.hud + 8, fields.arms + 8};
                context.source_definition = row.at("definition"); context.target_definition = context.source_definition;
                context.symbols = symbols; context.symbols.target_type = symbols.source_type;
                context.client = client; context.client_size = report.at("modules").at("client.dll").at("size");
                context.hud_body_controller = reader.Value<std::uintptr_t>(binding.hud_weapon.address + 0x30) + 0x510;
                context.slot_symbol = secondary.slot_symbol;
                KnifeNativePairAccess port(context, reader);
                native_evidence = port.EvidenceVerified();
                native_read = native_evidence && port.Read(binding, native_state);
            }
            if (!alive()) { throw std::runtime_error("binding changed during production probe"); }
            ++examined;
            if (result.SourceVerified()) { ++verified; }
            results["rows"].push_back({{"slot", row.at("slot")}, {"xuid", row.at("xuid")},
                {"definition", row.at("definition")}, {"source_verified", result.SourceVerified()},
                {"secondary_verified", secondary_verified}, {"status", static_cast<int>(result.status)},
                {"node_index", result.parameter.node_index}, {"node_count", result.parameter.node_count},
                {"secondary_slot", secondary.index}, {"secondary_hud_handle", secondary.hud_handle},
                {"resources_read", resources_read}, {"resource_pair_ready", resources_ready},
                {"world_model", resources.world_model}, {"hud_model", resources.hud_model}, {"arms_model", resources.arms_model},
                {"hud_skeleton", resources.hud_skeleton}, {"graph_skeleton_count", resources.graph_count},
                {"hud_skeleton_in_graph", resources.hud_skeleton_in_graph},
                {"native_call_evidence", native_evidence}, {"native_port_read", native_read},
                {"native_port_source_only", true}, {"native_port_world_definition", native_state.world_definition},
                {"native_port_hud_definition", native_state.hud_definition}, {"native_port_attachments", native_state.attachments_valid}});
            std::cout << "slot=" << row.at("slot") << " definition=" << row.at("definition")
                << " status=" << static_cast<int>(result.status) << " source_verified=" << result.SourceVerified()
                << " secondary_verified=" << secondary_verified << " secondary_slot=" << secondary.index
                << " node_index=" << result.parameter.node_index << " node_count=" << result.parameter.node_count
                << " resource_pair_ready=" << resources_ready << " graph_skeleton_count=" << resources.graph_count
                << " native_port_read=" << native_read << " native_call_evidence=" << native_evidence << '\n';
        }
        std::cout << "production_live_probe: examined=" << examined << " verified=" << verified << " writes=0\n";
        if (argc == 3)
        {
            results["verified"] = verified;
            std::ofstream output{std::filesystem::path(argv[2]), std::ios::binary};
            output << results.dump(2) << '\n';
            if (!output) { throw std::runtime_error("cannot save production probe report"); }
        }
        return verified > 0 ? 0 : 2;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
