#include "Core/Config.hpp"
#include "Core/Log.hpp"
#include "Cs2/Catalog.hpp"
#include "Cs2/CatalogStore.hpp"
#include "Cs2/Compatibility.hpp"
#include "Cs2/Runtime.hpp"
#include "Runtime/GloveAppearance.hpp"
#include "Runtime/GloveVisualRefresh.hpp"
#include "Runtime/KnifeAnimation.hpp"
#include "Runtime/KnifeAppearance.hpp"
#include "Runtime/KnifeUpdateProbe.hpp"
#include "Runtime/MapRemap.hpp"
#include "Runtime/RadarAlphaFix.hpp"
#include "Runtime/RadarNativeCompiler.hpp"
#include "Runtime/WeaponAppearance.hpp"
#include "Runtime/WeaponMaterialInit.hpp"
#include "Runtime/WorkshopMaps.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

void RunKnifeModelTests();
void RunKnifeGraphTests();
void RunKnifePairTests();
void RunDemoSkinRewriteTests();

namespace
{
std::filesystem::path g_compatibility_cs2_root;
bool g_mesh_benchmark_only = false;
bool g_knife_model_only = false;
bool g_weapon_paint_only = false;

void WriteRadarPreviewIfRequested(
    const std::string_view name,
    const nskinzcs2::runtime::RadarAlphaOverlay& overlay)
{
    const auto* preview_directory = std::getenv("NSKINZ_RADAR_PREVIEW_DIR");
    if (preview_directory == nullptr || *preview_directory == '\0' || overlay.png_bytes.empty())
    {
        return;
    }
    std::error_code error;
    const auto root = std::filesystem::path(preview_directory);
    std::filesystem::create_directories(root, error);
    if (error)
    {
        return;
    }
    std::string safe_name(name);
    std::replace_if(safe_name.begin(), safe_name.end(), [](const unsigned char character)
    {
        return std::isalnum(character) == 0 && character != '-' && character != '_';
    }, '_');
    std::ofstream output(root / (safe_name + ".png"), std::ios::binary | std::ios::trunc);
    output.write(
        reinterpret_cast<const char*>(overlay.png_bytes.data()),
        static_cast<std::streamsize>(overlay.png_bytes.size()));
}

template <typename Fn>
void RunNamedTest(const char* name, Fn&& fn)
{
    std::cout << "[TEST] " << name << '\n' << std::flush;
    fn();
}

int HandleStructuredTestException(const unsigned int code)
{
    std::cerr << "nSkinzCS2 test crash: SEH 0x" << std::hex << code << std::dec << '\n';
    return EXCEPTION_EXECUTE_HANDLER;
}

void Expect(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void TestCatalogStoreRoundTrip()
{
    nskinzcs2::model::Catalog source;
    source.base_items = {{7, "AK-47"}};
    source.knife_items = {{508, "M9 Bayonet"}};
    source.glove_items = {{5032, "Hand Wraps"}};
    source.weapon_paint_kits = {{409, "虎牙"}, {568, "伽玛多普勒"}};
    source.weapon_paint_uses_legacy_model = {{844, true}, {1353, false}};
    source.weapon_paint_compatibility = {{16, {844, 1353}}, {7, {44, 180}}, {60, {44, 984}}};
    source.glove_paint_kits = {{10009, "Leather"}};
    source.glove_paint_compatibility.emplace(10009, 5032);
    source.sticker_kits = {{1, "Sticker"}};
    source.keychain_kits = {{2, "Keychain"}};
    source.qualities = {{3, "Vintage"}};
    source.runtime_populated = true;
    source.warnings = {"catalog warning"};

    nskinzcs2::model::VisualInfo visual;
    visual.world_model = "weapons/models/knife/knife_m9/weapon_knife_m9.vmdl";
    visual.view_model = visual.world_model;
    visual.resolved_world_model_resource_path = visual.world_model + "_c";
    visual.resolved_view_model_resource_path = visual.resolved_world_model_resource_path;
    visual.knife_family = "m9_bayonet";
    visual.killfeed_icon = "knife_m9_bayonet";
    visual.knife_identity_resolved = true;
    visual.is_knife = true;
    source.visuals.emplace(508, visual);

    nskinzcs2::model::KeychainVisualInfo keychain_visual;
    keychain_visual.model_path = "models/keychain.vmdl";
    keychain_visual.resolved_model_resource_path = "models/keychain.vmdl_c";
    source.keychain_visuals.emplace(2, keychain_visual);

    const auto path = std::filesystem::temp_directory_path() /
        (L"nSkinzCS2.catalog.roundtrip." + std::to_wstring(GetCurrentProcessId()) + L".json");
    std::error_code remove_error;
    std::filesystem::remove(path, remove_error);
    Expect(nskinzcs2::CatalogStore::Save(path, source), "catalog IPC snapshot should save atomically");
    const auto loaded = nskinzcs2::CatalogStore::Load(path);
    std::filesystem::remove(path, remove_error);

    Expect(loaded.has_value(), "catalog IPC snapshot should load");
    Expect(loaded->base_items.size() == 1, "base item count must survive catalog IPC");
    Expect(
        loaded->base_items[0].id == 7 && loaded->base_items[0].name == "AK-47",
        "base item names and IDs must survive catalog IPC");
    Expect(loaded->weapon_paint_kits.size() == 2, "paint kit count must survive catalog IPC");
    Expect(loaded->weapon_paint_uses_legacy_model == source.weapon_paint_uses_legacy_model,
        "both legacy=true and modern=false metadata must survive catalog IPC");
    Expect(loaded->weapon_paint_compatibility == source.weapon_paint_compatibility,
        "exact shared and weapon-specific finish compatibility must survive catalog IPC");
    Expect(
        loaded->weapon_paint_kits[0].id == 409 && loaded->weapon_paint_kits[0].name == "虎牙" &&
            loaded->weapon_paint_kits[1].id == 568 && loaded->weapon_paint_kits[1].name == "伽玛多普勒",
        "localized paint names must survive catalog IPC");
    Expect(
        loaded->glove_paint_compatibility.at(10009) == 5032,
        "glove compatibility must survive catalog IPC");
    Expect(loaded->visuals.at(508).is_knife, "knife visual flags must survive catalog IPC");
    Expect(
        loaded->visuals.at(508).resolved_world_model_resource_path ==
            visual.resolved_world_model_resource_path,
        "resolved knife resources must survive catalog IPC");
    Expect(
        loaded->keychain_visuals.at(2).resolved_model_resource_path ==
            keychain_visual.resolved_model_resource_path,
        "keychain resources must survive catalog IPC");

    // Backwards-compatible IPC must not silently restore an unrestricted list.
    {
        std::ofstream legacy(path, std::ios::binary | std::ios::trunc);
        legacy << R"json({"version":1,"weapon_paint_kits":[{"id":844,"name":"Emperor"}],"runtime_populated":true})json";
    }
    const auto legacy = nskinzcs2::CatalogStore::Load(path);
    std::filesystem::remove(path, remove_error);
    Expect(legacy.has_value() && legacy->weapon_paint_compatibility.empty() &&
        nskinzcs2::CatalogBuilder::CheckWeaponPaintCompatibility(&*legacy, 16, 844) ==
            nskinzcs2::WeaponPaintCompatibility::Unknown,
        "old IPC catalogs without compatibility stay unknown and cannot authorize a colored finish");
}

void TestCompatibilityManifest()
{
    using namespace nskinzcs2::compatibility;
    Expect(kFrameStageNotifyVtableIndex == 36, "HLAE 2.192.6 FrameStageNotify slot should remain explicit");
    Expect(kFrameStartStage == 0, "HLAE 2.192.6 FRAME_START value should remain explicit");
    Expect(kFrameRenderPassStage == 12, "HLAE 2.192.6 FRAME_RENDER_PASS value should remain explicit");
    Expect(kEngineIsPlayingDemoVtableIndex == 42, "HLAE 2.192.6 IsPlayingDemo slot should remain explicit");
    Expect(kEngineGetDemoFilePathVtableIndex == 43, "HLAE 2.192.6 GetDemoFilePath slot should remain explicit");
    Expect(kEngineGetLevelNameShortVtableIndex == 65, "HLAE 2.192.6 GetLevelNameShort slot should remain explicit");
    Expect(kEngineGetDemoFileVtableIndex == 69, "HLAE 2.192.6 GetDemoFile slot should remain explicit");
    Expect(kEngineExecuteClientCmdVtableIndex == 51, "HLAE 2.192.6 ExecuteClientCmd slot should remain explicit");
    Expect(kDemoFileGetTickVtableIndex == 3, "HLAE 2.192.6 demo tick slot should remain explicit");
    Expect(kDemoFileIsPlayingVtableIndex == 11, "HLAE 2.192.6 IDemoFile IsPlayingDemo slot should remain explicit");
    Expect(kDemoFileIsPausedVtableIndex == 12, "HLAE 2.192.6 IDemoFile IsDemoPaused slot should remain explicit");
    Expect(
        IsHlae21911InterfaceCombination("Source2Client002", "ResourceSystem013", "VFileSystem017"),
        "verified HLAE 2.192.6 interfaces should be accepted");
    Expect(
        !IsHlae21911InterfaceCombination("Source2Client001", "ResourceSystem013", "VFileSystem017"),
        "an unverified Source2Client interface should be rejected");

    const std::array<std::uint8_t, 12> bytes {
        0x48, 0x83, 0xEC, 0x38, 0x41, 0xB1, 0x01, 0xCC, 0x48, 0x83, 0xEC, 0x38,
    };
    Expect(CountPatternMatches(bytes, "48 83 EC ?? 41 B1 01") == 1, "wildcard pattern should match once");
    Expect(CountPatternMatches(bytes, "48 83 EC 38") == 2, "duplicate patterns should be observable");
    Expect(CountPatternMatches(bytes, "DE AD BE EF") == 0, "missing patterns should be observable");

    const auto has_obsolete_set_mesh = std::any_of(
        kSignatureSpecs.begin(),
        kSignatureSpecs.end(),
        [](const SignatureSpec& spec) { return std::string_view(spec.name) == "set_mesh"; });
    Expect(!has_obsolete_set_mesh, "obsolete SetMesh reference must not be a compatibility requirement");

    const auto has_required_signature =
        [](const std::string_view name)
        {
            return std::any_of(
                kSignatureSpecs.begin(),
                kSignatureSpecs.end(),
                [name](const SignatureSpec& spec)
                {
                    return spec.required && std::string_view(spec.name) == name;
                });
        };
    Expect(
        has_required_signature("wearable_entity_factory_callsite"),
        "the current CGameEntitySystem wearable factory callsite must be required");
    Expect(
        has_required_signature("wearable_entity_create_by_name"),
        "the current formal client CreateEntityByName wrapper must be required");
    Expect(
        has_required_signature("wearable_entity_formal_create"),
        "the current formal identity allocation/create path must be required");
    Expect(
        has_required_signature("wearable_entity_class_lookup"),
        "the current client class descriptor lookup must be required");
    Expect(
        has_required_signature("wearable_definition_refresh_thunk"),
        "the current CEconWearable definition refresh thunk must be required");
    Expect(
        has_required_signature("base_model_entity_set_model"),
        "the current client C_BaseModelEntity::SetModel wrapper must be required");
    Expect(
        has_required_signature("base_entity_subclass_changed"),
        "the current C_BaseEntity::OnSubclassIDChanged callback must be required for knife VData identity");
    Expect(
        has_required_signature("utl_string_token_make_sret"),
        "the hidden-result CUtlStringToken::Make ABI must be required for knife subclass tokens");
    const auto has_reference_model_provider_signature = std::any_of(
        kSignatureSpecs.begin(),
        kSignatureSpecs.end(),
        [](const SignatureSpec& spec)
        {
            return !spec.required &&
                std::string_view(spec.name) == "skeleton_instance_bind_model_reference";
        });
    Expect(
        has_reference_model_provider_signature,
        "the unverified model-provider entry must remain reference-only");
}

void TestInstalledCompatibilityIfRequested()
{
    if (g_compatibility_cs2_root.empty())
    {
        return;
    }

    const auto report = nskinzcs2::compatibility::ScanInstalledCs2(g_compatibility_cs2_root);
    std::cout << "[COMPAT] ClientVersion=" << report.client_version
        << " PatchVersion=" << report.patch_version
        << " SourceRevision=" << report.source_revision << '\n';
    for (const auto& result : report.signatures)
    {
        std::cout << "[COMPAT] " << result.name << " matches=" << result.match_count
            << " size=" << result.file_size << " pe_timestamp=0x" << std::hex
            << result.pe_timestamp << std::dec << '\n';
    }
    Expect(!report.install_root.empty(), "--cs2-root must resolve to a CS2 installation");
    Expect(report.client_version == nskinzcs2::compatibility::kVerifiedClientVersion,
        "installed CS2 ClientVersion differs from the verified baseline");
    Expect(report.patch_version == nskinzcs2::compatibility::kVerifiedPatchVersion,
        "installed CS2 PatchVersion differs from the verified baseline");
    Expect(report.source_revision == nskinzcs2::compatibility::kVerifiedSourceRevision,
        "installed CS2 SourceRevision differs from the verified baseline");
    Expect(report.RequiredSignaturesUnique(), "all required installed CS2 signatures must match exactly once");
}

bool ContainsRefreshAttribute(
    const std::vector<std::pair<std::uint16_t, float>>& attributes,
    const std::uint16_t definition_index,
    const float expected_value,
    const float epsilon = 0.0001f)
{
    const auto it = std::find_if(
        attributes.begin(),
        attributes.end(),
        [definition_index](const auto& attribute)
        {
            return attribute.first == definition_index;
        });
    return it != attributes.end() && std::fabs(it->second - expected_value) <= epsilon;
}

bool ContainsExactWearAttribute(
    const std::vector<std::pair<std::uint16_t, float>>& attributes,
    const float expected_value)
{
    const auto it = std::find_if(
        attributes.begin(),
        attributes.end(),
        [](const auto& attribute)
        {
            return attribute.first == 8;
        });
    return it != attributes.end() &&
        nskinzcs2::model::CosmeticWearMatchesExactly(it->second, expected_value);
}

bool ContainsDefinitionIndex(
    const std::vector<std::uint16_t>& definition_indices,
    const std::uint16_t definition_index)
{
    return std::find(definition_indices.begin(), definition_indices.end(), definition_index) != definition_indices.end();
}

void ExpectContainsKeychainSlotZeroAttributes(
    const std::vector<std::pair<std::uint16_t, float>>& attributes,
    const float expected_keychain_id)
{
    Expect(ContainsRefreshAttribute(attributes, 299, expected_keychain_id), "keychain block should include id");
    Expect(ContainsRefreshAttribute(attributes, 300, 0.0f), "keychain block should include offset x");
    Expect(ContainsRefreshAttribute(attributes, 301, 0.0f), "keychain block should include offset y");
    Expect(ContainsRefreshAttribute(attributes, 302, 0.0f), "keychain block should include offset z");
    Expect(ContainsRefreshAttribute(attributes, 306, 0.0f), "keychain block should include seed");
    Expect(ContainsRefreshAttribute(attributes, 314, 0.0f), "keychain block should include highlight");
    Expect(ContainsRefreshAttribute(attributes, 321, 0.0f), "keychain block should include sticker");
}

void ExpectContainsKeychainSlotZeroDefinitionIndices(const std::vector<std::uint16_t>& definition_indices)
{
    Expect(ContainsDefinitionIndex(definition_indices, 299), "keychain block should include id");
    Expect(ContainsDefinitionIndex(definition_indices, 300), "keychain block should include offset x");
    Expect(ContainsDefinitionIndex(definition_indices, 301), "keychain block should include offset y");
    Expect(ContainsDefinitionIndex(definition_indices, 302), "keychain block should include offset z");
    Expect(ContainsDefinitionIndex(definition_indices, 306), "keychain block should include seed");
    Expect(ContainsDefinitionIndex(definition_indices, 314), "keychain block should include highlight");
    Expect(ContainsDefinitionIndex(definition_indices, 321), "keychain block should include sticker");
}

struct ExecutableBuffer
{
    std::uint8_t* address = nullptr;
    std::size_t size = 0;

    explicit ExecutableBuffer(
        const std::size_t requested_size,
        const DWORD protection = PAGE_EXECUTE_READWRITE)
        : size(requested_size)
    {
        address = static_cast<std::uint8_t*>(
            VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, protection));
        Expect(address != nullptr, "executable buffer allocation should succeed");
        std::memset(address, 0xCC, size);
    }

    ~ExecutableBuffer()
    {
        if (address != nullptr)
        {
            VirtualFree(address, 0, MEM_RELEASE);
        }
    }

    ExecutableBuffer(const ExecutableBuffer&) = delete;
    ExecutableBuffer& operator=(const ExecutableBuffer&) = delete;

    template <typename T>
    T Ptr(const std::size_t offset = 0) const
    {
        return reinterpret_cast<T>(address + offset);
    }

    void WriteBytes(const std::initializer_list<std::uint8_t> bytes, const std::size_t offset = 0)
    {
        Expect(offset + bytes.size() <= size, "byte write must fit inside executable buffer");
        std::memcpy(address + offset, bytes.begin(), bytes.size());
    }

    void WritePointer(const std::size_t offset, void* value)
    {
        Expect(offset + sizeof(value) <= size, "pointer write must fit inside executable buffer");
        std::memcpy(address + offset, &value, sizeof(value));
    }

    void WriteDisp32(const std::size_t offset, const std::intptr_t displacement)
    {
        const auto value = static_cast<std::int32_t>(displacement);
        Expect(offset + sizeof(value) <= size, "disp32 write must fit inside executable buffer");
        std::memcpy(address + offset, &value, sizeof(value));
    }

    void WriteU32(const std::size_t offset, const std::uint32_t value)
    {
        Expect(offset + sizeof(value) <= size, "u32 write must fit inside executable buffer");
        std::memcpy(address + offset, &value, sizeof(value));
    }

    void WriteCString(const std::size_t offset, const char* value)
    {
        const auto length = std::strlen(value) + 1;
        Expect(offset + length <= size, "cstring write must fit inside executable buffer");
        std::memcpy(address + offset, value, length);
    }
};

std::string NarrowAsciiLossy(const std::wstring& value)
{
    std::string narrowed;
    narrowed.reserve(value.size());
    for (const auto ch : value)
    {
        narrowed.push_back(ch >= 0 && ch <= 0x7F ? static_cast<char>(ch) : '?');
    }
    return narrowed;
}

struct GetterObject
{
    void** vtable = nullptr;
    std::array<void*, 8> slots {};
};

struct PointerWrapper
{
    std::array<void*, 8> slots {};
};

std::intptr_t RipDisp32(const std::uint8_t* instruction, const std::size_t next_instruction_offset, const void* target)
{
    const auto next = reinterpret_cast<std::intptr_t>(instruction + next_instruction_offset);
    const auto destination = reinterpret_cast<std::intptr_t>(target);
    return destination - next;
}

enum class SyntheticCreateFieldEncoding
{
    Ptr64,
    Rva32,
    Rel32,
    TaggedRva31,
};

void BuildSyntheticWearableClientClassModule(
    ExecutableBuffer& module,
    const SyntheticCreateFieldEncoding encoding,
    const std::size_t create_offset,
    const std::size_t name_offset,
    const std::size_t next_offset)
{
    constexpr std::size_t node_count = 4;
    constexpr std::size_t node_size = 0x30;
    constexpr std::size_t string_offset = 0x40;
    constexpr std::size_t rel_target_base = 0x80;
    constexpr std::size_t nodes_offset = 0x100;
    constexpr std::size_t direct_target_base = 0x240;

    module.WriteCString(string_offset, "CEconWearable");
    auto* const name_pointer = module.address + string_offset;
    for (std::size_t index = 0; index < node_count; ++index)
    {
        const auto node_offset = nodes_offset + index * node_size;
        auto* const node = module.address + node_offset;
        auto* const next_node =
            index + 1 < node_count
                ? module.address + nodes_offset + (index + 1) * node_size
                : nullptr;

        module.WritePointer(node_offset + name_offset, name_pointer);
        module.WritePointer(node_offset + next_offset, next_node);

        switch (encoding)
        {
        case SyntheticCreateFieldEncoding::Ptr64:
        {
            module.WritePointer(create_offset + node_offset, module.address + direct_target_base + index * sizeof(void*));
            break;
        }
        case SyntheticCreateFieldEncoding::Rva32:
        {
            const auto value = static_cast<std::uint32_t>(direct_target_base + index * sizeof(void*));
            module.WriteU32(create_offset + node_offset, value);
            break;
        }
        case SyntheticCreateFieldEncoding::Rel32:
        {
            auto* const target = module.address + rel_target_base + index * sizeof(void*);
            const auto displacement =
                reinterpret_cast<std::intptr_t>(target) -
                reinterpret_cast<std::intptr_t>(node + create_offset);
            module.WriteDisp32(create_offset + node_offset, displacement);
            break;
        }
        case SyntheticCreateFieldEncoding::TaggedRva31:
        {
            const auto value =
                0x80000000u | static_cast<std::uint32_t>(direct_target_base + index * sizeof(void*));
            module.WriteU32(create_offset + node_offset, value);
            break;
        }
        }
    }
}

void TestSource2ClientGetterProbe()
{
    using nskinzcs2::runtime::ProbeSource2ClientPointerGetterForTests;

    {
        GetterObject object;
        object.slots[1] = reinterpret_cast<void*>(0x12345678ULL);

        ExecutableBuffer getter(16);
        getter.WriteBytes({0x48, 0x8B, 0x41, 0x10, 0xC3});

        const auto result = ProbeSource2ClientPointerGetterForTests(
            GetModuleHandleW(nullptr),
            &object,
            getter.Ptr<void*>(0));
        Expect(result.success, "direct member getter should resolve");
        Expect(!result.used_forwarder, "direct member getter should not report forwarder usage");
        Expect(result.resolved_pointer == object.slots[1], "direct member getter should return the field pointer");
    }

    {
        GetterObject object;
        object.slots[1] = reinterpret_cast<void*>(0x13572468ULL);

        ExecutableBuffer getter(32);
        getter.WriteBytes({
            0x53,
            0x57,
            0x48, 0x83, 0xEC, 0x20,
            0x48, 0x8B, 0xD9,
            0x48, 0x8B, 0x43, 0x10,
            0x48, 0x83, 0xC4, 0x20,
            0x5F,
            0x5B,
            0xC3
        });

        const auto result = ProbeSource2ClientPointerGetterForTests(
            GetModuleHandleW(nullptr),
            &object,
            getter.Ptr<void*>(0));
        Expect(result.success, "prologue-wrapped member getter with push/pop epilogue should resolve");
        Expect(!result.used_forwarder, "prologue-wrapped member getter should remain a direct accessor");
        Expect(result.resolved_pointer == object.slots[1], "prologue-wrapped member getter should return the field pointer");
    }

    {
        GetterObject object;
        object.slots[1] = reinterpret_cast<void*>(0x24681357ULL);

        ExecutableBuffer getter(32);
        getter.WriteBytes({
            0x48, 0x89, 0x5C, 0x24, 0x08,
            0x48, 0x83, 0xEC, 0x20,
            0x48, 0x8B, 0xD9,
            0x48, 0x8B, 0x43, 0x10,
            0x48, 0x8B, 0x5C, 0x24, 0x28,
            0x48, 0x83, 0xC4, 0x20,
            0xC3
        });

        const auto result = ProbeSource2ClientPointerGetterForTests(
            GetModuleHandleW(nullptr),
            &object,
            getter.Ptr<void*>(0));
        Expect(result.success, "prologue-wrapped member getter with stack spill/restore should resolve");
        Expect(!result.used_forwarder, "stack-spill getter should remain a direct accessor");
        Expect(result.resolved_pointer == object.slots[1], "stack-spill getter should return the field pointer");
    }

    {
        GetterObject object;
        object.slots[1] = reinterpret_cast<void*>(0x22223333ULL);
        std::array<void*, 4> vtable {};

        ExecutableBuffer getter(16);
        getter.WriteBytes({0x48, 0x8B, 0x41, 0x10, 0xC3});
        vtable[2] = getter.Ptr<void*>(0);
        object.vtable = vtable.data();

        ExecutableBuffer thunk(64);
        thunk.WriteBytes({
            0x48, 0x8B, 0x0D, 0x00, 0x00, 0x00, 0x00,
            0x48, 0x8B, 0x01,
            0xFF, 0xA0, 0x10, 0x00, 0x00, 0x00
        });
        thunk.WritePointer(32, &object);
        thunk.WriteDisp32(3, RipDisp32(thunk.address, 7, thunk.address + 32));

        const auto result = ProbeSource2ClientPointerGetterForTests(
            GetModuleHandleW(nullptr),
            &object,
            thunk.Ptr<void*>(0));
        Expect(result.success, "rip-global forwarder thunk should resolve");
        Expect(result.used_forwarder, "rip-global thunk should report forwarder usage");
        Expect(!result.used_seh_call, "rip-global thunk should not need SEH call validation");
        Expect(result.real_object == &object, "rip-global thunk should resolve the real object");
        Expect(result.vtable_offset == 0x10, "rip-global thunk should preserve the vtable offset");
        Expect(result.final_getter_target == getter.Ptr<void*>(0), "rip-global thunk should resolve the final getter target");
        Expect(result.resolved_pointer == object.slots[1], "rip-global thunk should recurse into the direct getter");
    }

    {
        GetterObject object;
        object.slots[1] = reinterpret_cast<void*>(0x23234444ULL);
        std::array<void*, 4> vtable {};

        ExecutableBuffer getter(16);
        getter.WriteBytes({0x48, 0x8B, 0x41, 0x10, 0xC3});
        vtable[2] = getter.Ptr<void*>(0);
        object.vtable = vtable.data();

        ExecutableBuffer thunk(64);
        thunk.WriteBytes({
            0x48, 0x8B, 0x0D, 0x00, 0x00, 0x00, 0x00,
            0x48, 0x8B, 0x01,
            0x48, 0xFF, 0x60, 0x10
        });
        thunk.WritePointer(32, &object);
        thunk.WriteDisp32(3, RipDisp32(thunk.address, 7, thunk.address + 32));

        const auto result = ProbeSource2ClientPointerGetterForTests(
            GetModuleHandleW(nullptr),
            &object,
            thunk.Ptr<void*>(0));
        Expect(result.success, "rip-global forwarder thunk with REX dispatch should resolve");
        Expect(result.used_forwarder, "rip-global REX thunk should report forwarder usage");
        Expect(!result.used_seh_call, "rip-global REX thunk should not need SEH call validation");
        Expect(result.vtable_offset == 0x10, "rip-global REX thunk should preserve the vtable offset");
        Expect(result.final_getter_target == getter.Ptr<void*>(0), "rip-global REX thunk should resolve the final getter target");
        Expect(result.resolved_pointer == object.slots[1], "rip-global REX thunk should recurse into the direct getter");
    }

    {
        GetterObject object;
        object.slots[1] = reinterpret_cast<void*>(0x44445555ULL);
        std::array<void*, 4> vtable {};

        ExecutableBuffer getter(16);
        getter.WriteBytes({0x48, 0x8B, 0x41, 0x10, 0xC3});
        vtable[2] = getter.Ptr<void*>(0);
        object.vtable = vtable.data();

        PointerWrapper wrapper;
        wrapper.slots[1] = &object;

        ExecutableBuffer thunk(64);
        thunk.WriteBytes({
            0x48, 0x83, 0xEC, 0x28,
            0x48, 0x8B, 0x49, 0x08,
            0x48, 0x8B, 0x01,
            0xFF, 0x50, 0x10,
            0x48, 0x83, 0xC4, 0x28,
            0xC3
        });

        const auto result = ProbeSource2ClientPointerGetterForTests(
            GetModuleHandleW(nullptr),
            &wrapper,
            thunk.Ptr<void*>(0));
        Expect(result.success, "member forwarder thunk with stack frame should resolve");
        Expect(result.used_forwarder, "member forwarder thunk should report forwarder usage");
        Expect(!result.used_seh_call, "member forwarder thunk should not need SEH call validation");
        Expect(result.real_object == &object, "member forwarder thunk should resolve the wrapped object");
        Expect(result.resolved_pointer == object.slots[1], "member forwarder thunk should recurse into the final getter");
    }

    {
        GetterObject object;
        object.slots[3] = reinterpret_cast<void*>(0x66667777ULL);
        std::array<void*, 4> vtable {};
        ExecutableBuffer final_getter(32);
        final_getter.WriteBytes({
            0x48, 0x89, 0xC8,
            0x48, 0x83, 0xC0, 0x20,
            0x48, 0x8B, 0x00,
            0xC3
        });
        vtable[2] = final_getter.Ptr<void*>(0);
        object.vtable = vtable.data();

        PointerWrapper wrapper;
        wrapper.slots[1] = &object;

        ExecutableBuffer thunk(64);
        thunk.WriteBytes({
            0x48, 0x8B, 0x49, 0x08,
            0x48, 0x8B, 0x01,
            0xFF, 0x50, 0x10,
            0xC3
        });

        const auto result = ProbeSource2ClientPointerGetterForTests(
            GetModuleHandleW(nullptr),
            &wrapper,
            thunk.Ptr<void*>(0));
        Expect(result.success, "recognized forwarder should be allowed to validate the final getter with a single call");
        Expect(result.used_forwarder, "recognized forwarder should report forwarder usage");
        Expect(result.used_seh_call, "recognized forwarder should report SEH-call validation when static parsing fails");
        Expect(result.final_getter_target == final_getter.Ptr<void*>(0), "recognized forwarder should preserve the final getter target");
        Expect(result.resolved_pointer == object.slots[3], "recognized forwarder should return the call result");
    }

    {
        GetterObject object;
        std::array<void*, 4> vtable {};

        PointerWrapper wrapper;
        wrapper.slots[1] = &object;

        ExecutableBuffer thunk(64);
        thunk.WriteBytes({
            0x48, 0x8B, 0x49, 0x08,
            0x48, 0x8B, 0x01,
            0xFF, 0x60, 0x10
        });
        vtable[2] = thunk.Ptr<void*>(0);
        object.vtable = vtable.data();

        const auto result = ProbeSource2ClientPointerGetterForTests(
            GetModuleHandleW(nullptr),
            &wrapper,
            thunk.Ptr<void*>(0));
        Expect(!result.success, "recursive thunk loops should be rejected");
        Expect(result.failure_reason.find(L"target loop") != std::wstring::npos, "recursive thunk loop rejection should mention the target loop");
    }

    {
        ExecutableBuffer invalid(16);
        invalid.WriteBytes({0x90, 0x90, 0xC3});

        GetterObject object;
        const auto result = ProbeSource2ClientPointerGetterForTests(
            GetModuleHandleW(nullptr),
            &object,
            invalid.Ptr<void*>(0));
        Expect(!result.success, "non-accessor non-forwarder getter bodies should fail");
        Expect(
            result.failure_reason.find(L"unsupported getter body") != std::wstring::npos ||
            result.failure_reason.find(L"Forwarder thunk") != std::wstring::npos,
            "non-accessor failures should preserve parser diagnostics");
    }
}

void TestGloveWearableCandidateRanking()
{
    using nskinzcs2::runtime::GloveWearableCandidateProbeInput;
    using nskinzcs2::runtime::GloveWearableCandidateProbeStage;
    using nskinzcs2::runtime::ProbeGloveWearableCandidateRankingForTests;

    {
        GloveWearableCandidateProbeInput input;
        input.stage = GloveWearableCandidateProbeStage::PawnWearableHandles;
        input.has_item_view = true;
        input.exact_glove_definition = true;
        const auto result = ProbeGloveWearableCandidateRankingForTests(input);
        Expect(result.accepted, "handle-backed exact glove candidates should be accepted");
        Expect(result.score == 400, "handle-backed exact glove candidates should have the highest score");
    }

    {
        GloveWearableCandidateProbeInput handle_exact;
        handle_exact.stage = GloveWearableCandidateProbeStage::PawnWearableHandles;
        handle_exact.has_item_view = true;
        handle_exact.exact_glove_definition = true;

        GloveWearableCandidateProbeInput loose_renderable;
        loose_renderable.stage = GloveWearableCandidateProbeStage::LooseGloveRenderables;
        loose_renderable.has_item_view = true;
        loose_renderable.owner_matches_xuid = true;
        loose_renderable.glove_renderable = true;

        const auto exact_result = ProbeGloveWearableCandidateRankingForTests(handle_exact);
        const auto loose_result = ProbeGloveWearableCandidateRankingForTests(loose_renderable);
        Expect(
            exact_result.accepted && loose_result.accepted && exact_result.score > loose_result.score,
            "handle-backed exact glove candidates should outrank loose glove renderables");
    }

    {
        GloveWearableCandidateProbeInput input;
        input.stage = GloveWearableCandidateProbeStage::LooseGloveRenderables;
        input.has_item_view = true;
        input.glove_renderable = true;
        const auto result = ProbeGloveWearableCandidateRankingForTests(input);
        Expect(!result.accepted, "glove-like entities without owner/account evidence should be rejected");
        Expect(
            result.rejection_reason.find(L"owner") != std::wstring::npos,
            "owner/account rejection should be surfaced");
    }

    {
        GloveWearableCandidateProbeInput input;
        input.stage = GloveWearableCandidateProbeStage::PawnWearableHandles;
        input.exact_glove_definition = true;
        const auto result = ProbeGloveWearableCandidateRankingForTests(input);
        Expect(!result.accepted, "candidates without item views should be rejected");
        Expect(
            result.rejection_reason == L"missing_item_view",
            "missing item-view rejection should be preserved");
    }
}

void TestDataFirstWearableClientClassInference()
{
    using nskinzcs2::runtime::ProbeWearableClientClassNodeInferenceForTests;

    const auto run_case =
        [](const SyntheticCreateFieldEncoding encoding, const wchar_t* expected_resolution)
        {
            constexpr std::size_t create_offset = 0x08;
            constexpr std::size_t name_offset = 0x10;
            constexpr std::size_t next_offset = 0x20;

            ExecutableBuffer module(0x400, PAGE_READWRITE);
            BuildSyntheticWearableClientClassModule(
                module,
                encoding,
                create_offset,
                name_offset,
                next_offset);

            const auto result =
                ProbeWearableClientClassNodeInferenceForTests(module.address, module.size, "CEconWearable");
            if (!result.success)
            {
                throw std::runtime_error(
                    "data-first node inference should succeed on a synthetic CEconWearable neighborhood: " +
                    NarrowAsciiLossy(result.failure_reason));
            }
            Expect(result.create_fn_offset == create_offset, "data-first node inference should recover the create offset");
            Expect(result.name_offset == name_offset, "data-first node inference should recover the name offset");
            Expect(result.next_offset == next_offset, "data-first node inference should recover the next offset");
            Expect(result.chain_length >= 4, "data-first node inference should require at least four valid nodes");
            Expect(
                result.create_field_resolution == expected_resolution,
                "data-first node inference should preserve the create-field encoding kind");
            Expect(
                result.raw_create_target != nullptr,
                "data-first node inference should resolve the raw create target");
        };

    run_case(SyntheticCreateFieldEncoding::Ptr64, L"ptr64");
    run_case(SyntheticCreateFieldEncoding::Rva32, L"rva32");
    run_case(SyntheticCreateFieldEncoding::Rel32, L"rel32");
    run_case(SyntheticCreateFieldEncoding::TaggedRva31, L"tagged_rva31");
}

const std::vector<int>& StandardWeaponDefinitionIndices()
{
    static const std::vector<int> indices {
        1, 2, 3, 4, 7, 8, 9, 10, 11, 13, 14, 16, 17, 19, 23, 24, 25,
        26, 27, 28, 29, 30, 32, 33, 34, 35, 36, 38, 39, 40, 60, 61, 63, 64
    };
    return indices;
}

void TestConfigRoundTrip()
{
    namespace fs = std::filesystem;
    const auto temp_path = fs::temp_directory_path() / "nSkinzCS2.config.test.json";

    nskinzcs2::model::ConfigDocument document;
    nskinzcs2::model::CosmeticProfile profile;
    profile.name = "AK test";
    profile.enabled = true;
    profile.xuid = 76561198000000000ULL;
    profile.base_item = 7;
    profile.override_item = 515;
    profile.paint_kit = 0;
    profile.explicit_vanilla_finish = true;
    profile.quality = 12;
    profile.seed = 1337;
    profile.stat_trak = 42;
    profile.wear = 0.1337f;
    profile.custom_name = "movie prop";
    profile.stickers[0].kit_id = 3;
    profile.stickers[0].rotation = 90.0f;
    document.profiles.push_back(profile);
    nskinzcs2::model::KnifeProfile knife_profile;
    knife_profile.explicit_vanilla_finish = true;
    document.knife_profiles.push_back(knife_profile);
    nskinzcs2::model::GloveProfile glove_profile;
    glove_profile.explicit_vanilla_finish = true;
    document.glove_profiles.push_back(glove_profile);
    nskinzcs2::model::KeychainProfile keychain_profile;
    keychain_profile.explicit_clear = true;
    document.keychain_profiles.push_back(keychain_profile);
    document.map_remap.enabled = true;
    document.map_remap.use_source_radar = false;
    document.map_remap.radar_black_tolerance = 37;
    document.map_remap.radar_dark_detail_radius = 9;
    document.map_remap.radar_background_alpha_percent = 12;
    document.map_remap.source_map_name = "de_mirage";
    document.map_remap.workshop_id = "3415616810";
    document.map_remap.workshop_title = "Rainy Mirage";
    document.map_remap.addon_name = "mirage_rain";
    document.map_remap.target_map_name = "de_mirage_rain";
    document.map_remap.target_skybox_resource_prefix =
        "maps/prefabs/de_mirage/de_mirage_rain_skybox";
    document.map_remap.radar_overlay_request_virtual_path =
        "panorama/images/overheadmaps/de_mirage_rain_radar_tga.vtex_c";
    document.map_remap.radar_overlay_virtual_path =
        "panorama/images/overheadmaps/nskinz_0123456789abcdef_radar_tga.vtex_c";
    document.map_remap.radar_overlay_root_path = "C:/Temp/nSkinzCS2/radar";
    document.map_remap.radar_overlay_file_path =
        "C:/Temp/nSkinzCS2/radar/panorama/images/overheadmaps/de_mirage_rain_radar_tga.vtex_c";
    document.experimental_allow_hud_alias_synthetic_proxy_model_write = true;

    Expect(nskinzcs2::ConfigStore::Save(temp_path, document), "config save should succeed");
    const auto loaded = nskinzcs2::ConfigStore::Load(temp_path);

    Expect(loaded.version == 7, "config should round-trip version 7");
    Expect(loaded.profiles.size() == 1, "config should contain one profile");
    Expect(loaded.profiles[0].name == "AK test", "profile name should round-trip");
    Expect(loaded.profiles[0].stickers[0].kit_id == 3, "sticker should round-trip");
    Expect(loaded.profiles[0].explicit_vanilla_finish, "weapon vanilla intent should round-trip");
    Expect(loaded.knife_profiles[0].explicit_vanilla_finish, "knife vanilla intent should round-trip");
    Expect(loaded.glove_profiles[0].explicit_vanilla_finish, "glove vanilla intent should round-trip");
    Expect(loaded.keychain_profiles[0].explicit_clear, "keychain clear intent should round-trip");
    Expect(loaded.map_remap.enabled, "map remap enable intent should round-trip");
    Expect(!loaded.map_remap.use_source_radar, "map remap radar source intent should round-trip");
    Expect(
        loaded.map_remap.radar_black_tolerance == 37,
        "map remap radar black tolerance should round-trip");
    Expect(
        loaded.map_remap.radar_dark_detail_radius == 9,
        "radar dark-detail radius should round-trip within the expanded safe range");
    Expect(
        loaded.map_remap.radar_background_alpha_percent == 12,
        "map remap radar background opacity should round-trip");
    Expect(loaded.map_remap.source_map_name == "de_mirage", "map remap source should round-trip");
    Expect(loaded.map_remap.workshop_id == "3415616810", "map remap workshop id should round-trip");
    Expect(loaded.map_remap.addon_name == "mirage_rain", "map remap addon name should round-trip");
    Expect(loaded.map_remap.target_map_name == "de_mirage_rain", "map remap target should round-trip");
    Expect(
        loaded.map_remap.target_skybox_resource_prefix ==
            document.map_remap.target_skybox_resource_prefix,
        "map remap target skybox resource prefix should round-trip");
    Expect(
        loaded.map_remap.radar_overlay_request_virtual_path ==
            document.map_remap.radar_overlay_request_virtual_path,
        "map remap Workshop radar request path should round-trip");
    Expect(
        loaded.map_remap.radar_overlay_virtual_path == document.map_remap.radar_overlay_virtual_path,
        "map remap external radar virtual path should round-trip");
    Expect(
        loaded.map_remap.radar_overlay_root_path == document.map_remap.radar_overlay_root_path,
        "map remap external radar root should round-trip");
    Expect(
        loaded.map_remap.radar_overlay_file_path == document.map_remap.radar_overlay_file_path,
        "map remap external radar file should round-trip");
    Expect(!loaded.keychain_profiles.empty(), "config load should repair missing keychain profiles");
    Expect(
        loaded.experimental_allow_hud_alias_synthetic_proxy_model_write,
        "experimental HUD proxy write toggle should round-trip");

    fs::remove(temp_path);

    {
        std::ofstream legacy_stream(temp_path, std::ios::trunc);
        legacy_stream << R"({
            "version": 6,
            "map_remap": {
                "radar_black_tolerance": 24,
                "radar_dark_detail_radius": 4,
                "radar_overlay_virtual_path": "panorama/images/overheadmaps/de_old_radar_tga.vtex_c",
                "radar_overlay_root_path": "C:/Temp/old",
                "radar_overlay_file_path": "C:/Temp/old/de_old_radar_tga.vtex_c"
            }
        })";
    }
    const auto migrated = nskinzcs2::ConfigStore::Load(temp_path);
    Expect(migrated.version == 7, "v6 radar config should migrate to version 7");
    Expect(
        migrated.map_remap.radar_black_tolerance == 12 &&
            migrated.map_remap.radar_dark_detail_radius == 12,
        "v6 destructive radar defaults should migrate to road-preserving defaults");
    Expect(
        migrated.map_remap.radar_overlay_request_virtual_path.empty() &&
            migrated.map_remap.radar_overlay_virtual_path.empty() &&
            migrated.map_remap.radar_overlay_root_path.empty() &&
            migrated.map_remap.radar_overlay_file_path.empty(),
        "v6 radar overlay cache should be invalidated during migration");
    fs::remove(temp_path);
}

void TestWorkshopMapDiscovery()
{
    Expect(
        nskinzcs2::model::MapRemapProfile {}.use_source_radar,
        "map remap should default to the original demo radar");
    Expect(
        nskinzcs2::model::MapRemapProfile {}.radar_black_tolerance == 12 &&
            nskinzcs2::model::MapRemapProfile {}.radar_dark_detail_radius == 12 &&
            nskinzcs2::model::MapRemapProfile {}.radar_background_alpha_percent == 0,
        "map remap should default to the balanced transparent radar mask");
    using nskinzcs2::runtime::IsLikelyAuxiliaryWorkshopMap;
    using nskinzcs2::runtime::IsWorkshopVpkIndexFile;
    Expect(IsWorkshopVpkIndexFile(L"3114174859.vpk"), "self-contained workshop VPK should be scanned");
    Expect(IsWorkshopVpkIndexFile(L"addon_dir.vpk"), "split VPK directory index should be scanned");
    Expect(!IsWorkshopVpkIndexFile(L"addon_000.vpk"), "split VPK data archive should not be scanned as an index");
    Expect(!IsWorkshopVpkIndexFile(L"publish_data.txt"), "non-VPK files should not be scanned as indexes");
    Expect(IsLikelyAuxiliaryWorkshopMap("de_cache_sky_d"), "separate skybox package should be auxiliary");
    Expect(IsLikelyAuxiliaryWorkshopMap("maps/de_cache_3dsky.vpk"), "3D skybox package should be auxiliary");
    Expect(IsLikelyAuxiliaryWorkshopMap("de_map_background"), "background package should be auxiliary");
    Expect(!IsLikelyAuxiliaryWorkshopMap("de_cache_d"), "playable cache package should remain selectable");
    Expect(!IsLikelyAuxiliaryWorkshopMap("de_skyline"), "ordinary map names containing sky should remain selectable");

    using nskinzcs2::runtime::ShouldSuppressSourceMapModelPath;
    using nskinzcs2::runtime::BuildSourceSkyboxResourceRedirect;
    using nskinzcs2::runtime::BuildSourceMapRuntimeDataRedirect;
    constexpr std::string_view nuke_skybox =
        "maps/prefabs/de_nuke/de_nuke_skybox02_d";
    std::array<char, 512> skybox_redirect {};
    std::array<char, 512> runtime_data_redirect {};
    Expect(
        BuildSourceMapRuntimeDataRedirect(
            "maps/de_nuke/entities/default_ents.vents_c",
            "de_nuke",
            "de_nuke_rainy",
            runtime_data_redirect.data(),
            runtime_data_redirect.size()) &&
            std::string_view(runtime_data_redirect.data()) ==
                "maps/de_nuke_rainy/entities/default_ents.vents_c",
        "stale demo entity lumps must redirect to the target map so its light-probe volumes are instantiated");
    Expect(
        !BuildSourceMapRuntimeDataRedirect(
            "MAPS\\DE_NUKE\\LIGHTMAPS\\env_light_probe_volume_atlas.vtex_c",
            "maps/de_nuke.vmap",
            "de_nuke_rainy",
            runtime_data_redirect.data(),
            runtime_data_redirect.size()),
        "Rainy Nuke must retain stock Nuke's compatible source probe atlas for dynamic models");
    Expect(
        BuildSourceMapRuntimeDataRedirect(
            "maps/de_nuke_rainy/lightmaps/env_light_probe_volume_atlas.vtex_c",
            "de_nuke",
            "de_nuke_rainy",
            runtime_data_redirect.data(),
            runtime_data_redirect.size()) &&
            std::string_view(runtime_data_redirect.data()) ==
                "maps/de_nuke/lightmaps/env_light_probe_volume_atlas.vtex_c",
        "Rainy Nuke target probe samples must use the verified compatible stock Nuke atlas");
    Expect(
        BuildSourceMapRuntimeDataRedirect(
            "maps/de_mirage/lightmaps/env_light_probe_volume_atlas.vtex_c",
            "de_mirage",
            "nirage",
            runtime_data_redirect.data(),
            runtime_data_redirect.size()) &&
            std::string_view(runtime_data_redirect.data()) ==
                "maps/nirage/lightmaps/env_light_probe_volume_atlas.vtex_c",
        "unverified map pairs must continue using their target probe atlas");
    Expect(
        BuildSourceMapRuntimeDataRedirect(
            "maps/de_nuke/lightmaps/env_light_probe_volume_1372856349_octree.dat",
            "de_nuke",
            "de_nuke_rainy",
            runtime_data_redirect.data(),
            runtime_data_redirect.size()) &&
            std::string_view(runtime_data_redirect.data()) ==
                "maps/de_nuke_rainy/lightmaps/env_light_probe_volume_303904010_octree.dat",
        "Rainy Nuke's first source light-probe octree must map to its verified target handshake");
    Expect(
        BuildSourceMapRuntimeDataRedirect(
            "maps/de_nuke/lightmaps/env_light_probe_volume_1372856443_octree.dat",
            "de_nuke",
            "de_nuke_rainy",
            runtime_data_redirect.data(),
            runtime_data_redirect.size()) &&
            std::string_view(runtime_data_redirect.data()) ==
                "maps/de_nuke_rainy/lightmaps/env_light_probe_volume_303903916_octree.dat",
        "Rainy Nuke's last source light-probe octree must map to its verified target handshake");
    Expect(
        !BuildSourceMapRuntimeDataRedirect(
            "maps/de_mirage/lightmaps/env_light_probe_volume_123_octree.dat",
            "de_mirage",
            "nirage",
            runtime_data_redirect.data(),
            runtime_data_redirect.size()),
        "unverified numeric probe handshakes must never be redirected to a guessed target file");
    Expect(
        BuildSourceMapRuntimeDataRedirect(
            "maps/de_nuke/cubemaps/env_cubemap_array.vtex_c",
            "de_nuke",
            "de_nuke_rainy",
            runtime_data_redirect.data(),
            runtime_data_redirect.size()) &&
            std::string_view(runtime_data_redirect.data()) ==
                "maps/de_nuke_rainy/cubemaps/env_cubemap_array.vtex_c",
        "source cubemap arrays must redirect with the target light-probe volume");
    Expect(
        !BuildSourceMapRuntimeDataRedirect(
            "maps/de_nuke/entities/unnamed_2516.vmdl_c",
            "de_nuke",
            "de_nuke_rainy",
            runtime_data_redirect.data(),
            runtime_data_redirect.size()),
        "map-generated models must stay on the separately guarded suppression path");
    Expect(
        BuildSourceSkyboxResourceRedirect(
            "maps/prefabs/de_mirage/3dskybox_mirage_legacy.vmap",
            "de_mirage",
            "nirage",
            nuke_skybox,
            skybox_redirect.data(),
            skybox_redirect.size()) &&
            std::string_view(skybox_redirect.data()) ==
                "maps/prefabs/de_nuke/de_nuke_skybox02_d.vmap",
        "source skybox entity references must redirect to the target skybox vmap");
    Expect(
        !BuildSourceSkyboxResourceRedirect(
            "E:\\SteamLibrary\\game\\csgo\\maps\\prefabs\\de_mirage\\3dskybox_mirage_legacy.vpk",
            "maps/de_mirage.vmap",
            "nirage",
            nuke_skybox,
            skybox_redirect.data(),
            skybox_redirect.size()),
        "source skybox VPK packages must retain their archive identity");
    Expect(
        !BuildSourceSkyboxResourceRedirect(
            "E:\\SteamLibrary\\game\\csgo\\maps\\prefabs\\de_mirage\\3dskybox_mirage_legacy_dir.vpk",
            "de_mirage",
            "nirage",
            nuke_skybox,
            skybox_redirect.data(),
            skybox_redirect.size()),
        "source VPK directory-index requests must never be redirected");
    Expect(
        !BuildSourceSkyboxResourceRedirect(
            "E:\\SteamLibrary\\game\\csgo\\maps\\prefabs\\de_mirage\\3dskybox_mirage_legacy_flt.vpk",
            "de_mirage",
            "nirage",
            nuke_skybox,
            skybox_redirect.data(),
            skybox_redirect.size()),
        "source VPK filter-stage requests must never be redirected");
    Expect(
        !BuildSourceSkyboxResourceRedirect(
            "maps/prefabs/de_mirage/not_a_skybox.vmdl_c",
            "de_mirage",
            "nirage",
            nuke_skybox,
            skybox_redirect.data(),
            skybox_redirect.size()),
        "ordinary prefab resources must not be redirected as skybox roots");
    Expect(
        !BuildSourceSkyboxResourceRedirect(
            "maps/prefabs/de_mirage/3dskybox_mirage_legacy.vpk",
            "de_mirage",
            "nirage",
            {},
            skybox_redirect.data(),
            skybox_redirect.size()),
        "skybox redirection must stay disabled without a proven target package");
    Expect(
        ShouldSuppressSourceMapModelPath(
            "maps/de_mirage/entities/unnamed_2_60417_63.vmdl",
            "de_mirage",
            "nirage",
            nuke_skybox),
        "source map-generated entity models must be suppressed during remap");
    Expect(
        ShouldSuppressSourceMapModelPath(
            "MAPS\\DE_MIRAGE\\WORLDNODES\\n0_lr0_agg.vmdl_c",
            "maps/de_mirage.vmap",
            "nirage",
            nuke_skybox),
        "source world-node models must be suppressed case-insensitively");
    Expect(
        ShouldSuppressSourceMapModelPath(
            "maps/prefabs/de_mirage/3dskybox_mirage_legacy/worldnodes/node000_world_lr0_agg6_3_towertop_d.vmdl_c",
            "de_mirage",
            "nirage",
            nuke_skybox),
        "a proven-different target skybox must suppress the demo map tower model");
    Expect(
        !ShouldSuppressSourceMapModelPath(
            "maps/prefabs/de_mirage/3dskybox_mirage_legacy/worldnodes/node000_world.vmdl_c",
            "de_mirage",
            "de_mirage_water",
            "maps/prefabs/de_mirage/3dskybox_mirage_legacy"),
        "a target that intentionally shares the source skybox must keep it");
    Expect(
        !ShouldSuppressSourceMapModelPath(
            "models/props/de_mirage/shutter_window_l_breakable.vmdl",
            "de_mirage",
            "nirage",
            nuke_skybox),
        "ordinary prop models must never be blanket-filtered");
    Expect(
        !ShouldSuppressSourceMapModelPath(
            "maps/nirage/worldnodes/n0_lr0_agg.vmdl_c",
            "de_mirage",
            "nirage",
            nuke_skybox),
        "target map world-node models must remain untouched");
    Expect(
        !ShouldSuppressSourceMapModelPath(
            "maps/prefabs/de_mirage/3dskybox_mirage_legacy.vpk",
            "de_mirage",
            "nirage",
            nuke_skybox),
        "source skybox packages must remain loadable because Source 2 treats them as required resources");

    const auto result = nskinzcs2::runtime::DiscoverSubscribedWorkshopMaps();
    if (result.entries.empty())
    {
        std::cout << "workshop_map_discovery=SKIPPED (no installed CS2 workshop maps found)\n";
        return;
    }

    for (const auto& entry : result.entries)
    {
        Expect(!entry.workshop_id.empty(), "workshop entry should contain an item id");
        Expect(
            std::all_of(entry.workshop_id.begin(), entry.workshop_id.end(), [](const unsigned char character)
            {
                return std::isdigit(character) != 0;
            }),
            "workshop item id should be numeric");
        Expect(!entry.title.empty(), "workshop entry should contain a title");
        Expect(!entry.addon_name.empty(), "workshop entry should contain an addon folder");
        Expect(!entry.map_name.empty(), "workshop entry should contain a VPK map name");
        Expect(!IsLikelyAuxiliaryWorkshopMap(entry.map_name), "auxiliary workshop maps should be filtered");
        Expect(std::filesystem::is_directory(entry.item_directory), "workshop item directory should exist");
        if (entry.workshop_id == "3573314592" && entry.map_name == "nirage")
        {
            Expect(
                entry.skybox_resource_prefix == "maps/prefabs/de_nuke/de_nuke_skybox02_d",
                "nirage must expose its Nuke 3D skybox package to the remap runtime");
        }
    }
    std::cout << "workshop_map_entries=" << result.entries.size() << '\n';

    const auto nirage = nskinzcs2::runtime::ReadWorkshopRadarTexture("3573314592", "nirage");
    if (nirage.has_value())
    {
        nskinzcs2::runtime::RadarAlphaOverlay overlay;
        std::wstring diagnostic;
        const auto outcome = nskinzcs2::runtime::BuildRadarAlphaOverlayFromVtex(
            nirage->virtual_path,
            nirage->bytes,
            overlay,
            diagnostic);
        if (outcome != nskinzcs2::runtime::RadarAlphaFixOutcome::Generated)
        {
            std::wcout << L"generic_radar_overlay_diagnostic=" << diagnostic << L'\n';
        }
        Expect(
            outcome == nskinzcs2::runtime::RadarAlphaFixOutcome::Generated,
            "opaque nirage Workshop radar should produce a generic transparent overlay");
        Expect(overlay.width == 664 && overlay.height == 664, "nirage overlay should retain its source dimensions");
        Expect(!overlay.png_bytes.empty(), "generic radar overlay should contain the masked PNG source");
        Expect(
            overlay.request_virtual_path == nirage->virtual_path,
            "generic radar overlay should retain the Workshop request path");
        Expect(
            overlay.virtual_path != overlay.request_virtual_path &&
                overlay.virtual_path.starts_with("panorama/images/overheadmaps/nskinz_") &&
                overlay.virtual_path.ends_with("_radar_tga.vtex_c"),
            "generic radar overlay should use a content-addressed native resource alias");
        Expect(overlay.vtex_bytes.empty(), "an uncompiled in-memory VTEX must never be exposed to CS2");
        Expect(
            overlay.transparent_pixels > overlay.visible_pixels,
            "nirage overlay should make the opaque background transparent");
        Expect(
            overlay.visible_pixels >= 97600,
            "nirage overlay should retain coloured interior details and the complete dark connecting road");
        Expect(
            overlay.softened_edge_pixels >= 50,
            "nirage overlay should recover the near-black boundary as translucent coloured pixels instead of deleting it");
        WriteRadarPreviewIfRequested("nirage-default", overlay);

        nskinzcs2::runtime::RadarAlphaMaskSettings no_dark_bridge;
        no_dark_bridge.dark_detail_radius = 0;
        nskinzcs2::runtime::RadarAlphaOverlay no_dark_bridge_overlay;
        auto adjustable_outcome = nskinzcs2::runtime::BuildRadarAlphaOverlayFromVtex(
            nirage->virtual_path,
            nirage->bytes,
            no_dark_bridge_overlay,
            diagnostic,
            no_dark_bridge);
        Expect(
            adjustable_outcome == nskinzcs2::runtime::RadarAlphaFixOutcome::Generated,
            "nirage radar should support disabling dark-road recovery");
        Expect(
            no_dark_bridge_overlay.virtual_path != overlay.virtual_path,
            "changing radar recovery settings should publish a different cache-isolated resource alias");
        Expect(
            overlay.visible_pixels >= no_dark_bridge_overlay.visible_pixels,
            "dark-road recovery must never reduce retained map coverage");

        nskinzcs2::runtime::RadarAlphaMaskSettings translucent_background;
        translucent_background.background_alpha_percent = 25;
        nskinzcs2::runtime::RadarAlphaOverlay translucent_background_overlay;
        adjustable_outcome = nskinzcs2::runtime::BuildRadarAlphaOverlayFromVtex(
            nirage->virtual_path,
            nirage->bytes,
            translucent_background_overlay,
            diagnostic,
            translucent_background);
        Expect(
            adjustable_outcome == nskinzcs2::runtime::RadarAlphaFixOutcome::Generated,
            "nirage radar should support a manually adjustable background opacity");
        Expect(
            translucent_background_overlay.visible_pixels == overlay.visible_pixels,
            "background opacity must not change the classified map-road mask");
        Expect(
            translucent_background_overlay.transparent_pixels < overlay.transparent_pixels,
            "raising background opacity should reduce fully transparent pixels without deleting roads");
        WriteRadarPreviewIfRequested("nirage-background-25", translucent_background_overlay);

        const auto nirage_entry = std::find_if(result.entries.begin(), result.entries.end(), [](const auto& entry)
        {
            return entry.workshop_id == "3573314592" && entry.map_name == "nirage";
        });
        Expect(
            nirage_entry != result.entries.end(),
            "the installed nirage radar audit should have a discoverable Workshop entry");
        auto steamapps = nirage_entry->item_directory;
        for (int level = 0; level < 4; ++level)
        {
            steamapps = steamapps.parent_path();
        }
        const auto cs2_install_root = steamapps / L"common/Counter-Strike Global Offensive";
        Expect(
            nskinzcs2::runtime::CompileRadarOverlayWithCs2(cs2_install_root, overlay, diagnostic),
            "the masked radar PNG should compile through CS2's native resourcecompiler");
        Expect(overlay.vtex_bytes.size() >= 64, "the native compiled radar VTEX should not be empty");
        std::uint32_t compiled_block_count = 0;
        std::uint32_t first_block = 0;
        std::memcpy(&compiled_block_count, overlay.vtex_bytes.data() + 12, sizeof(compiled_block_count));
        std::memcpy(&first_block, overlay.vtex_bytes.data() + 16, sizeof(first_block));
        Expect(compiled_block_count >= 2, "native radar VTEX should contain full Source 2 resource blocks");
        Expect(first_block == 0x32444552u, "native radar VTEX should start with the RED2 metadata block");
        std::filesystem::path materialized_root;
        std::filesystem::path materialized_file;
        Expect(
            nskinzcs2::runtime::MaterializeCompiledRadarOverlay(
                overlay,
                materialized_root,
                materialized_file,
                diagnostic),
            "the native radar VTEX should materialize into an overlay-only cache");
        Expect(
            materialized_file == materialized_root / std::filesystem::path(overlay.virtual_path),
            "materialized radar file should match its virtual path under the overlay root");
        Expect(
            nskinzcs2::runtime::ValidateNativeRadarVtexFile(materialized_file, diagnostic),
            "the injected-runtime validation should accept the materialized native radar VTEX");
        auto failed_native_compile = overlay;
        failed_native_compile.vtex_bytes.assign(64, 0xCC);
        Expect(
            !nskinzcs2::runtime::CompileRadarOverlayWithCs2(
                cs2_install_root / L"missing-install-root",
                failed_native_compile,
                diagnostic),
            "a missing CS2 native compiler must fail closed");
        Expect(
            failed_native_compile.vtex_bytes.empty(),
            "native compiler failure must clear all candidate VTEX bytes before map loading");
        std::cout << "generic_radar_overlay=" << overlay.width << 'x' << overlay.height
                  << " visible=" << overlay.visible_pixels
                  << " transparent=" << overlay.transparent_pixels
                  << " soft_edge=" << overlay.softened_edge_pixels
                  << " recovered_dark_detail=" << overlay.recovered_detail_pixels
                  << " native_vtex=" << overlay.vtex_bytes.size() << '\n';
    }
    else
    {
        std::cout << "generic_radar_overlay=SKIPPED (nirage Workshop item not installed)\n";
    }

    constexpr std::array<std::string_view, 6> additional_radar_maps {
        "de_all_in_one",
        "de_ancient_night",
        "de_mirage_water",
        "de_dust2_water",
        "de_inferno_water",
        "de_nuke_rainy",
    };
    for (const auto map_name : additional_radar_maps)
    {
        const auto entry = std::find_if(result.entries.begin(), result.entries.end(), [&](const auto& candidate)
        {
            return candidate.map_name == map_name;
        });
        if (entry == result.entries.end())
        {
            continue;
        }
        std::wstring radar_diagnostic;
        const auto radar = nskinzcs2::runtime::ReadWorkshopRadarTexture(
            entry->workshop_id,
            entry->map_name,
            0,
            &radar_diagnostic);
        if (!radar.has_value())
        {
            std::wcout << L"generic_radar_map=" << std::wstring(map_name.begin(), map_name.end())
                       << L" source=missing diagnostic=" << radar_diagnostic << L'\n';
        }
        Expect(radar.has_value(), "an installed audited workshop map should expose its own radar texture");
        nskinzcs2::runtime::RadarAlphaOverlay map_overlay;
        const auto map_outcome = nskinzcs2::runtime::BuildRadarAlphaOverlayFromVtex(
            radar->virtual_path,
            radar->bytes,
            map_overlay,
            radar_diagnostic);
        Expect(
            map_outcome == nskinzcs2::runtime::RadarAlphaFixOutcome::Generated ||
                map_outcome == nskinzcs2::runtime::RadarAlphaFixOutcome::NotNeeded,
            "each installed audited workshop map radar should be generated or already transparent");
        WriteRadarPreviewIfRequested(std::string(map_name) + "-default", map_overlay);
        if (map_name == "de_ancient_night")
        {
            std::wcout << L"generic_radar_map=de_ancient_night auto_tolerance="
                       << map_overlay.applied_black_tolerance
                       << L" neutral_mode=" << map_overlay.detected_neutral_mode
                       << L" neutral_pixels=" << map_overlay.detected_neutral_pixels
                       << L" neutral_mode_pixels=" << map_overlay.detected_neutral_mode_pixels
                       << L" visible=" << map_overlay.visible_pixels << L'\n';
            Expect(
                map_overlay.applied_black_tolerance >= 26 &&
                    map_overlay.applied_black_tolerance <= 32,
                "Ancient Night should auto-detect its brighter opaque neutral grid");
            Expect(
                map_overlay.visible_pixels >= 270000 && map_overlay.visible_pixels <= 285000,
                "Ancient Night should retain its roads without retaining the opaque editor grid");
        }
        if (map_name == "de_inferno_water")
        {
            nskinzcs2::runtime::RadarAlphaMaskSettings no_dark_recovery;
            no_dark_recovery.dark_detail_radius = 0;
            nskinzcs2::runtime::RadarAlphaOverlay no_dark_recovery_overlay;
            const auto no_dark_recovery_outcome =
                nskinzcs2::runtime::BuildRadarAlphaOverlayFromVtex(
                    radar->virtual_path,
                    radar->bytes,
                    no_dark_recovery_overlay,
                    radar_diagnostic,
                    no_dark_recovery);
            Expect(
                no_dark_recovery_outcome == nskinzcs2::runtime::RadarAlphaFixOutcome::Generated,
                "Inferno Water should support disabling dark-detail recovery");
            WriteRadarPreviewIfRequested(
                "de_inferno_water-no-dark-recovery",
                no_dark_recovery_overlay);
            std::wcout << L"generic_radar_map=de_inferno_water no_dark_recovery visible="
                       << no_dark_recovery_overlay.visible_pixels
                       << L" transparent=" << no_dark_recovery_overlay.transparent_pixels
                       << L'\n';
            Expect(
                map_overlay.visible_pixels >= no_dark_recovery_overlay.visible_pixels + 5000,
                "Inferno Water dark-road radius should visibly restore bounded road pixels");
            Expect(
                map_overlay.virtual_path != no_dark_recovery_overlay.virtual_path,
                "changing Inferno Water recovery radius should change its cache-isolated alias");

            nskinzcs2::runtime::RadarAlphaMaskSettings lower_tolerance;
            lower_tolerance.black_tolerance = 0;
            lower_tolerance.dark_detail_radius = 12;
            nskinzcs2::runtime::RadarAlphaOverlay lower_tolerance_overlay;
            const auto lower_tolerance_outcome =
                nskinzcs2::runtime::BuildRadarAlphaOverlayFromVtex(
                    radar->virtual_path,
                    radar->bytes,
                    lower_tolerance_overlay,
                    radar_diagnostic,
                    lower_tolerance);
            Expect(
                lower_tolerance_outcome == nskinzcs2::runtime::RadarAlphaFixOutcome::Generated,
                "Inferno Water should support a road-preserving low tolerance");

            nskinzcs2::runtime::RadarAlphaMaskSettings higher_tolerance;
            higher_tolerance.black_tolerance = 36;
            higher_tolerance.dark_detail_radius = 12;
            nskinzcs2::runtime::RadarAlphaOverlay higher_tolerance_overlay;
            const auto higher_tolerance_outcome =
                nskinzcs2::runtime::BuildRadarAlphaOverlayFromVtex(
                    radar->virtual_path,
                    radar->bytes,
                    higher_tolerance_overlay,
                    radar_diagnostic,
                    higher_tolerance);
            Expect(
                higher_tolerance_outcome == nskinzcs2::runtime::RadarAlphaFixOutcome::Generated,
                "Inferno Water should support a stronger black-grid tolerance");
            Expect(
                lower_tolerance_overlay.applied_black_tolerance <
                    higher_tolerance_overlay.applied_black_tolerance &&
                lower_tolerance_overlay.visible_pixels > higher_tolerance_overlay.visible_pixels + 1000,
                "Inferno Water black tolerance slider should materially change retained pixels");
            Expect(
                lower_tolerance_overlay.virtual_path != higher_tolerance_overlay.virtual_path,
                "changing Inferno Water tolerance should publish a new cache-isolated alias");
            WriteRadarPreviewIfRequested(
                "de_inferno_water-tolerance-0",
                lower_tolerance_overlay);
            WriteRadarPreviewIfRequested(
                "de_inferno_water-tolerance-36",
                higher_tolerance_overlay);
            std::wcout << L"generic_radar_map=de_inferno_water tolerance_slider low="
                       << lower_tolerance_overlay.applied_black_tolerance
                       << L"/" << lower_tolerance_overlay.visible_pixels
                       << L" high=" << higher_tolerance_overlay.applied_black_tolerance
                       << L"/" << higher_tolerance_overlay.visible_pixels << L'\n';
        }
        if (map_name == "de_nuke_rainy")
        {
            nskinzcs2::runtime::RadarAlphaMaskSettings oversized_recovery;
            oversized_recovery.black_tolerance = 24;
            oversized_recovery.dark_detail_radius = 16;
            oversized_recovery.background_alpha_percent = 0;
            nskinzcs2::runtime::RadarAlphaOverlay oversized_recovery_overlay;
            const auto oversized_recovery_outcome =
                nskinzcs2::runtime::BuildRadarAlphaOverlayFromVtex(
                    radar->virtual_path,
                    radar->bytes,
                    oversized_recovery_overlay,
                    radar_diagnostic,
                    oversized_recovery);
            Expect(
                oversized_recovery_outcome == nskinzcs2::runtime::RadarAlphaFixOutcome::Generated,
                "Rainy Nuke radar should support a legacy oversized recovery setting safely");
            Expect(
                oversized_recovery_overlay.recovered_detail_pixels <=
                    map_overlay.recovered_detail_pixels + 1500,
                "an oversized recovery setting must not bridge long black grid lines back into the map");
            WriteRadarPreviewIfRequested(
                "de_nuke_rainy-oversized-recovery",
                oversized_recovery_overlay);

            nskinzcs2::runtime::RadarAlphaMaskSettings maximum_tolerance;
            maximum_tolerance.black_tolerance = 96;
            maximum_tolerance.dark_detail_radius = 4;
            maximum_tolerance.background_alpha_percent = 0;
            nskinzcs2::runtime::RadarAlphaOverlay maximum_tolerance_overlay;
            const auto maximum_tolerance_outcome =
                nskinzcs2::runtime::BuildRadarAlphaOverlayFromVtex(
                    radar->virtual_path,
                    radar->bytes,
                    maximum_tolerance_overlay,
                    radar_diagnostic,
                    maximum_tolerance);
            Expect(
                maximum_tolerance_outcome == nskinzcs2::runtime::RadarAlphaFixOutcome::Generated,
                "Rainy Nuke radar should remain usable at maximum black-grid tolerance");
            Expect(
                maximum_tolerance_overlay.visible_pixels >= 40000,
                "maximum Rainy Nuke tolerance must retain a usable map-road mask");
            WriteRadarPreviewIfRequested("de_nuke_rainy-max", maximum_tolerance_overlay);
            std::wcout << L"generic_radar_map=de_nuke_rainy-max"
                       << L" visible=" << maximum_tolerance_overlay.visible_pixels
                       << L" transparent=" << maximum_tolerance_overlay.transparent_pixels
                       << L" recovered_dark_detail=" << maximum_tolerance_overlay.recovered_detail_pixels
                       << L" diagnostic=" << radar_diagnostic << L'\n';
        }
        std::wcout << L"generic_radar_map=" << std::wstring(map_name.begin(), map_name.end())
                   << L" source=" << std::wstring(radar->virtual_path.begin(), radar->virtual_path.end())
                   << L" outcome=" << static_cast<int>(map_outcome)
                   << L" visible=" << map_overlay.visible_pixels
                   << L" transparent=" << map_overlay.transparent_pixels
                   << L" recovered_dark_detail=" << map_overlay.recovered_detail_pixels
                   << L" applied_black_tolerance=" << map_overlay.applied_black_tolerance
                   << L" neutral_mode=" << map_overlay.detected_neutral_mode
                   << L" diagnostic=" << radar_diagnostic << L'\n';
    }
}

void TestCosmeticWearPolicy()
{
    using nskinzcs2::model::CosmeticWearMatchesExactly;
    using nskinzcs2::model::NormalizeCosmeticWear;
    using nskinzcs2::model::kMaximumCosmeticWear;
    using nskinzcs2::model::kMinimumCosmeticWear;

    Expect(
        CosmeticWearMatchesExactly(NormalizeCosmeticWear(0.0f), kMinimumCosmeticWear),
        "zero wear should normalize to the shared cosmetic minimum");
    Expect(
        CosmeticWearMatchesExactly(NormalizeCosmeticWear(-0.5f), kMinimumCosmeticWear),
        "negative wear should normalize to the shared cosmetic minimum");
    Expect(
        CosmeticWearMatchesExactly(NormalizeCosmeticWear(2.0f), kMaximumCosmeticWear),
        "wear above one should normalize to the shared cosmetic maximum");
    Expect(
        CosmeticWearMatchesExactly(NormalizeCosmeticWear(0.25f), 0.25f),
        "wear inside the supported range should be preserved");
    Expect(
        CosmeticWearMatchesExactly(
            NormalizeCosmeticWear(std::numeric_limits<float>::quiet_NaN()),
            kMinimumCosmeticWear),
        "non-finite wear should normalize to the shared cosmetic minimum");
    Expect(
        !CosmeticWearMatchesExactly(0.0f, kMinimumCosmeticWear),
        "exact wear comparison must distinguish zero from the configured minimum");
    Expect(
        !CosmeticWearMatchesExactly(
            std::nextafter(kMinimumCosmeticWear, kMaximumCosmeticWear),
            kMinimumCosmeticWear),
        "exact wear comparison must distinguish adjacent float values");

    namespace fs = std::filesystem;
    const auto load_path = fs::temp_directory_path() / "nSkinzCS2.wear.load.test.json";
    {
        std::ofstream stream(load_path, std::ios::trunc);
        stream << R"json({
  "version": 4,
  "profiles": [
    { "name": "zero", "enabled": true, "xuid": 1, "base_item": 7, "wear": 0.0 },
    { "name": "negative", "enabled": true, "xuid": 2, "base_item": 9, "wear": -0.5 },
    { "name": "above", "enabled": true, "xuid": 3, "base_item": 16, "wear": 2.0 },
    { "name": "inside", "enabled": true, "xuid": 4, "base_item": 17, "wear": 0.25 }
  ],
  "glove_profiles": [
    { "name": "default gloves", "enabled": true, "xuid": 5, "mode": "model_and_finish", "model_definition": 5030, "paint_kit": 10018, "wear": 0.0 }
  ]
})json";
    }

    const auto loaded = nskinzcs2::ConfigStore::Load(load_path);
    Expect(loaded.profiles.size() == 4, "wear policy config should preserve weapon profiles");
    Expect(
        CosmeticWearMatchesExactly(loaded.profiles[0].wear, kMinimumCosmeticWear),
        "loaded zero weapon wear should clamp to the shared minimum");
    Expect(
        CosmeticWearMatchesExactly(loaded.profiles[1].wear, kMinimumCosmeticWear),
        "loaded negative weapon wear should clamp to the shared minimum");
    Expect(
        CosmeticWearMatchesExactly(loaded.profiles[2].wear, kMaximumCosmeticWear),
        "loaded weapon wear above one should clamp to the shared maximum");
    Expect(
        CosmeticWearMatchesExactly(loaded.profiles[3].wear, 0.25f),
        "loaded weapon wear inside the supported range should be preserved");
    Expect(
        !loaded.glove_profiles.empty() &&
            CosmeticWearMatchesExactly(loaded.glove_profiles[0].wear, kMinimumCosmeticWear),
        "loaded zero glove wear should clamp to the shared minimum");
    fs::remove(load_path);

    const auto save_path = fs::temp_directory_path() / "nSkinzCS2.wear.save.test.json";
    nskinzcs2::model::ConfigDocument unbounded_document;
    nskinzcs2::model::CosmeticProfile unbounded_weapon;
    unbounded_weapon.wear = 0.0f;
    unbounded_document.profiles.push_back(unbounded_weapon);
    nskinzcs2::model::GloveProfile unbounded_glove;
    unbounded_glove.wear = 5.0f;
    unbounded_document.glove_profiles.push_back(unbounded_glove);
    Expect(nskinzcs2::ConfigStore::Save(save_path, unbounded_document), "wear policy config save should succeed");
    const auto saved = nskinzcs2::ConfigStore::Load(save_path);
    Expect(
        !saved.profiles.empty() &&
            CosmeticWearMatchesExactly(saved.profiles[0].wear, kMinimumCosmeticWear),
        "saved weapon wear should be normalized");
    Expect(
        !saved.glove_profiles.empty() &&
            CosmeticWearMatchesExactly(saved.glove_profiles[0].wear, kMaximumCosmeticWear),
        "saved glove wear should be normalized");
    fs::remove(save_path);
}

void TestProfileMatching()
{
    nskinzcs2::model::ConfigDocument document;

    nskinzcs2::model::CosmeticProfile first;
    first.name = "AK";
    first.enabled = true;
    first.xuid = 10;
    first.base_item = 7;
    document.profiles.push_back(first);

    nskinzcs2::model::CosmeticProfile second;
    second.name = "AWP";
    second.enabled = true;
    second.xuid = 10;
    second.base_item = 9;
    document.profiles.push_back(second);

    const auto* match = nskinzcs2::ConfigStore::FindMatchingProfile(document, 10, 9);
    Expect(match != nullptr, "profile lookup should find matching profile");
    Expect(match->name == "AWP", "profile lookup should choose correct base item");
}

void TestKeychainConfigAndMatching()
{
    namespace fs = std::filesystem;
    const auto temp_path = fs::temp_directory_path() / "nSkinzCS2.keychain.config.test.json";

    nskinzcs2::model::ConfigDocument document;

    nskinzcs2::model::KeychainProfile first;
    first.name = "AK Charm Old";
    first.enabled = true;
    first.xuid = 10;
    first.base_item = 7;
    first.keychain_kit = 1001;
    document.keychain_profiles.push_back(first);

    nskinzcs2::model::KeychainProfile second = first;
    second.name = "AK Charm New";
    second.keychain_kit = 1002;
    document.keychain_profiles.push_back(second);

    nskinzcs2::model::KeychainProfile awp;
    awp.name = "AWP Charm";
    awp.enabled = true;
    awp.xuid = 10;
    awp.base_item = 9;
    awp.keychain_kit = 2001;
    document.keychain_profiles.push_back(awp);

    Expect(nskinzcs2::ConfigStore::Save(temp_path, document), "keychain config save should succeed");
    const auto loaded = nskinzcs2::ConfigStore::Load(temp_path);

    Expect(loaded.version == 7, "keychain config should round-trip version 7");
    const auto* ak_match = nskinzcs2::ConfigStore::FindMatchingKeychainProfile(loaded, 10, 7);
    Expect(ak_match != nullptr, "keychain profile lookup should find the configured weapon binding");
    Expect(ak_match->name == "AK Charm New", "duplicate keychain bindings should keep the last entry");
    Expect(ak_match->keychain_kit == 1002, "duplicate keychain bindings should keep the last kit id");

    const auto* awp_match = nskinzcs2::ConfigStore::FindMatchingKeychainProfile(loaded, 10, 9);
    Expect(awp_match != nullptr, "keychain profile lookup should distinguish different weapons");
    Expect(awp_match->keychain_kit == 2001, "keychain lookup should preserve per-weapon bindings");

    nskinzcs2::model::KeychainProfile no_op;
    no_op.enabled = true;
    no_op.xuid = 11;
    no_op.base_item = 7;
    auto matching_document = loaded;
    matching_document.keychain_profiles.push_back(no_op);
    Expect(
        nskinzcs2::ConfigStore::FindMatchingKeychainProfile(matching_document, 11, 7) == nullptr,
        "dash keychain intent should not create a runtime match");
    matching_document.keychain_profiles.back().explicit_clear = true;
    Expect(
        nskinzcs2::ConfigStore::FindMatchingKeychainProfile(matching_document, 11, 7) != nullptr,
        "explicit keychain clear intent should create a runtime match");

    fs::remove(temp_path);
}

void TestGloveConfigAndMatching()
{
    namespace fs = std::filesystem;
    const auto temp_path = fs::temp_directory_path() / "nSkinzCS2.glove.config.test.json";

    nskinzcs2::model::ConfigDocument document;

    nskinzcs2::model::CosmeticProfile weapon_profile;
    weapon_profile.name = "AK";
    weapon_profile.enabled = true;
    weapon_profile.xuid = 10;
    weapon_profile.base_item = 7;
    document.profiles.push_back(weapon_profile);

    nskinzcs2::model::CosmeticProfile legacy_glove_profile;
    legacy_glove_profile.name = "Legacy glove";
    legacy_glove_profile.enabled = true;
    legacy_glove_profile.xuid = 10;
    legacy_glove_profile.base_item = 5028;
    legacy_glove_profile.paint_kit = 10018;
    document.profiles.push_back(legacy_glove_profile);

    nskinzcs2::model::GloveProfile glove_profile;
    glove_profile.name = "Pandora";
    glove_profile.enabled = true;
    glove_profile.xuid = 10;
    glove_profile.mode = nskinzcs2::model::GloveProfileMode::ModelAndFinish;
    glove_profile.model_definition = 5030;
    glove_profile.paint_kit = 10018;
    glove_profile.quality = 3;
    glove_profile.seed = 123;
    glove_profile.wear = 0.2f;
    document.glove_profiles.push_back(glove_profile);

    Expect(nskinzcs2::ConfigStore::Save(temp_path, document), "glove config save should succeed");
    const auto loaded = nskinzcs2::ConfigStore::Load(temp_path);

    Expect(loaded.version == 7, "glove config should round-trip version 7");
    Expect(!loaded.glove_profiles.empty(), "glove config should contain glove profiles");
    Expect(loaded.glove_profiles[0].mode == nskinzcs2::model::GloveProfileMode::ModelAndFinish, "glove mode should round-trip");
    Expect(loaded.glove_profiles[0].model_definition == 5030, "glove model should round-trip");
    Expect(loaded.glove_profiles[0].paint_kit == 10018, "glove paint should round-trip");

    const auto* weapon_match = nskinzcs2::ConfigStore::FindMatchingProfile(loaded, 10, 7);
    Expect(weapon_match != nullptr, "weapon profile lookup should still succeed");
    const auto* legacy_glove_match = nskinzcs2::ConfigStore::FindMatchingProfile(loaded, 10, 5028);
    Expect(legacy_glove_match == nullptr, "legacy glove profiles in profiles must be ignored");
    const auto* glove_match = nskinzcs2::ConfigStore::FindMatchingGloveProfile(loaded, 10);
    Expect(glove_match != nullptr, "glove profile lookup should use glove_profiles");
    Expect(glove_match->name == "Pandora", "glove lookup should return the glove profile");

    fs::remove(temp_path);
}

void TestKnifeProfileConfigMigration()
{
    namespace fs = std::filesystem;
    const auto temp_path = fs::temp_directory_path() / "nSkinzCS2.knife.config.test.json";

    {
        std::ofstream stream(temp_path, std::ios::trunc);
        stream << R"json({
  "version": 1,
  "profiles": [
    { "name": "AK", "enabled": true, "xuid": 10, "base_item": 7 },
    { "name": "Legacy Knife Old", "enabled": true, "xuid": 11, "base_item": 42, "override_item": 507, "paint_kit": 344 },
    { "name": "Legacy Knife New", "enabled": true, "xuid": 11, "base_item": 42, "override_item": 0, "paint_kit": 345 },
    { "name": "Legacy Knife Placeholder", "enabled": true, "xuid": 0, "base_item": 42, "override_item": 515, "paint_kit": 12 }
  ]
})json";
    }

    auto loaded = nskinzcs2::ConfigStore::Load(temp_path);
    Expect(loaded.version == 7, "legacy knife config should migrate to version 7");
    Expect(loaded.profiles.size() == 1, "legacy knife entries should be removed from weapon profiles");
    Expect(loaded.profiles[0].base_item == 7, "non-knife weapon entries should remain after migration");
    Expect(
        !loaded.profiles[0].explicit_vanilla_finish,
        "legacy paint_kit=0 entries should remain dash/no-modify rather than becoming vanilla intent");

    const auto* migrated_knife = nskinzcs2::ConfigStore::FindMatchingKnifeProfile(loaded, 11);
    Expect(migrated_knife != nullptr, "legacy knife profile should migrate into knife_profiles");
    Expect(
        migrated_knife->mode == nskinzcs2::model::KnifeProfileMode::FinishOnly,
        "duplicate legacy knife migration should keep the last entry");
    Expect(migrated_knife->paint_kit == 345, "duplicate legacy knife migration should keep the last paint");
    Expect(
        nskinzcs2::ConfigStore::FindMatchingKnifeProfile(loaded, 0) == nullptr,
        "xuid=0 legacy knife placeholders should not migrate");

    {
        std::ofstream stream(temp_path, std::ios::trunc);
        stream << R"json({
  "version": 3,
  "profiles": [
    { "name": "Legacy Knife", "enabled": true, "xuid": 20, "base_item": 42, "override_item": 515, "paint_kit": 12 },
    { "name": "Legacy Knife Extra", "enabled": true, "xuid": 21, "base_item": 42, "override_item": 0, "paint_kit": 44, "stickers": [ { "kit_id": 17 } ] }
  ],
  "knife_profiles": [
    { "name": "Explicit Knife", "enabled": true, "xuid": 20, "mode": "model_and_finish", "model_definition": 507, "paint_kit": 38, "stickers": [ { "kit_id": 18 } ] }
  ]
})json";
    }

    loaded = nskinzcs2::ConfigStore::Load(temp_path);
    Expect(loaded.version == 7, "explicit knife config should normalize to version 7");
    const auto* explicit_knife = nskinzcs2::ConfigStore::FindMatchingKnifeProfile(loaded, 20);
    Expect(explicit_knife != nullptr, "explicit knife profile should remain available");
    Expect(explicit_knife->name == "Explicit Knife", "explicit knife_profiles entry should beat legacy knife profiles");
    Expect(
        explicit_knife->mode == nskinzcs2::model::KnifeProfileMode::ModelAndFinish,
        "explicit knife mode should survive migration");
    Expect(explicit_knife->model_definition == 507, "explicit knife model should survive migration");
    const auto* migrated_extra_knife = nskinzcs2::ConfigStore::FindMatchingKnifeProfile(loaded, 21);
    Expect(migrated_extra_knife != nullptr, "non-conflicting legacy knife entries should still migrate");
    Expect(
        migrated_extra_knife->paint_kit == 44,
        "non-conflicting legacy knife migration should preserve finish fields");

    loaded.knife_profiles.push_back(nskinzcs2::model::KnifeProfile {});
    loaded.knife_profiles.back().enabled = true;
    loaded.knife_profiles.back().xuid = 33;
    loaded.knife_profiles.back().name = "Duplicate Old";
    loaded.knife_profiles.back().mode = nskinzcs2::model::KnifeProfileMode::FinishOnly;
    loaded.knife_profiles.push_back(nskinzcs2::model::KnifeProfile {});
    loaded.knife_profiles.back().enabled = true;
    loaded.knife_profiles.back().xuid = 33;
    loaded.knife_profiles.back().name = "Duplicate New";
    loaded.knife_profiles.back().mode = nskinzcs2::model::KnifeProfileMode::ModelAndFinish;
    loaded.knife_profiles.back().model_definition = 523;
    const auto* staged_duplicate = nskinzcs2::ConfigStore::FindMatchingKnifeProfile(loaded, 33);
    Expect(staged_duplicate != nullptr, "duplicate in-memory knife profile should remain available");
    Expect(staged_duplicate->name == "Duplicate New", "in-memory duplicate knife lookup should keep the last profile");
    Expect(
        staged_duplicate->model_definition == 523,
        "in-memory duplicate knife lookup should keep the last model definition");

    Expect(nskinzcs2::ConfigStore::Save(temp_path, loaded), "deduplicated knife config save should succeed");
    const auto saved = nskinzcs2::ConfigStore::Load(temp_path);
    const auto* deduplicated = nskinzcs2::ConfigStore::FindMatchingKnifeProfile(saved, 33);
    Expect(deduplicated != nullptr, "saved knife profile should remain available");
    Expect(deduplicated->name == "Duplicate New", "save should keep the last duplicate knife profile");
    Expect(
        deduplicated->mode == nskinzcs2::model::KnifeProfileMode::ModelAndFinish,
        "save should keep the last duplicate knife profile mode");

    const auto saved_text =
        [&temp_path]()
        {
            std::ifstream stream(temp_path);
            return std::string(
                std::istreambuf_iterator<char>(stream),
                std::istreambuf_iterator<char>());
        }();
    Expect(saved_text.find("\"knife_profiles\"") != std::string::npos, "saved config should contain knife_profiles");
    Expect(saved_text.find("\"base_item\": 42") == std::string::npos, "saved config should not write legacy knife profiles");
    const auto knife_profiles_pos = saved_text.find("\"knife_profiles\"");
    const auto profiles_pos = saved_text.find("\"profiles\"", knife_profiles_pos + std::strlen("\"knife_profiles\""));
    const auto knife_section =
        knife_profiles_pos == std::string::npos
            ? std::string {}
            : saved_text.substr(
                  knife_profiles_pos,
                  profiles_pos == std::string::npos
                      ? std::string::npos
                      : profiles_pos - knife_profiles_pos);
    Expect(
        knife_section.find("\"stickers\"") == std::string::npos,
        "saved knife profiles should not write knife stickers");

    fs::remove(temp_path);
}

void TestCatalogFallback()
{
    const auto catalog = nskinzcs2::CatalogBuilder::BuildFallbackCatalog();
    Expect(!catalog.base_items.empty(), "fallback catalog should contain base items");
    Expect(!catalog.weapon_paint_kits.empty(), "fallback catalog should contain paint kits");
    Expect(!catalog.glove_paint_compatibility.empty(), "fallback catalog should expose glove paint compatibility");
    Expect(!catalog.keychain_kits.empty(), "fallback catalog should contain keychain entries");
    Expect(!catalog.runtime_populated, "fallback catalog should not claim runtime population");
    Expect(!catalog.warnings.empty(), "fallback catalog should surface warnings");

    const auto bayonet_visual = catalog.visuals.find(500);
    Expect(bayonet_visual != catalog.visuals.end(), "fallback catalog should expose bayonet visuals");
    if (bayonet_visual != catalog.visuals.end())
    {
        Expect(
            bayonet_visual->second.resolved_world_model_resource_path == "models/weapons/w_knife_bayonet.vmdl_c",
            "fallback knife visuals should cache the normalized world model resource path");
        Expect(
            bayonet_visual->second.resolved_view_model_resource_path == "models/weapons/v_knife_bayonet.vmdl_c",
            "fallback knife visuals should cache the normalized view model resource path");
        Expect(
            bayonet_visual->second.knife_family == "knife_bayonet",
            "fallback knife visuals should derive a stable knife family from the selected knife");
        Expect(
            bayonet_visual->second.knife_identity_resolved,
            "fallback knife visuals should mark knife identity as resolved once model paths are normalized");
    }
}

void TestCatalogFromGameFiles()
{
    namespace fs = std::filesystem;
    const auto temp_directory = fs::temp_directory_path() / "nSkinzCS2.catalog.test";
    fs::create_directories(temp_directory);

    const auto items_game_path = temp_directory / "items_game.txt";
    const auto localization_path = temp_directory / "csgo_english.txt";

    {
        std::ofstream stream(items_game_path, std::ios::trunc);
        stream << R"kv("items_game"
{
    "qualities"
    {
        "normal"
        {
            "value" "0"
        }
        "tournament"
        {
            "value" "12"
        }
    }
    "items"
    {
        "7"
        {
            "name" "weapon_ak47"
            "item_name" "#SFUI_WPNHUD_AK47"
            "model_player" "models/weapons/v_rif_ak47.mdl"
            "model_world" "models/weapons/w_rif_ak47.mdl"
        }
        "500"
        {
            "name" "weapon_bayonet"
            "prefab" "melee_unusual"
            "item_name" "#SFUI_WPNHUD_KnifeBayonet"
            "model_player" "models/weapons/v_knife_bayonet.mdl"
            "model_world" "models/weapons/w_knife_bayonet.mdl"
        }
        "5027"
        {
            "name" "studded_bloodhound_gloves"
            "prefab" "hands_paintable"
            "item_name" "#CSGO_Wearable_t_studdedgloves"
            "model_player" "models/weapons/v_models/arms/glove_bloodhound/v_glove_bloodhound.mdl"
            "model_world" "models/weapons/w_models/arms/w_glove_bloodhound.mdl"
        }
    }
    "paint_kits"
    {
        "0"
        {
            "name" "default"
            "description_string" "#PaintKit_Default"
        }
        "44"
        {
            "name" "aq_case_hardened"
            "description_string" "#PaintKit_case_desc"
            "description_tag" "#PaintKit_case"
        }
        "9001"
        {
            "name" "workshop_default"
        }
        "10018"
        {
            "name" "sporty_purple"
            "description_string" "#PaintKit_pandora"
        }
    }
    "paint_kits"
    {
        "600"
        {
            "name" "cu_test_future"
            "description_tag" "#PaintKit_future"
        }
    }
    "sticker_kits"
    {
        "3"
        {
            "name" "crown"
            "item_name" "#StickerKit_crown"
            "sticker_material" "crown/crown"
        }
        "4"
        {
            "name" "patch_test"
            "item_name" "#PatchKit_test"
            "sticker_material" "patch/test"
        }
        "100"
        {
            "name" "spray_test"
            "item_name" "#SprayKit_test"
            "sticker_material" "spray/test"
        }
    }
    "sticker_kits"
    {
        "200"
        {
            "name" "future_team"
            "item_name" "#StickerKit_future"
            "sticker_material" "future/team"
        }
    }
}
)kv";
    }

    {
        std::ofstream stream(localization_path, std::ios::binary | std::ios::trunc);
        const unsigned char bom[] {0xFF, 0xFE};
        stream.write(reinterpret_cast<const char*>(bom), sizeof(bom));

        const std::u16string text = uR"kv("lang"
{
    "Language" "English"
    "Tokens"
    {
        "SFUI_WPNHUD_AK47" "AK-47" [$WIN32]
        "SFUI_WPNHUD_KnifeBayonet" "Bayonet"
        "CSGO_Wearable_t_studdedgloves" "Bloodhound"
        "PaintKit_Default" "Default"
        "PaintKit_case" "Case Hardened"
        "PaintKit_case_desc" "A very long workshop description that should not be used"
        "PaintKit_pandora" "Pandora's Box"
        "PaintKit_future" "Future Shock"
        "StickerKit_crown" "Crown (Foil)"
        "PatchKit_test" "Patch Test"
        "StickerKit_future" "Future Team"
    }
}
)kv";
        stream.write(
            reinterpret_cast<const char*>(text.data()),
            static_cast<std::streamsize>(text.size() * sizeof(char16_t)));
    }

    const auto catalog = nskinzcs2::CatalogBuilder::BuildCatalogFromGameFiles(items_game_path, localization_path);
    Expect(catalog.has_value(), "catalog should parse from local game files");
    Expect(catalog->runtime_populated, "file-backed catalog should mark itself populated");
    Expect(!catalog->base_items.empty(), "catalog should retain base items");

    const auto ak_it = std::find_if(
        catalog->base_items.begin(),
        catalog->base_items.end(),
        [](const nskinzcs2::model::NamedId& item)
        {
            return item.id == 7;
        });
    Expect(ak_it != catalog->base_items.end(), "AK-47 should remain in the base item list");
    Expect(ak_it->name == "AK-47", "AK-47 should use localized game data");

    const auto knife_it = std::find_if(
        catalog->knife_items.begin(),
        catalog->knife_items.end(),
        [](const nskinzcs2::model::NamedId& item)
        {
            return item.id == 500;
        });
    Expect(knife_it != catalog->knife_items.end(), "bayonet should remain in the knife list");
    Expect(knife_it->name == "Bayonet", "knife names should use localized game data");

    const auto glove_it = std::find_if(
        catalog->glove_items.begin(),
        catalog->glove_items.end(),
        [](const nskinzcs2::model::NamedId& item)
        {
            return item.id == 5027;
        });
    Expect(glove_it != catalog->glove_items.end(), "bloodhound gloves should remain in the glove list");
    Expect(glove_it->name == "Bloodhound", "glove names should use localized game data");

    const auto case_hardened_it = std::find_if(
        catalog->weapon_paint_kits.begin(),
        catalog->weapon_paint_kits.end(),
        [](const nskinzcs2::model::NamedId& item)
        {
            return item.id == 44;
        });
    Expect(case_hardened_it != catalog->weapon_paint_kits.end(), "case hardened should be in the weapon paint list");
    Expect(case_hardened_it->name == "Case Hardened", "paint kit should prefer localized short tag names");

    const auto future_shock_it = std::find_if(
        catalog->weapon_paint_kits.begin(),
        catalog->weapon_paint_kits.end(),
        [](const nskinzcs2::model::NamedId& item)
        {
            return item.id == 600;
        });
    Expect(future_shock_it != catalog->weapon_paint_kits.end(), "repeated paint_kits sections should merge");
    Expect(future_shock_it->name == "Future Shock", "merged paint kits should retain localized names");

    const auto sporty_glove_paint_it = catalog->glove_paint_compatibility.find(10018);
    Expect(sporty_glove_paint_it != catalog->glove_paint_compatibility.end(), "glove paint compatibility should be recorded");
    Expect(sporty_glove_paint_it->second == 5030, "sporty glove paints should map back to sport gloves");

    const auto future_sticker_it = std::find_if(
        catalog->sticker_kits.begin(),
        catalog->sticker_kits.end(),
        [](const nskinzcs2::model::NamedId& item)
        {
            return item.id == 200;
        });
    Expect(future_sticker_it != catalog->sticker_kits.end(), "repeated sticker_kits sections should merge");
    Expect(future_sticker_it->name == "Future Team", "merged sticker kits should retain localized names");

    Expect(catalog->weapon_paint_kits.size() == 3, "weapon paint kits should include default and merged weapon kits");
    Expect(catalog->glove_paint_kits.size() == 3, "glove paint kits should include default, workshop default and glove kits");
    Expect(catalog->sticker_kits.size() == 4, "sticker kits should keep none, stickers, merged sticker sections and sticker-material patches while filtering sprays");
    Expect(catalog->qualities.size() == 2, "qualities should come from the game files");

    fs::remove(localization_path);
    fs::remove(items_game_path);
    fs::remove(temp_directory);
}

void TestCatalogFromGameText()
{
    constexpr std::string_view items_game_text = R"kv("items_game"
{
    "qualities"
    {
        "normal"
        {
            "value" "0"
        }
    }
    "items"
    {
        "7"
        {
            "name" "weapon_ak47"
            "item_name" "#SFUI_WPNHUD_AK47"
            "model_player" "models/weapons/v_rif_ak47.mdl"
            "model_world" "models/weapons/w_rif_ak47.mdl"
        }
        "16"
        {
            "name" "weapon_m4a1"
        }
        "60"
        {
            "name" "weapon_m4a1_silencer"
        }
    }
    "paint_kits"
    {
        "0"
        {
            "name" "default"
            "description_string" "#PaintKit_Default"
        }
        "711"
        {
            "name" "cu_printstream"
            "description_tag" "#PaintKit_Printstream"
        }
        "844"
        {
            "name" "gs_m4a4_emperor"
            "use_legacy_model" "1"
        }
        "1353"
        {
            "name" "m4a4_skrrrt_green"
            "composite_material_path" "weapons/paints/community/community_36/m4a4_skrrrt_green.vcompmat"
        }
    }
    "item_sets"
    {
        "legacy_collection"
        {
            "items"
            {
                "[gs_m4a4_emperor]weapon_m4a1" "1"
                "[cu_printstream]weapon_ak47" "1"
                "[unknown_paint]weapon_m4a1" "1"
                "[gs_m4a4_emperor]weapon_m4a1_extra" "1"
                // "[gs_m4a4_emperor]weapon_ak47" "1"
                "[gs_m4a4_emperor]weapon_ak47" "1" [$X360]
            }
        }
    }
    "item_sets"
    {
        "modern_collection"
        {
            "items"
            {
                "[m4a4_skrrrt_green]weapon_m4a1" "1"
            }
        }
    }
    "client_loot_lists"
    {
        "shared_finish"
        {
            "[cu_printstream]weapon_m4a1_silencer" "1"
            "[gs_m4a4_emperor]weapon_m4a1" "1"
            "[gs_m4a4_emperorweapon_ak47" "1"
        }
    }
    "sticker_kits"
    {
        "3"
        {
            "name" "crown"
            "item_name" "#StickerKit_crown"
            "sticker_material" "crown/crown"
        }
    }
    "keychain_definitions"
    {
        "101"
        {
            "name" "charm_diamond_dog"
            "loc_name" "#Keychain_diamond_dog"
            "pedestal_display_model" "weapons/keychains/diamond_dog/kc_diamond_dog.vmdl"
        }
    }
}
)kv";

    constexpr std::string_view localization_text = R"kv("lang"
{
    "Language" "English"
    "Tokens"
    {
        "SFUI_WPNHUD_AK47" "AK-47"
        "PaintKit_Default" "Default"
        "PaintKit_Printstream" "Printstream"
        "StickerKit_crown" "Crown (Foil)"
        "Keychain_diamond_dog" "Diamond Dog"
    }
}
)kv";

    const auto catalog = nskinzcs2::CatalogBuilder::BuildCatalogFromGameText(items_game_text, localization_text);
    Expect(catalog.has_value(), "catalog should parse from in-memory game text");
    Expect(catalog->runtime_populated, "text-backed catalog should mark itself populated");
    Expect(catalog->weapon_paint_kits.size() == 4, "text-backed catalog should expose full parsed paint kits");
    Expect(catalog->weapon_paint_uses_legacy_model.at(844), "Emperor explicitly requires legacy mesh");
    Expect(!catalog->weapon_paint_uses_legacy_model.at(1353), "absent legacy flag means modern mesh, not unknown");
    Expect(catalog->weapon_paint_compatibility.at(16) == std::vector<int>({844, 1353}),
        "all repeated collection/loot sections merge and duplicate pairs are removed");
    Expect(catalog->weapon_paint_compatibility.at(7) == std::vector<int>({711}),
        "comments, foreign-platform pairs, malformed names and unknown IDs must not grant compatibility");
    Expect(catalog->weapon_paint_compatibility.at(60) == std::vector<int>({711}),
        "shared finishes retain each exact weapon; M4A4 and M4A1-S are distinct");
    Expect(catalog->keychain_kits.size() == 2, "text-backed catalog should expose parsed keychain kits");

    const auto printstream_it = std::find_if(
        catalog->weapon_paint_kits.begin(),
        catalog->weapon_paint_kits.end(),
        [](const nskinzcs2::model::NamedId& item)
        {
            return item.id == 711;
        });
    Expect(printstream_it != catalog->weapon_paint_kits.end(), "in-memory parse should preserve weapon paint kits");
    Expect(printstream_it->name == "Printstream", "in-memory parse should preserve localized paint kit names");

    const auto keychain_it = std::find_if(
        catalog->keychain_kits.begin(),
        catalog->keychain_kits.end(),
        [](const nskinzcs2::model::NamedId& item)
        {
            return item.id == 101;
        });
    Expect(keychain_it != catalog->keychain_kits.end(), "in-memory parse should preserve keychain kits");
    Expect(keychain_it->name == "Diamond Dog", "in-memory parse should preserve localized keychain names");

    const auto keychain_visual_it = catalog->keychain_visuals.find(101);
    Expect(keychain_visual_it != catalog->keychain_visuals.end(), "in-memory parse should preserve keychain model mappings");
    Expect(
        keychain_visual_it->second.model_path == "weapons/keychains/diamond_dog/kc_diamond_dog.vmdl",
        "in-memory parse should preserve keychain raw model paths");
    Expect(
        keychain_visual_it->second.resolved_model_resource_path ==
            "weapons/keychains/diamond_dog/kc_diamond_dog.vmdl_c",
        "in-memory parse should derive keychain compiled model paths");

    const auto m4a4_it = std::find_if(
        catalog->base_items.begin(),
        catalog->base_items.end(),
        [](const nskinzcs2::model::NamedId& item)
        {
            return item.id == 16;
        });
    Expect(m4a4_it != catalog->base_items.end(), "in-memory parse should preserve the M4A4 entry");
    Expect(m4a4_it->name == "M4A4", "known base weapons should prefer canonical catalog names over ambiguous raw ids");
}

void TestWeaponPaintCompatibility()
{
    using nskinzcs2::CatalogBuilder;
    using nskinzcs2::WeaponPaintCompatibility;
    nskinzcs2::model::Catalog catalog;
    catalog.weapon_paint_kits = {{0, "-"}, {44, "共享涂装"}, {844, "皇帝"}, {1353, "破浪狂飙"}, {1040, "倒吊人"}};
    catalog.weapon_paint_compatibility = {{16, {44, 844, 1353}}, {7, {44}}, {61, {1040}}};
    const auto original = catalog.weapon_paint_kits.size();
    const auto m4 = CatalogBuilder::BuildCompatibleWeaponPaintList(catalog, 16);
    const auto ak = CatalogBuilder::BuildCompatibleWeaponPaintList(catalog, 7);
    Expect(m4.size() == 4 && m4[2].name == "皇帝" && ak.size() == 2,
        "UI filtering retains compatible IDs, localized names and No Change");
    Expect(catalog.weapon_paint_kits.size() == original, "filtering must not reduce the full catalog");
    Expect(CatalogBuilder::CheckWeaponPaintCompatibility(&catalog, 16, 844) == WeaponPaintCompatibility::Compatible,
        "M4A4 Emperor must remain available");
    Expect(CatalogBuilder::CheckWeaponPaintCompatibility(&catalog, 7, 844) == WeaponPaintCompatibility::Incompatible &&
        CatalogBuilder::CheckWeaponPaintCompatibility(&catalog, 16, 1040) == WeaponPaintCompatibility::Incompatible,
        "cross-weapon finishes must be rejected");
    Expect(CatalogBuilder::CheckWeaponPaintCompatibility(&catalog, 61, 1040) == WeaponPaintCompatibility::Compatible,
        "USP-S Traitor remains available");
    Expect(CatalogBuilder::CheckWeaponPaintCompatibility(&catalog, 16, 999999) == WeaponPaintCompatibility::Incompatible &&
        CatalogBuilder::CheckWeaponPaintCompatibility(&catalog, 16, -1) == WeaponPaintCompatibility::Incompatible,
        "manual nonexistent/negative paint IDs must not bypass validation");
    Expect(CatalogBuilder::CheckWeaponPaintCompatibility(nullptr, 16, 844) == WeaponPaintCompatibility::Unknown &&
        CatalogBuilder::CheckWeaponPaintCompatibility(&catalog, 9, 844) == WeaponPaintCompatibility::Unknown,
        "missing catalog or definition metadata must not authorize every finish");
    Expect(CatalogBuilder::CheckWeaponPaintCompatibility(nullptr, 16, 0) == WeaponPaintCompatibility::Compatible,
        "No Change and explicit Vanilla both remain usable without compatibility data");
    catalog.weapon_paint_compatibility.clear();
    Expect(CatalogBuilder::BuildCompatibleWeaponPaintList(catalog, 16).size() == 1,
        "fallback/old snapshot must expose only No Change before the explicit Vanilla UI entry");
    Expect(nskinzcs2::runtime::ProbeWeaponPaintCompatibilityForTests(),
        "actual runtime entrypoints must reject incompatible paints before writes, binding or cached success");
}

void TestCatalogFromRealGameFilesIfProvided()
{
    const char* items_game_env = std::getenv("NSKINZCS2_REAL_ITEMS_GAME");
    const char* localization_env = std::getenv("NSKINZCS2_REAL_LOCALIZATION");
    if (items_game_env == nullptr || localization_env == nullptr)
    {
        return;
    }

    namespace fs = std::filesystem;
    const fs::path items_game_path(items_game_env);
    const fs::path localization_path(localization_env);
    Expect(fs::exists(items_game_path), "real items_game path should exist");
    Expect(fs::exists(localization_path), "real localization path should exist");

    const auto catalog = nskinzcs2::CatalogBuilder::BuildCatalogFromGameFiles(items_game_path, localization_path);
    Expect(catalog.has_value(), "real installed game files should parse successfully");
    Expect(catalog->runtime_populated, "real installed game catalog should be runtime populated");
    Expect(catalog->weapon_paint_kits.size() > 100, "real installed game files should provide a large weapon paint list");
    Expect(catalog->sticker_kits.size() > 100, "real installed game files should provide a large sticker list");
    Expect(catalog->keychain_kits.size() > 1, "real installed game files should provide parsed keychain kits");
    Expect(!catalog->keychain_visuals.empty(), "real installed game files should provide keychain model mappings");
    Expect(catalog->qualities.size() >= 5, "real installed game files should provide quality entries");
    std::size_t compatible_pairs = 0;
    std::size_t checked_weapons = 0;
    for (const auto& weapon : catalog->base_items)
    {
        if (weapon.id == 42 || weapon.id == 5028) { continue; }
        const auto found = catalog->weapon_paint_compatibility.find(weapon.id);
        Expect(found != catalog->weapon_paint_compatibility.end() && !found->second.empty(),
            "every supported gun must have exact compatibility pairs in installed game data");
        Expect(std::is_sorted(found->second.begin(), found->second.end()), "compatibility IDs must be sorted");
        Expect(std::adjacent_find(found->second.begin(), found->second.end()) == found->second.end(),
            "compatibility IDs must not contain duplicates");
        for (const auto paint : found->second)
        {
            Expect(paint > 0 && catalog->weapon_paint_uses_legacy_model.contains(paint),
                "every compatible gun finish must have explicit modern/legacy metadata");
        }
        compatible_pairs += found->second.size();
        ++checked_weapons;
        const auto visible = nskinzcs2::CatalogBuilder::BuildCompatibleWeaponPaintList(*catalog, weapon.id);
        Expect(visible.size() == found->second.size() + 1, "every game-defined pair must remain visible exactly once");
        std::cout << "compatible gun " << weapon.id << ": " << found->second.size() << " finishes\n";
    }
    using nskinzcs2::WeaponPaintCompatibility;
    for (const auto paint : {844, 1353})
    {
        Expect(nskinzcs2::CatalogBuilder::CheckWeaponPaintCompatibility(&*catalog, 16, paint) == WeaponPaintCompatibility::Compatible,
            "real M4A4 legacy and Armory modern finish must both remain selectable");
        Expect(nskinzcs2::CatalogBuilder::CheckWeaponPaintCompatibility(&*catalog, 60, paint) == WeaponPaintCompatibility::Incompatible,
            "M4A1-S must not inherit M4A4 finishes");
    }
    Expect(nskinzcs2::CatalogBuilder::CheckWeaponPaintCompatibility(&*catalog, 9, 174) == WeaponPaintCompatibility::Compatible &&
        catalog->weapon_paint_uses_legacy_model.at(174), "real AWP BOOM remains compatible and uses legacy mesh 2");
    Expect(nskinzcs2::CatalogBuilder::CheckWeaponPaintCompatibility(&*catalog, 61, 1040) == WeaponPaintCompatibility::Compatible,
        "real USP-S Traitor must remain selectable");
    Expect(nskinzcs2::CatalogBuilder::CheckWeaponPaintCompatibility(&*catalog, 7, 44) == WeaponPaintCompatibility::Compatible &&
        nskinzcs2::CatalogBuilder::CheckWeaponPaintCompatibility(&*catalog, 3, 44) == WeaponPaintCompatibility::Compatible,
        "real shared Case Hardened paint retains both AK-47 and Five-SeveN combinations");
    std::cout << "compatibility coverage: guns=" << checked_weapons << ", pairs=" << compatible_pairs << '\n';
    if (g_weapon_paint_only)
    {
        Expect(catalog->weapon_paint_uses_legacy_model.at(844), "installed Emperor uses legacy model");
        Expect(!catalog->weapon_paint_uses_legacy_model.at(1353), "installed Full Throttle uses modern model");
        std::cout << "installed M4A4 paint metadata: 1353=modern/mesh1; 844=legacy/mesh2\n";
    }
    std::cout
        << "real catalog counts: weapon paints=" << catalog->weapon_paint_kits.size()
        << ", glove paints=" << catalog->glove_paint_kits.size()
        << ", stickers=" << catalog->sticker_kits.size()
        << ", keychains=" << catalog->keychain_kits.size()
        << ", qualities=" << catalog->qualities.size()
        << '\n';
}

void TestDuplicateDetection()
{
    std::vector<nskinzcs2::model::NamedId> unique {
        {1, "One"},
        {2, "Two"},
    };

    std::vector<nskinzcs2::model::NamedId> duplicate {
        {1, "One"},
        {1, "Again"},
    };

    Expect(!nskinzcs2::CatalogBuilder::HasDuplicateIds(unique), "unique ids should not flag duplicates");
    Expect(nskinzcs2::CatalogBuilder::HasDuplicateIds(duplicate), "duplicate ids should be detected");
}

void TestVersionCandidateFiltering()
{
    const std::vector<std::string> discovered {
        "Source2Client002",
        "Source2Client005",
        "Source2Client001",
        "Source2EngineToClient001",
        "Source2ClientABC",
    };

    const auto filtered = nskinzcs2::RuntimeController::FilterVersionCandidates(
        discovered,
        "Source2Client",
        1,
        8);

    Expect(filtered.size() == 3, "three valid Source2Client versions should remain");
    Expect(filtered[0] == "Source2Client005", "highest discovered version should sort first");
    Expect(filtered[1] == "Source2Client002", "middle discovered version should sort second");
    Expect(filtered[2] == "Source2Client001", "lowest discovered version should sort last");
}

void TestRuntimeRebuildDecision()
{
    using nskinzcs2::runtime::EvaluateRuntimeRebuildDecision;
    using nskinzcs2::runtime::RuntimeRebuildInputs;
    using nskinzcs2::runtime::RuntimeRebuildReason;

    RuntimeRebuildInputs manual_inputs;
    manual_inputs.manual_refresh = true;
    const auto manual = EvaluateRuntimeRebuildDecision(manual_inputs);
    Expect(manual.rebuild, "manual refresh should trigger a rebuild");
    Expect(manual.reason == RuntimeRebuildReason::ManualRefresh, "manual refresh should win rebuild priority");

    RuntimeRebuildInputs watched_switch_inputs;
    watched_switch_inputs.watched_player_switched = true;
    const auto watched_switch = EvaluateRuntimeRebuildDecision(watched_switch_inputs);
    Expect(watched_switch.rebuild, "watched player switch should trigger a rebuild");
    Expect(
        watched_switch.reason == RuntimeRebuildReason::WatchedPlayerSwitch,
        "watched player switch should surface its own rebuild reason");

    const auto none = EvaluateRuntimeRebuildDecision(RuntimeRebuildInputs {});
    Expect(!none.rebuild, "empty transition should not trigger a rebuild");
}

void TestKnifeRefreshCrashGuardPolicy()
{
    using nskinzcs2::runtime::KnifeRefreshHelperPolicy;
    using nskinzcs2::runtime::KnifeRefreshHelperPolicyArmsOnRebuildReason;
    using nskinzcs2::runtime::ProbeKnifeRefreshAttachmentPromotionAllowedForTests;
    using nskinzcs2::runtime::ProbeKnifeRefreshGuardBindingRestoresOnAbandonForTests;
    using nskinzcs2::runtime::ProbePromotedWeaponRefreshAttachmentRestoresOnAbandonForTests;
    using nskinzcs2::runtime::RuntimeRebuildReason;
    using nskinzcs2::runtime::ShouldAllowAttributeListVectorMutationForProfile;
    using nskinzcs2::runtime::ShouldAllowVerifiedInPlaceKnifeInitializationRefresh;
    using nskinzcs2::runtime::ShouldAllowWeaponRefreshHelperDispatch;
    using nskinzcs2::runtime::ShouldIncrementCustomEconReloadEventForMaterialRefresh;
    using nskinzcs2::runtime::ShouldConsumeKnifeRefreshHelperEpoch;
    using nskinzcs2::runtime::ShouldDispatchKnifeRefreshWithInPlaceAttributes;
    using nskinzcs2::runtime::ShouldPreserveWeaponAppearanceSignaturesOnRebuildReason;
    using nskinzcs2::runtime::ShouldUseStableKnifeFinishFastPath;
    using nskinzcs2::runtime::ShouldUseStableStandardWeaponMaterialFastPath;
    using nskinzcs2::runtime::ShouldInstallRuntimeOwnedAttributeListForProfile;
    using nskinzcs2::runtime::ShouldUseKnifeVanillaTemporaryRefreshOverlay;

    Expect(
        !KnifeRefreshHelperPolicyArmsOnRebuildReason(
            KnifeRefreshHelperPolicy::Disabled,
            RuntimeRebuildReason::ManualRefresh),
        "disabled crash-guard mode should never arm a knife refresh epoch");
    Expect(
        KnifeRefreshHelperPolicyArmsOnRebuildReason(
            KnifeRefreshHelperPolicy::ManualOnly,
            RuntimeRebuildReason::ManualRefresh),
        "manual refresh should arm the knife refresh epoch in manual-only mode");
    Expect(
        !KnifeRefreshHelperPolicyArmsOnRebuildReason(
            KnifeRefreshHelperPolicy::ManualOnly,
            RuntimeRebuildReason::DemoSeek),
        "demo seek must stay outside the knife refresh epoch in manual-only mode");
    Expect(
        !KnifeRefreshHelperPolicyArmsOnRebuildReason(
            KnifeRefreshHelperPolicy::ManualOnly,
            RuntimeRebuildReason::FullUpdate),
        "full update must stay outside the knife refresh epoch in manual-only mode");
    Expect(
        KnifeRefreshHelperPolicyArmsOnRebuildReason(
            KnifeRefreshHelperPolicy::ManualOrSeekOnly,
            RuntimeRebuildReason::ManualRefresh),
        "manual refresh should arm the knife refresh epoch in manual-or-seek mode");
    Expect(
        KnifeRefreshHelperPolicyArmsOnRebuildReason(
            KnifeRefreshHelperPolicy::ManualOrSeekOnly,
            RuntimeRebuildReason::DemoSeek),
        "demo seek should arm the knife refresh epoch in manual-or-seek mode");
    Expect(
        KnifeRefreshHelperPolicyArmsOnRebuildReason(
            KnifeRefreshHelperPolicy::ManualOrSeekOnly,
            RuntimeRebuildReason::FullUpdate),
        "full update should arm the knife refresh epoch in manual-or-seek mode");
    Expect(
        !KnifeRefreshHelperPolicyArmsOnRebuildReason(
            KnifeRefreshHelperPolicy::ManualOrSeekOnly,
            RuntimeRebuildReason::WatchedPlayerSwitch),
        "watched-player switches must stay outside the knife refresh epoch");
    Expect(
        !KnifeRefreshHelperPolicyArmsOnRebuildReason(
            KnifeRefreshHelperPolicy::ManualOrSeekOnly,
            RuntimeRebuildReason::PauseResume),
        "pause/resume must stay outside the knife refresh epoch");
    Expect(
        !KnifeRefreshHelperPolicyArmsOnRebuildReason(
            KnifeRefreshHelperPolicy::ManualOrSeekOnly,
            RuntimeRebuildReason::EntityListChanged),
        "entity-list changes must stay outside the knife refresh epoch");

    Expect(
        ShouldPreserveWeaponAppearanceSignaturesOnRebuildReason(RuntimeRebuildReason::PauseResume),
        "pause/resume should preserve verified weapon material signatures");
    Expect(
        ShouldPreserveWeaponAppearanceSignaturesOnRebuildReason(RuntimeRebuildReason::ManualRefresh),
        "an unchanged manual Update should preserve verified weapon material signatures");
    Expect(
        ShouldPreserveWeaponAppearanceSignaturesOnRebuildReason(RuntimeRebuildReason::WatchedPlayerSwitch),
        "spectator-target switches should preserve verified weapon material signatures");
    Expect(
        !ShouldPreserveWeaponAppearanceSignaturesOnRebuildReason(RuntimeRebuildReason::DemoSeek),
        "demo seeks must invalidate weapon material signatures because entities can be replaced");
    Expect(
        !ShouldPreserveWeaponAppearanceSignaturesOnRebuildReason(RuntimeRebuildReason::ConfigChanged),
        "configuration changes must invalidate weapon material signatures");

    Expect(
        ShouldAllowWeaponRefreshHelperDispatch(false, KnifeRefreshHelperPolicy::Disabled, false),
        "standard weapon helper dispatch should stay enabled under the knife crash guard");
    Expect(
        !ShouldIncrementCustomEconReloadEventForMaterialRefresh(),
        "explicit material refresh must not also forge a second network reload event");
    Expect(
        !ShouldAllowWeaponRefreshHelperDispatch(true, KnifeRefreshHelperPolicy::Disabled, false),
        "disabled crash-guard mode should suppress knife helper dispatch");
    Expect(
        ShouldAllowWeaponRefreshHelperDispatch(true, KnifeRefreshHelperPolicy::ManualOnly, true),
        "an armed manual-only epoch should allow one knife helper dispatch");
    Expect(
        !ShouldAllowWeaponRefreshHelperDispatch(true, KnifeRefreshHelperPolicy::ManualOnly, false),
        "manual-only knife helper dispatch should stay suppressed outside Update");
    Expect(
        ShouldAllowWeaponRefreshHelperDispatch(true, KnifeRefreshHelperPolicy::ManualOrSeekOnly, true),
        "an armed manual-or-seek epoch should allow one knife helper dispatch");
    Expect(
        !ShouldAllowWeaponRefreshHelperDispatch(true, KnifeRefreshHelperPolicy::ManualOrSeekOnly, false),
        "steady-state frames should suppress knife helper dispatch outside the armed epoch");

    Expect(
        ShouldConsumeKnifeRefreshHelperEpoch(true, true),
        "a successful knife helper dispatch should consume the armed epoch");
    Expect(
        !ShouldConsumeKnifeRefreshHelperEpoch(true, false),
        "a failed knife helper dispatch must not consume the armed epoch");
    Expect(
        !ShouldConsumeKnifeRefreshHelperEpoch(false, true),
        "standard-weapon helper dispatches must not consume the knife epoch");

    Expect(
        ShouldDispatchKnifeRefreshWithInPlaceAttributes(true, true, true),
        "a knife refresh should reuse verified in-place attributes without replacing its vector");
    Expect(
        !ShouldDispatchKnifeRefreshWithInPlaceAttributes(true, true, false),
        "a knife refresh must not dispatch with incomplete in-place attributes");
    Expect(
        !ShouldDispatchKnifeRefreshWithInPlaceAttributes(false, true, true),
        "standard weapons should use the native material gate without temporary attribute attachments");

    Expect(
        ShouldUseKnifeVanillaTemporaryRefreshOverlay(true, true, false),
        "an explicit vanilla knife with no paint attribute should use the scoped empty-vector refresh path");
    Expect(
        ShouldUseKnifeVanillaTemporaryRefreshOverlay(true, true, true),
        "an explicit vanilla knife must clear an existing paint attribute instead of reusing it in place");
    Expect(
        !ShouldUseKnifeVanillaTemporaryRefreshOverlay(false, true, false),
        "standard weapons must not enter the knife-only vanilla overlay path");

    Expect(
        ShouldAllowVerifiedInPlaceKnifeInitializationRefresh(true, true, true, false),
        "a new knife entity with verified in-place attributes should receive one initialization refresh");
    Expect(
        !ShouldAllowVerifiedInPlaceKnifeInitializationRefresh(true, true, true, true),
        "an already refreshed knife entity must not receive another automatic initialization refresh");
    Expect(
        !ShouldAllowVerifiedInPlaceKnifeInitializationRefresh(true, true, false, false),
        "an incomplete knife attribute state must remain behind the manual refresh epoch");

    Expect(
        ShouldUseStableKnifeFinishFastPath(true, true, false, true, true, true),
        "a refreshed finish-only knife with matching live state should skip the exact pipeline");
    Expect(
        !ShouldUseStableKnifeFinishFastPath(true, true, true, true, true, true),
        "a knife model rebind request must never use the finish-only fast path");
    Expect(
        !ShouldUseStableKnifeFinishFastPath(true, true, false, true, true, false),
        "a changed live knife attribute must invalidate the finish-only fast path");
    Expect(
        !ShouldUseStableKnifeFinishFastPath(true, true, false, false, true, true),
        "a knife without a completed material refresh must run the exact pipeline");
    Expect(
        !ShouldUseStableKnifeFinishFastPath(true, true, false, true, false, true),
        "a changed live knife definition must invalidate the finish-only fast path");

    Expect(
        ShouldUseStableStandardWeaponMaterialFastPath(true, true, true, true, true, true),
        "a refreshed standard weapon with matching durable material state should skip another helper dispatch");
    Expect(
        !ShouldUseStableStandardWeaponMaterialFastPath(true, false, true, true, true, true),
        "a standard weapon without a completed material refresh must run the exact pipeline");
    Expect(
        !ShouldUseStableStandardWeaponMaterialFastPath(true, true, true, false, true, true),
        "a replaced standard-weapon attribute vector must invalidate the stable material path");
    Expect(
        !ShouldUseStableStandardWeaponMaterialFastPath(true, true, true, true, false, true),
        "a changed render mesh mask must invalidate the stable material path");
    Expect(
        !ShouldUseStableStandardWeaponMaterialFastPath(false, true, true, true, true, true),
        "knife profiles must stay outside the standard-weapon stable material policy");

    {
        nskinzcs2::model::CosmeticProfile knife_profile;
        knife_profile.base_item = 42;
        Expect(
            !ShouldAllowAttributeListVectorMutationForProfile(knife_profile),
            "knife crash guard should suppress attribute-list vector mutation");
        Expect(
            !ShouldInstallRuntimeOwnedAttributeListForProfile(knife_profile),
            "knife crash guard should suppress runtime-owned attribute-list installs");
        Expect(
            !ProbeKnifeRefreshAttachmentPromotionAllowedForTests(knife_profile),
            "knife refresh helper attachments must restore immediately instead of promoting to runtime-owned");
        Expect(
            ProbePromotedWeaponRefreshAttachmentRestoresOnAbandonForTests(knife_profile),
            "knife promoted refresh helper attachments should restore on abandon-mode cleanup boundaries");

        knife_profile.paint_kit = 0;
        knife_profile.explicit_vanilla_finish = true;
        Expect(
            !ProbeKnifeRefreshAttachmentPromotionAllowedForTests(knife_profile),
            "an explicit vanilla knife should not promote an attribute payload after its paint lists are cleared");
        Expect(
            !ProbePromotedWeaponRefreshAttachmentRestoresOnAbandonForTests(knife_profile),
            "an explicit vanilla knife has no promoted payload to restore after a seek");
    }

    {
        nskinzcs2::model::CosmeticProfile weapon_profile;
        weapon_profile.base_item = 9;
        Expect(
            !ShouldAllowAttributeListVectorMutationForProfile(weapon_profile),
            "standard weapons must not grow or replace live attribute-list vectors");
        Expect(
            !ShouldInstallRuntimeOwnedAttributeListForProfile(weapon_profile),
            "standard weapons must not install runtime-owned attribute-list storage");
        Expect(
            !ProbeKnifeRefreshAttachmentPromotionAllowedForTests(weapon_profile),
            "standard weapon refresh must use native invalidation without promoted attribute attachments");
        Expect(
            !ProbePromotedWeaponRefreshAttachmentRestoresOnAbandonForTests(weapon_profile),
            "standard weapon in-place attachments must not touch stale entity storage after seek detection");

        nskinzcs2::model::KeychainProfile keychain_profile;
        keychain_profile.base_item = weapon_profile.base_item;
        keychain_profile.keychain_kit = 101;
        Expect(
            !ProbeKnifeRefreshAttachmentPromotionAllowedForTests(weapon_profile, &keychain_profile),
            "standard weapon keychain refresh must not retain rewritten CS2 attribute storage");
        Expect(
            !ProbePromotedWeaponRefreshAttachmentRestoresOnAbandonForTests(weapon_profile, &keychain_profile),
            "standard weapon promoted in-place attachments must be abandoned without stale entity writes");
    }

    Expect(
        ProbeKnifeRefreshGuardBindingRestoresOnAbandonForTests(),
        "promoted knife refresh bindings should restore on abandon-mode cleanup boundaries");
}

void TestKnifeAnimationRemapTable()
{
    Expect(
        !nskinzcs2::runtime::KnifeAnimationRemapTableHasDuplicateKeys(),
        "knife animation remap table should not contain duplicate keys");

    const std::vector<int> supported_definitions {
        42,
        59,
        500,
        503,
        505,
        506,
        507,
        508,
        509,
        512,
        514,
        515,
        516,
        517,
        518,
        519,
        520,
        521,
        522,
        523,
    };
    Expect(
        nskinzcs2::runtime::KnifeAnimationRemapTableSupportsDefinitions(supported_definitions),
        "supported knife ids should all have remap entries");
    Expect(
        nskinzcs2::runtime::KnifeAnimationRemapTableSupportsTransition(
            42,
            42,
            nskinzcs2::runtime::KnifeRemapActionKind::Deploy),
        "identity deploy remap should exist for supported knives");
    Expect(
        nskinzcs2::runtime::KnifeAnimationRemapTableSupportsTransition(
            42,
            42,
            nskinzcs2::runtime::KnifeRemapActionKind::Inspect),
        "identity inspect remap should exist for supported knives");
    Expect(
        nskinzcs2::runtime::KnifeAnimationRemapTableSupportsTransition(
            42,
            42,
            nskinzcs2::runtime::KnifeRemapActionKind::LightAttack),
        "identity light-attack remap should exist for supported knives");
    Expect(
        nskinzcs2::runtime::KnifeAnimationRemapTableSupportsTransition(
            42,
            42,
            nskinzcs2::runtime::KnifeRemapActionKind::HeavyAttack),
        "identity heavy-attack remap should exist for supported knives");
    Expect(
        !nskinzcs2::runtime::KnifeAnimationRemapTableSupportsTransition(
            42,
            1337,
            nskinzcs2::runtime::KnifeRemapActionKind::Deploy),
        "unsupported knife ids should not report a deploy remap");
    Expect(
        !nskinzcs2::runtime::KnifeAnimationRemapTableSupportsTransition(
            16256,
            507,
            nskinzcs2::runtime::KnifeRemapActionKind::Deploy),
        "unsupported source knife ids should not report a deploy remap");
    Expect(
        nskinzcs2::runtime::KnifeAnimationRemapTableSupportsTransition(
            42,
            507,
            nskinzcs2::runtime::KnifeRemapActionKind::Deploy),
        "cross-knife deploy remap should be available once the runtime interceptor is authored");
    Expect(
        nskinzcs2::runtime::KnifeAnimationRemapTableSupportsTransition(
            42,
            507,
            nskinzcs2::runtime::KnifeRemapActionKind::Inspect),
        "cross-knife inspect remap should be available once the runtime interceptor is authored");
    Expect(
        nskinzcs2::runtime::KnifeAnimationRemapTableSupportsTransition(
            42,
            507,
            nskinzcs2::runtime::KnifeRemapActionKind::LightAttack),
        "cross-knife light-attack remap should be available once the runtime interceptor is authored");
    Expect(
        nskinzcs2::runtime::KnifeAnimationRemapTableSupportsTransition(
            42,
            507,
            nskinzcs2::runtime::KnifeRemapActionKind::HeavyAttack),
        "cross-knife heavy-attack remap should be available once the runtime interceptor is authored");
}

void TestPreferredKnifeTargetXuid()
{
    using nskinzcs2::runtime::ProbePreferredKnifeTargetXuidForTests;

    constexpr std::uint64_t kTargetXuid = 76561198086901534ull;
    constexpr std::uint64_t kOtherXuid = 76561198000000042ull;
    constexpr std::uint64_t kThirdXuid = 76561198000000099ull;

    {
        nskinzcs2::model::ConfigDocument config;
        nskinzcs2::model::KnifeProfile profile;
        profile.enabled = true;
        profile.xuid = kTargetXuid;
        config.knife_profiles.push_back(profile);

        Expect(
            ProbePreferredKnifeTargetXuidForTests(config, {kOtherXuid, kTargetXuid}) == kTargetXuid,
            "a single live enabled knife-profile target should become the preferred watched xuid");
    }

    {
        nskinzcs2::model::ConfigDocument config;
        nskinzcs2::model::KnifeProfile profile;
        profile.enabled = true;
        profile.xuid = kTargetXuid;
        config.knife_profiles.push_back(profile);

        Expect(
            ProbePreferredKnifeTargetXuidForTests(config, {kOtherXuid, kThirdXuid}) == 0,
            "preferred knife targets should stay disabled until the configured xuid is live");
    }

    {
        nskinzcs2::model::ConfigDocument config;
        nskinzcs2::model::KnifeProfile first;
        first.enabled = true;
        first.xuid = kTargetXuid;
        config.knife_profiles.push_back(first);

        auto duplicate = first;
        config.knife_profiles.push_back(duplicate);

        Expect(
            ProbePreferredKnifeTargetXuidForTests(config, {kTargetXuid}) == kTargetXuid,
            "duplicate enabled knife profiles for the same target should keep the preferred watched xuid");
    }

    {
        nskinzcs2::model::ConfigDocument config;
        nskinzcs2::model::KnifeProfile first;
        first.enabled = true;
        first.xuid = kTargetXuid;
        config.knife_profiles.push_back(first);

        nskinzcs2::model::KnifeProfile second;
        second.enabled = true;
        second.xuid = kOtherXuid;
        config.knife_profiles.push_back(second);

        Expect(
            ProbePreferredKnifeTargetXuidForTests(config, {kTargetXuid, kOtherXuid}) == 0,
            "multiple enabled knife-profile targets should not force a preferred watched xuid");
    }
}

void TestWatchedKnifeTargetResolutionFallback()
{
    using nskinzcs2::runtime::ProbeResolveWatchedKnifeTargetXuidForTests;

    constexpr std::uint64_t kObserverXuid = 76561198086901534ull;
    constexpr std::uint64_t kHeuristicXuid = 76561198000000042ull;

    Expect(
        ProbeResolveWatchedKnifeTargetXuidForTests(
            true,
            true,
            kObserverXuid,
            true,
            kHeuristicXuid,
            kHeuristicXuid) == kObserverXuid,
        "resolved observer-first watched targets should stay authoritative over bridge fallback");
    Expect(
        ProbeResolveWatchedKnifeTargetXuidForTests(
            false,
            true,
            0,
            true,
            kHeuristicXuid,
            kHeuristicXuid) == kHeuristicXuid,
        "invalid observer chains should now fall back to the resolved heuristic-xuid bridge");
    Expect(
        ProbeResolveWatchedKnifeTargetXuidForTests(
            false,
            false,
            0,
            true,
            kHeuristicXuid,
            kHeuristicXuid) == kHeuristicXuid,
        "missing observer chains should keep the bridged watched target when controller-pawn resolution succeeds");
    Expect(
        ProbeResolveWatchedKnifeTargetXuidForTests(
            false,
            true,
            0,
            false,
            0,
            kHeuristicXuid) == 0,
        "heuristic xuid alone should no longer become the formal watched target without a bridged pawn");
    Expect(
        ProbeResolveWatchedKnifeTargetXuidForTests(false, true, 0, false, 0, 0) == 0,
        "observer invalid states without bridge or heuristic evidence should still resolve to zero");
}

void TestRuntimeGloveBindingChoice()
{
    using nskinzcs2::runtime::EvaluateGloveBindingChoice;
    using nskinzcs2::runtime::GloveApplyMode;
    using nskinzcs2::runtime::RuntimeGloveBindingChoice;
    using nskinzcs2::runtime::RuntimeGloveBindingInputs;
    using nskinzcs2::runtime::ResolveGloveApplyMode;

    RuntimeGloveBindingInputs stable_inputs;
    stable_inputs.stable_binding_valid = true;
    Expect(
        EvaluateGloveBindingChoice(stable_inputs) ==
            RuntimeGloveBindingChoice::ReuseStable,
        "stable glove binding should be preferred");
    RuntimeGloveBindingInputs runtime_created_inputs;
    runtime_created_inputs.runtime_created_binding_valid = true;
    Expect(
        EvaluateGloveBindingChoice(runtime_created_inputs) ==
            RuntimeGloveBindingChoice::ReuseRuntimeCreated,
        "runtime-created glove binding should be reused when available");
    RuntimeGloveBindingInputs recreate_inputs;
    recreate_inputs.client_factory_available = true;
    Expect(
        EvaluateGloveBindingChoice(recreate_inputs) ==
            RuntimeGloveBindingChoice::RecreateRuntimeCreated,
        "client factory availability should request runtime glove recreation");
    Expect(
        EvaluateGloveBindingChoice(RuntimeGloveBindingInputs {}) ==
            RuntimeGloveBindingChoice::Unavailable,
        "missing glove paths should fail closed");

    {
        nskinzcs2::model::CosmeticProfile profile;
        profile.base_item = 5028;
        profile.override_item = 0;
        Expect(
            ResolveGloveApplyMode(profile) == GloveApplyMode::TextureOnly,
            "glove profiles without an override item should stay in texture-only mode");
    }

    {
        nskinzcs2::model::CosmeticProfile profile;
        profile.base_item = 5028;
        profile.override_item = 5030;
        profile.paint_kit = 10018;
        Expect(
            ResolveGloveApplyMode(profile) == GloveApplyMode::ModelAndTexture,
            "glove profiles with a selected glove model should enter model+texture mode");
    }

    {
        nskinzcs2::model::CosmeticProfile profile;
        profile.base_item = 9;
        profile.paint_kit = 344;
        Expect(
            ResolveGloveApplyMode(profile) == GloveApplyMode::None,
            "non-glove profiles must not enter glove apply modes");
    }
}

void TestWearableFactoryConfirmation()
{
    using nskinzcs2::runtime::EvaluateWearableFactoryConfirmation;
    using nskinzcs2::runtime::IsWearableModelSourceTypeReady;
    using nskinzcs2::runtime::MatchesWearableDefinitionRefreshThunk;

    const auto first_candidate = EvaluateWearableFactoryConfirmation({true, false, 0, 0});
    Expect(
        first_candidate.candidate_confirmations == 1 &&
            !first_candidate.publish_available &&
            !first_candidate.publish_unavailable,
        "a first CEconWearable factory observation must remain pending");

    const auto confirmed_candidate = EvaluateWearableFactoryConfirmation({true, true, 1, 0});
    Expect(
        confirmed_candidate.candidate_confirmations == 2 &&
            confirmed_candidate.publish_available &&
            !confirmed_candidate.publish_unavailable,
        "two matching FRAME_RENDER_PASS factory observations should publish the factory");

    const auto changed_candidate = EvaluateWearableFactoryConfirmation({true, false, 1, 0});
    Expect(
        changed_candidate.candidate_confirmations == 1 &&
            !changed_candidate.publish_available,
        "a changed client-class node or create function must restart confirmation");

    const auto first_failure = EvaluateWearableFactoryConfirmation({false, false, 1, 0});
    Expect(
        first_failure.candidate_confirmations == 0 &&
            first_failure.failure_confirmations == 1 &&
            !first_failure.publish_unavailable,
        "one transient GetAllClasses failure must not publish a callable address or a permanent failure");

    const auto confirmed_failure = EvaluateWearableFactoryConfirmation({false, false, 0, 1});
    Expect(
        confirmed_failure.failure_confirmations == 2 &&
            confirmed_failure.publish_unavailable &&
            !confirmed_failure.publish_available,
        "two consecutive GetAllClasses failures should fail the wearable factory closed");

    const auto recovery_candidate = EvaluateWearableFactoryConfirmation({true, false, 0, 2});
    Expect(
        recovery_candidate.candidate_confirmations == 1 &&
            recovery_candidate.failure_confirmations == 0 &&
            !recovery_candidate.publish_unavailable,
        "a valid candidate observation should clear pending failure confirmation state");

    Expect(
        !IsWearableModelSourceTypeReady(0) &&
            !IsWearableModelSourceTypeReady(1) &&
            IsWearableModelSourceTypeReady(2) &&
            IsWearableModelSourceTypeReady(3) &&
            IsWearableModelSourceTypeReady(4) &&
            !IsWearableModelSourceTypeReady(5) &&
            !IsWearableModelSourceTypeReady(6) &&
            IsWearableModelSourceTypeReady(7) &&
            IsWearableModelSourceTypeReady(8) &&
            !IsWearableModelSourceTypeReady(9),
        "wearable formal bind readiness must match the native model-source type dispatch");

    const std::array<std::uint8_t, 27> refresh_thunk {
        0x40, 0x53,
        0x48, 0x83, 0xEC, 0x20,
        0x48, 0x8B, 0xD9,
        0xE8, 0x22, 0xAC, 0x99, 0xFF,
        0x48, 0x8B, 0xCB,
        0x48, 0x83, 0xC4, 0x20,
        0x5B,
        0xE9, 0x35, 0x0D, 0xFF, 0xFF};
    Expect(
        MatchesWearableDefinitionRefreshThunk(refresh_thunk.data(), refresh_thunk.size()),
        "the verified CEconWearable client-think shape should be accepted");

    auto wrong_prologue = refresh_thunk;
    wrong_prologue[5] = 0x28;
    Expect(
        !MatchesWearableDefinitionRefreshThunk(wrong_prologue.data(), wrong_prologue.size()),
        "a changed CEconWearable client-think prologue must fail closed");

    auto missing_tail_jump = refresh_thunk;
    missing_tail_jump[22] = 0xC3;
    Expect(
        !MatchesWearableDefinitionRefreshThunk(missing_tail_jump.data(), missing_tail_jump.size()),
        "a CEconWearable callback without the verified refresh tail-call must fail closed");

    Expect(
        !MatchesWearableDefinitionRefreshThunk(refresh_thunk.data(), refresh_thunk.size() - 1),
        "a truncated CEconWearable client-think body must fail closed");
}

void TestKnifeAppearanceModule()
{
    using nskinzcs2::runtime::BuildKnifeAppearanceRequest;
    using nskinzcs2::runtime::BuildSyntheticKnifeEconItemHigh;
    using nskinzcs2::runtime::BuildKnifeFinishRequest;
    using nskinzcs2::runtime::BuildKnifeProfileApplySignature;
    using nskinzcs2::runtime::EvaluateKnifeApplyGate;
    using nskinzcs2::runtime::EvaluateKnifeRenderVerification;
    using nskinzcs2::runtime::KnifeApplyGateInputs;
    using nskinzcs2::runtime::KnifeApplyGateReason;
    using nskinzcs2::runtime::KnifeFirstPersonOwnerKind;
    using nskinzcs2::runtime::KnifeProfileRequiresModelDefinition;
    using nskinzcs2::runtime::KnifeRenderVerificationInputs;
    using nskinzcs2::runtime::KnifeRenderVerificationReason;
    using nskinzcs2::runtime::ResolveKnifeAppearancePipelineRoute;
    using nskinzcs2::runtime::ResolveKnifeSlotMode;
    using nskinzcs2::runtime::ResolveKnifeEconItemHigh;
    using nskinzcs2::runtime::IsKnifeEconItemHighCompatible;

    Expect(
        BuildSyntheticKnifeEconItemHigh(507) == 0xC52101FBu,
        "default karambit materialisation should encode definition 507 in the synthetic high word");
    Expect(
        ResolveKnifeEconItemHigh(0x0000000Cu, 515) == 0x0000000Cu,
        "real knife inventory identities must survive a model replacement");
    Expect(
        ResolveKnifeEconItemHigh(0xFFFFFFFFu, 515) == 0xC5210203u,
        "invalid default knife identities should materialise for the target definition");
    Expect(
        IsKnifeEconItemHighCompatible(0xC5210203u, 515) &&
            !IsKnifeEconItemHighCompatible(0xC52101FBu, 515),
        "synthetic knife identity verification must reject a different target definition");
    Expect(
        IsKnifeEconItemHighCompatible(0x0000000Cu, 515),
        "real inventory high words should remain compatible with the replacement model");
    using nskinzcs2::runtime::ShouldApplyKnifeModelDefinitionOverride;
    using nskinzcs2::runtime::KnifeSlotMode;

    nskinzcs2::model::KnifeProfile finish_only_profile;
    finish_only_profile.enabled = true;
    finish_only_profile.xuid = 76561198000000000ull;
    finish_only_profile.mode = nskinzcs2::model::KnifeProfileMode::FinishOnly;
    finish_only_profile.paint_kit = 344;
    finish_only_profile.quality = 3;
    finish_only_profile.seed = 123;
    finish_only_profile.stat_trak = 9;
    finish_only_profile.wear = 0.2f;
    finish_only_profile.custom_name = "Knife";

    auto model_and_finish_profile = finish_only_profile;
    model_and_finish_profile.mode = nskinzcs2::model::KnifeProfileMode::ModelAndFinish;
    model_and_finish_profile.model_definition = 507;

    auto invalid_model_profile = model_and_finish_profile;
    invalid_model_profile.model_definition = 42;
    auto skeleton_model_profile = model_and_finish_profile;
    skeleton_model_profile.model_definition = 525;

    Expect(
        ResolveKnifeSlotMode(finish_only_profile) == KnifeSlotMode::FinishOnly,
        "finish-only knife profiles should stay in the finish-only mode");
    Expect(
        ResolveKnifeSlotMode(model_and_finish_profile) == KnifeSlotMode::ModelAndFinish,
        "model-and-finish knife profiles with a supported model should resolve to model-and-finish");
    Expect(
        ResolveKnifeSlotMode(invalid_model_profile) == KnifeSlotMode::FinishOnly,
        "model-and-finish knife profiles with an invalid model should degrade to finish-only");
    Expect(
        ResolveKnifeSlotMode(skeleton_model_profile) == KnifeSlotMode::ModelAndFinish,
        "supported late-added knife definitions should stay in model-and-finish mode");

    const auto finish_request = BuildKnifeAppearanceRequest(finish_only_profile);
    const auto model_request = BuildKnifeAppearanceRequest(model_and_finish_profile);
    const auto invalid_model_request = BuildKnifeAppearanceRequest(invalid_model_profile);
    const auto skeleton_model_request = BuildKnifeAppearanceRequest(skeleton_model_profile);
    Expect(finish_request.xuid == finish_only_profile.xuid, "knife request should preserve xuid");
    Expect(finish_request.mode == KnifeSlotMode::FinishOnly, "finish-only request should stay finish-only");
    Expect(model_request.mode == KnifeSlotMode::ModelAndFinish, "valid knife request should preserve model-and-finish");

    auto finish_with_stale_m9 = finish_only_profile;
    finish_with_stale_m9.model_definition = 508;
    const auto stale_m9_request = BuildKnifeAppearanceRequest(finish_with_stale_m9);
    Expect(stale_m9_request.mode == KnifeSlotMode::FinishOnly && stale_m9_request.model_definition == 0,
        "finish-only must ignore an M9 saved by an older panel without enabling model replacement");
    Expect(stale_m9_request.paint_kit == finish_request.paint_kit &&
        stale_m9_request.seed == finish_request.seed && stale_m9_request.wear == finish_request.wear,
        "ignoring a stale model must preserve the selected finish, seed and wear");
    Expect(BuildKnifeProfileApplySignature(stale_m9_request, stale_m9_request.xuid, 507, 0x55ull) ==
        BuildKnifeProfileApplySignature(finish_request, finish_request.xuid, 507, 0x55ull),
        "a hidden stale M9 selection must not alter finish-only material identity");

    auto vanilla_profile = finish_only_profile;
    vanilla_profile.paint_kit = 0;
    vanilla_profile.explicit_vanilla_finish = true;
    const auto vanilla_request = BuildKnifeAppearanceRequest(vanilla_profile);
    auto no_change_request = vanilla_request;
    no_change_request.explicit_vanilla_finish = false;
    Expect(vanilla_request.explicit_vanilla_finish, "knife request should preserve explicit vanilla intent");
    auto model_without_finish = model_and_finish_profile;
    model_without_finish.paint_kit = 0;
    model_without_finish.explicit_vanilla_finish = false;
    Expect(BuildKnifeAppearanceRequest(model_without_finish).explicit_vanilla_finish &&
        BuildKnifeFinishRequest(model_without_finish).explicit_vanilla_finish,
        "model plus dash must use the target knife's stock material, never its source finish");
    model_without_finish.mode = nskinzcs2::model::KnifeProfileMode::FinishOnly;
    Expect(!BuildKnifeFinishRequest(model_without_finish).explicit_vanilla_finish,
        "finish-only dash must still mean no paint modification");
    model_without_finish.mode = nskinzcs2::model::KnifeProfileMode::ModelAndFinish;
    model_without_finish.model_definition = 0;
    Expect(!BuildKnifeAppearanceRequest(model_without_finish).explicit_vanilla_finish,
        "an unselected model must not imply destructive paint removal");
    Expect(
        BuildKnifeProfileApplySignature(vanilla_request, vanilla_request.xuid, 42, 0x55ull) !=
            BuildKnifeProfileApplySignature(no_change_request, no_change_request.xuid, 42, 0x55ull),
        "knife signature should distinguish dash from explicit vanilla");
    Expect(model_request.model_definition == 507, "valid knife request should preserve the selected knife model");
    Expect(
        skeleton_model_request.mode == KnifeSlotMode::ModelAndFinish &&
            skeleton_model_request.model_definition == 525,
        "supported late-added knife definitions should survive request normalization");
    Expect(
        invalid_model_request.mode == KnifeSlotMode::FinishOnly && invalid_model_request.model_definition == 0,
        "invalid knife model definitions should degrade to a finish-only runtime request");
    Expect(
        !KnifeProfileRequiresModelDefinition(finish_request.mode),
        "finish-only knife runtime should not require a model definition");
    Expect(
        KnifeProfileRequiresModelDefinition(model_request.mode),
        "model-and-finish knife runtime should require a model definition");

    KnifeApplyGateInputs finish_inputs;
    finish_inputs.request = finish_request;
    finish_inputs.context.watched_xuid = finish_request.xuid;
    finish_inputs.context.watched_target = true;
    finish_inputs.context.active_weapon_is_knife = true;
    finish_inputs.context.world_carrier_valid = true;
    finish_inputs.context.world_item_view_valid = true;
    finish_inputs.context.first_person_source_valid = true;
    finish_inputs.context.direct_first_person_render_valid = true;
    finish_inputs.context.hud_proxy_fallback_allowed = true;
    finish_inputs.context.knife_sync_ready = true;

    const auto finish_gate = EvaluateKnifeApplyGate(finish_inputs);
    Expect(finish_gate.allow_finish_writes, "finish-only knife gate should allow stable finish writes");
    Expect(finish_gate.resolved_mode == KnifeSlotMode::FinishOnly, "finish-only knife gate should stay in finish-only");
    Expect(
        finish_gate.reason == KnifeApplyGateReason::ReadyForFinishOnly,
        "finish-only knife gate should report the stable finish-only path");
    const auto finish_pipeline_route = ResolveKnifeAppearancePipelineRoute(finish_gate);
    Expect(
        finish_pipeline_route.use_exact_appearance_pipeline &&
            !finish_pipeline_route.allow_model_rebind,
        "finish-only knife profiles should use the exact carrier/material pipeline without allowing model rebinds");
    Expect(
        !ShouldApplyKnifeModelDefinitionOverride(finish_request, 42),
        "finish-only knife runtime should never request a knife definition override");

    KnifeApplyGateInputs model_inputs = finish_inputs;
    model_inputs.request = model_request;
    model_inputs.context.viewmodel_carrier_valid = true;
    model_inputs.context.first_person_source_valid = true;
    model_inputs.context.first_person_owner_kind = KnifeFirstPersonOwnerKind::ViewModel;
    model_inputs.context.animation_hook_ready = true;
    model_inputs.context.attack_classifier_ready = true;
    const auto model_gate = EvaluateKnifeApplyGate(model_inputs);
    Expect(
        model_gate.allow_finish_writes && model_gate.allow_model_writes && model_gate.allow_animation_remap,
        "model-and-finish knife gate should unlock world, first-person and animation writes");
    Expect(
        model_gate.animation_binding_ready && model_gate.animation_requires_strict_verification,
        "model-and-finish knife gate should require strict verification before deploy/inspect remap");
    Expect(
        model_gate.resolved_mode == KnifeSlotMode::ModelAndFinish &&
            model_gate.resolved_model_definition == model_request.model_definition,
        "model-and-finish knife gate should preserve the resolved model definition");
    Expect(
        model_gate.reason == KnifeApplyGateReason::ReadyForModelAndFinish,
        "fully satisfied knife model gate should enter the model-and-finish path");
    const auto model_pipeline_route = ResolveKnifeAppearancePipelineRoute(model_gate);
    Expect(
        model_pipeline_route.use_exact_appearance_pipeline &&
            model_pipeline_route.allow_model_rebind,
        "model-and-finish knife profiles should retain the exact pipeline model-rebind route");
    Expect(
        ShouldApplyKnifeModelDefinitionOverride(model_request, 42),
        "model-and-finish knife runtime should request a definition override when the target differs");

    KnifeApplyGateInputs providee_only_inputs = model_inputs;
    providee_only_inputs.context.viewmodel_carrier_valid = false;
    providee_only_inputs.context.providee_carrier_valid = true;
    providee_only_inputs.context.first_person_owner_kind = KnifeFirstPersonOwnerKind::Providee;
    const auto providee_only_gate = EvaluateKnifeApplyGate(providee_only_inputs);
    Expect(
        providee_only_gate.allow_finish_writes &&
            providee_only_gate.degraded &&
            !providee_only_gate.allow_model_writes &&
            !providee_only_gate.allow_animation_remap &&
            providee_only_gate.reason == KnifeApplyGateReason::MissingFirstPersonBinding,
        "providee-only first-person evidence must stay on the finish-only path until a real viewmodel carrier is verified");
    Expect(
        providee_only_gate.resolved_model_definition == model_request.model_definition,
        "providee-only finish-only knife paths should still preserve the resolved target definition for the query swap path");

    KnifeApplyGateInputs resource_swap_inputs = providee_only_inputs;
    resource_swap_inputs.context.resource_query_bridge_ready = true;
    resource_swap_inputs.context.resource_model_swap_ready = false;
    resource_swap_inputs.context.resource_query_redirect_pairs = 2;
    resource_swap_inputs.context.resource_query_redirect_hits = 0;
    const auto resource_swap_gate = EvaluateKnifeApplyGate(resource_swap_inputs);
    Expect(
        resource_swap_gate.allow_finish_writes &&
            !resource_swap_gate.allow_model_writes &&
            !resource_swap_gate.allow_animation_remap &&
            resource_swap_gate.degraded &&
            resource_swap_gate.resolved_mode == KnifeSlotMode::FinishOnly,
        "resource-query-backed knife swaps must stay on the finish-only path until the resource layer marks the bridge state as ready for model writes");
    Expect(
        resource_swap_gate.reason == KnifeApplyGateReason::MissingFirstPersonBinding,
        "resource-query-backed knife swaps must not report the model path as ready when the bridge has not exposed a writable target handle, hit, or readback");
    Expect(
        resource_swap_gate.resolved_model_definition == model_request.model_definition,
        "resource-query-backed knife swaps should preserve the resolved target definition");

    auto resource_swap_handle_inputs = resource_swap_inputs;
    resource_swap_handle_inputs.context.resource_query_target_handle_ready = true;
    resource_swap_handle_inputs.context.resource_model_swap_ready = true;
    const auto resource_swap_handle_gate = EvaluateKnifeApplyGate(resource_swap_handle_inputs);
    Expect(
        resource_swap_handle_gate.allow_finish_writes &&
            resource_swap_handle_gate.allow_model_writes &&
            !resource_swap_handle_gate.allow_animation_remap &&
            resource_swap_handle_gate.degraded &&
            resource_swap_handle_gate.resolved_mode == KnifeSlotMode::ModelAndFinish,
        "resource-query-backed knife swaps should unlock model replacement once the bridge exposes a stable writable target handle");
    Expect(
        resource_swap_handle_gate.reason == KnifeApplyGateReason::ReadyForModelAndFinish,
        "a stable bridge target handle should mark the resource-layer model path as ready");

    auto resource_swap_hit_inputs = resource_swap_inputs;
    resource_swap_hit_inputs.context.resource_model_swap_ready = true;
    resource_swap_hit_inputs.context.resource_query_redirect_hits = 1;
    const auto resource_swap_hit_gate = EvaluateKnifeApplyGate(resource_swap_hit_inputs);
    Expect(
        resource_swap_hit_gate.allow_finish_writes &&
            resource_swap_hit_gate.allow_model_writes &&
            !resource_swap_hit_gate.allow_animation_remap &&
            resource_swap_hit_gate.degraded &&
            resource_swap_hit_gate.resolved_mode == KnifeSlotMode::ModelAndFinish,
        "resource-query-backed knife swaps should unlock model replacement after the hook records a real redirected resource hit");
    Expect(
        resource_swap_hit_gate.reason == KnifeApplyGateReason::ReadyForModelAndFinish,
        "resource-query-backed knife swaps should report the resource-layer model path as ready after a real redirected resource hit");

    auto resource_swap_readback_inputs = resource_swap_inputs;
    resource_swap_readback_inputs.context.resource_model_swap_ready = true;
    resource_swap_readback_inputs.context.resource_model_path_readback_changed = true;
    resource_swap_readback_inputs.context.resource_world_model_matches_target = true;
    const auto resource_swap_readback_gate = EvaluateKnifeApplyGate(resource_swap_readback_inputs);
    Expect(
        resource_swap_readback_gate.allow_finish_writes &&
            resource_swap_readback_gate.allow_model_writes &&
            !resource_swap_readback_gate.allow_animation_remap &&
            resource_swap_readback_gate.degraded &&
            resource_swap_readback_gate.resolved_mode == KnifeSlotMode::ModelAndFinish,
        "resource-query-backed knife swaps should unlock model replacement after live model-path readback confirms the target model");
    Expect(
        resource_swap_readback_gate.reason == KnifeApplyGateReason::ReadyForModelAndFinish,
        "live model-path readback confirmation should mark the resource-layer model path as ready");

    KnifeApplyGateInputs missing_watched_inputs = model_inputs;
    missing_watched_inputs.context.watched_target = false;
    const auto missing_watched_gate = EvaluateKnifeApplyGate(missing_watched_inputs);
    Expect(
        missing_watched_gate.allow_finish_writes &&
            missing_watched_gate.degraded &&
            missing_watched_gate.resolved_mode == KnifeSlotMode::FinishOnly,
        "model-and-finish knife runtime should still keep the finish-only path when safe first-person evidence is available");
    Expect(
        missing_watched_gate.reason == KnifeApplyGateReason::MissingWatchedKnife,
        "non-watched knife model paths should report the watched-player degrade reason");

    KnifeApplyGateInputs missing_watched_resource_hit_inputs = resource_swap_hit_inputs;
    missing_watched_resource_hit_inputs.context.watched_target = false;
    const auto missing_watched_resource_hit_gate =
        EvaluateKnifeApplyGate(missing_watched_resource_hit_inputs);
    Expect(
        missing_watched_resource_hit_gate.allow_finish_writes &&
            missing_watched_resource_hit_gate.allow_model_writes &&
            !missing_watched_resource_hit_gate.allow_animation_remap &&
            missing_watched_resource_hit_gate.degraded &&
            missing_watched_resource_hit_gate.resolved_mode == KnifeSlotMode::ModelAndFinish,
        "resource-query-backed knife swaps should stay on the model path after a real redirected hit even when the watched target moved away");
    Expect(
        missing_watched_resource_hit_gate.reason == KnifeApplyGateReason::ReadyForModelAndFinish,
        "resource-query-backed knife swaps should not degrade to MissingWatchedKnife after a real redirected hit");

    KnifeApplyGateInputs degraded_classifier_inputs = model_inputs;
    degraded_classifier_inputs.context.attack_classifier_ready = false;
    const auto degraded_classifier_gate = EvaluateKnifeApplyGate(degraded_classifier_inputs);
    Expect(
        degraded_classifier_gate.allow_finish_writes &&
            degraded_classifier_gate.allow_model_writes &&
            degraded_classifier_gate.allow_animation_remap &&
            degraded_classifier_gate.animation_binding_ready &&
            degraded_classifier_gate.animation_requires_strict_verification &&
            !degraded_classifier_gate.degraded &&
            degraded_classifier_gate.resolved_mode == KnifeSlotMode::ModelAndFinish,
        "uncalibrated knife attack classifiers should not block deploy/inspect-ready knife swaps");
    Expect(
        degraded_classifier_gate.reason == KnifeApplyGateReason::ReadyForModelAndFinish,
        "attack classifier readiness should not participate in the deploy/inspect-ready knife gate");

    KnifeApplyGateInputs missing_first_person_inputs = model_inputs;
    missing_first_person_inputs.context.viewmodel_carrier_valid = false;
    missing_first_person_inputs.context.first_person_source_valid = false;
    missing_first_person_inputs.context.first_person_owner_kind = KnifeFirstPersonOwnerKind::None;
    const auto missing_first_person_gate = EvaluateKnifeApplyGate(missing_first_person_inputs);
    Expect(
        missing_first_person_gate.allow_finish_writes &&
            missing_first_person_gate.resolved_mode == KnifeSlotMode::FinishOnly &&
            !missing_first_person_gate.allow_animation_remap &&
            missing_first_person_gate.reason == KnifeApplyGateReason::MissingFirstPersonBinding,
        "world-only knife evidence should allow carrier finish writes while model and animation writes remain closed");
    const auto blocked_pipeline_route = ResolveKnifeAppearancePipelineRoute(missing_first_person_gate);
    Expect(
        blocked_pipeline_route.use_exact_appearance_pipeline &&
            !blocked_pipeline_route.allow_model_rebind,
        "world-only knife profiles should use the exact finish route without entering model rebind");

    KnifeApplyGateInputs unsafe_finish_only_inputs = finish_inputs;
    unsafe_finish_only_inputs.context.watched_target = true;
    unsafe_finish_only_inputs.context.first_person_source_valid = false;
    unsafe_finish_only_inputs.context.providee_carrier_valid = false;
    unsafe_finish_only_inputs.context.viewmodel_carrier_valid = false;
    unsafe_finish_only_inputs.context.hud_proxy_fallback_allowed = false;
    unsafe_finish_only_inputs.context.direct_first_person_render_valid = false;
    const auto unsafe_finish_only_gate = EvaluateKnifeApplyGate(unsafe_finish_only_inputs);
    Expect(
        unsafe_finish_only_gate.allow_finish_writes &&
            unsafe_finish_only_gate.resolved_mode == KnifeSlotMode::FinishOnly &&
            unsafe_finish_only_gate.reason == KnifeApplyGateReason::ReadyForFinishOnly,
        "finish-only watched knife paths should accept authoritative carrier writes without requiring a first-person model owner");

    KnifeApplyGateInputs direct_render_only_inputs = model_inputs;
    direct_render_only_inputs.context.viewmodel_carrier_valid = false;
    direct_render_only_inputs.context.first_person_owner_kind = KnifeFirstPersonOwnerKind::None;
    direct_render_only_inputs.context.direct_first_person_render_valid = true;
    direct_render_only_inputs.context.direct_first_person_render_only = true;
    direct_render_only_inputs.context.hud_proxy_fallback_allowed = true;
    const auto direct_render_only_gate = EvaluateKnifeApplyGate(direct_render_only_inputs);
    Expect(
        direct_render_only_gate.allow_finish_writes &&
            direct_render_only_gate.degraded &&
            !direct_render_only_gate.allow_model_writes &&
            !direct_render_only_gate.allow_animation_remap &&
            !direct_render_only_gate.animation_binding_ready &&
            !direct_render_only_gate.animation_requires_strict_verification &&
            direct_render_only_gate.resolved_mode == KnifeSlotMode::FinishOnly &&
            direct_render_only_gate.reason == KnifeApplyGateReason::MissingFirstPersonBinding,
        "direct-render-only knife evidence should now fall back to finish-only until a real first-person owner is resolved");
    Expect(
        direct_render_only_gate.resolved_model_definition == model_request.model_definition,
        "direct-render-only finish-only knife paths should still preserve the resolved target definition for query-path swapping");

    KnifeApplyGateInputs blocked_direct_render_inputs = direct_render_only_inputs;
    blocked_direct_render_inputs.context.hud_proxy_fallback_allowed = false;
    const auto blocked_direct_render_gate = EvaluateKnifeApplyGate(blocked_direct_render_inputs);
    Expect(
        blocked_direct_render_gate.allow_finish_writes &&
            blocked_direct_render_gate.resolved_mode == KnifeSlotMode::FinishOnly &&
            !blocked_direct_render_gate.allow_model_writes &&
            blocked_direct_render_gate.reason == KnifeApplyGateReason::MissingFirstPersonBinding,
        "direct-render-only knife evidence without HUD fallback should retain carrier finish writes while model writes fail closed");

    KnifeApplyGateInputs observer_invalid_inputs = model_inputs;
    observer_invalid_inputs.context.observer_chain_present = true;
    observer_invalid_inputs.context.observer_in_eye_valid = false;
    const auto observer_invalid_gate = EvaluateKnifeApplyGate(observer_invalid_inputs);
    Expect(
        !observer_invalid_gate.allow_finish_writes &&
            !observer_invalid_gate.allow_model_writes &&
            observer_invalid_gate.reason == KnifeApplyGateReason::MissingWorldBinding,
        "observer-first knife model path must stay blocked when the view leaves stable In-Eye");

    KnifeApplyGateInputs observer_mismatch_inputs = model_inputs;
    observer_mismatch_inputs.context.observer_chain_present = true;
    observer_mismatch_inputs.context.observer_in_eye_valid = true;
    observer_mismatch_inputs.context.observer_active_weapon_is_knife = true;
    observer_mismatch_inputs.context.real_viewmodel_entity_index = 171;
    observer_mismatch_inputs.context.real_viewmodel_handle_invalid = true;
    observer_mismatch_inputs.context.real_viewmodel_weapon_matches_active_weapon = false;
    const auto observer_mismatch_gate = EvaluateKnifeApplyGate(observer_mismatch_inputs);
    Expect(
        observer_mismatch_gate.allow_finish_writes &&
            observer_mismatch_gate.degraded &&
            !observer_mismatch_gate.allow_model_writes &&
            observer_mismatch_gate.reason == KnifeApplyGateReason::MissingFirstPersonBinding,
        "observer-first knife model path must degrade when viewmodel weapon binding drifts away from the active knife");
    Expect(
        observer_mismatch_gate.resolved_model_definition == model_request.model_definition,
        "observer-degraded finish-only knife paths should keep the resolved target definition while strict model writes stay blocked");

    KnifeApplyGateInputs bridge_nonknife_inputs = direct_render_only_inputs;
    bridge_nonknife_inputs.context.bridged_pawn_entity_index = 271;
    bridge_nonknife_inputs.context.bridge_active_weapon_entity_index = 503;
    bridge_nonknife_inputs.context.bridge_active_weapon_is_knife = false;
    const auto bridge_nonknife_gate = EvaluateKnifeApplyGate(bridge_nonknife_inputs);
    Expect(
        !bridge_nonknife_gate.allow_finish_writes &&
            !bridge_nonknife_gate.allow_model_writes &&
            bridge_nonknife_gate.reason == KnifeApplyGateReason::MissingWatchedKnife,
        "bridge-resolved watched knife paths must stay blocked when the bridged active weapon is not a knife");

    KnifeApplyGateInputs observer_verified_inputs = model_inputs;
    observer_verified_inputs.context.observer_chain_present = true;
    observer_verified_inputs.context.observer_in_eye_valid = true;
    observer_verified_inputs.context.observer_active_weapon_is_knife = true;
    observer_verified_inputs.context.real_viewmodel_handle_valid = true;
    observer_verified_inputs.context.real_viewmodel_weapon_matches_active_weapon = true;
    observer_verified_inputs.context.real_viewmodel_active_weapon_verified_knife = true;
    const auto observer_verified_gate = EvaluateKnifeApplyGate(observer_verified_inputs);
    Expect(
        observer_verified_gate.allow_finish_writes &&
            observer_verified_gate.allow_model_writes &&
            observer_verified_gate.allow_animation_remap &&
            observer_verified_gate.reason == KnifeApplyGateReason::ReadyForModelAndFinish,
        "observer-first verified real viewmodel bindings should unlock the full knife model path");

    KnifeApplyGateInputs missing_world_inputs = model_inputs;
    missing_world_inputs.context.world_carrier_valid = false;
    const auto missing_world_gate = EvaluateKnifeApplyGate(missing_world_inputs);
    Expect(!missing_world_gate.allow_finish_writes, "missing world bindings should block the knife slot entirely");
    Expect(
        missing_world_gate.reason == KnifeApplyGateReason::MissingWorldBinding,
        "missing world bindings should report the dedicated knife gate reason");

    const auto finish_signature =
        BuildKnifeProfileApplySignature(finish_request, finish_request.xuid, 42, 0x55ull);
    Expect(
        BuildKnifeProfileApplySignature(invalid_model_request, invalid_model_request.xuid, 42, 0x55ull) == finish_signature,
        "degraded finish-only knife requests should keep the stable finish-only signature");

    auto model_changed_profile = model_and_finish_profile;
    model_changed_profile.model_definition = 523;
    const auto model_changed_request = BuildKnifeAppearanceRequest(model_changed_profile);
    Expect(
        BuildKnifeProfileApplySignature(model_request, model_request.xuid, 42, 0x55ull) !=
            BuildKnifeProfileApplySignature(model_changed_request, model_changed_request.xuid, 42, 0x55ull),
        "resolved model-and-finish knife signatures should change when the selected model changes");

    auto name_changed = finish_request;
    name_changed.custom_name = "Other";
    Expect(
        BuildKnifeProfileApplySignature(name_changed, finish_request.xuid, 42, 0x55ull) != finish_signature,
        "changing knife name tag should change the apply signature");
    auto paint_changed = finish_request;
    paint_changed.paint_kit = 345;
    Expect(
        BuildKnifeProfileApplySignature(paint_changed, finish_request.xuid, 42, 0x55ull) != finish_signature,
        "changing knife paint should change the apply signature");

    {
        KnifeRenderVerificationInputs inputs;
        inputs.world_render_state_resolved = true;
        inputs.world_render_expected_model_resolved = true;
        inputs.world_render_model_matches_target = true;
        inputs.world_render_mesh_matches_target = true;
        inputs.first_person_render_state_resolved = true;
        inputs.first_person_render_expected_model_resolved = true;
        inputs.first_person_render_model_matches_target = true;
        inputs.first_person_render_mesh_matches_target = true;

        const auto verification = EvaluateKnifeRenderVerification(inputs);
        Expect(
            verification.verified &&
                verification.reason == KnifeRenderVerificationReason::Verified,
            "knife post-refresh verification should pass only when world and first-person render targets both match");
    }

    {
        KnifeRenderVerificationInputs inputs;
        inputs.world_render_state_resolved = true;
        inputs.world_render_expected_model_resolved = true;
        inputs.world_render_model_matches_target = true;
        inputs.world_render_mesh_matches_target = true;
        inputs.first_person_render_state_resolved = true;
        inputs.first_person_render_expected_model_resolved = true;
        inputs.first_person_render_untrusted = true;
        inputs.first_person_render_model_matches_target = true;
        inputs.first_person_render_mesh_matches_target = true;

        const auto verification = EvaluateKnifeRenderVerification(inputs);
        Expect(
            !verification.verified &&
                verification.reason == KnifeRenderVerificationReason::UntrustedFirstPersonRender,
            "untrusted synthetic first-person render targets must fail strict knife verification");
    }

    {
        KnifeRenderVerificationInputs inputs;
        inputs.world_render_state_resolved = true;
        inputs.world_render_expected_model_resolved = true;
        inputs.world_render_model_matches_target = false;
        inputs.world_render_mesh_matches_target = true;
        inputs.first_person_render_state_resolved = true;
        inputs.first_person_render_expected_model_resolved = true;
        inputs.first_person_render_model_matches_target = true;
        inputs.first_person_render_mesh_matches_target = true;

        const auto verification = EvaluateKnifeRenderVerification(inputs);
        Expect(
            !verification.verified &&
                verification.reason == KnifeRenderVerificationReason::WorldRenderModelMismatch,
            "world render model mismatches must force knife model+finish back to fail-closed");
    }
}

void TestKnifePhase2WorldOnlyModule()
{
    using nskinzcs2::runtime::KnifePhase2ArmProbeInput;
    using nskinzcs2::runtime::KnifePhase2TargetMode;
    using nskinzcs2::runtime::KnifePhase2TargetSelectionProbeInput;
    using nskinzcs2::runtime::KnifeViewmodelAbsenceAuditResult;
    using nskinzcs2::runtime::KnifeWorldOnlyRenderVerificationProbeInput;
    using nskinzcs2::runtime::ProbeEvaluateKnifeWorldOnlyRenderVerificationForTests;
    using nskinzcs2::runtime::ProbeKnifePhase2ArmGateForTests;
    using nskinzcs2::runtime::ProbeResolveKnifePhase2TargetSelectionForTests;

    {
        KnifePhase2TargetSelectionProbeInput input;
        input.has_real_first_person_render_target = true;

        Expect(
            ProbeResolveKnifePhase2TargetSelectionForTests(input) ==
                KnifePhase2TargetMode::FirstPersonRender,
            "phase2 target selection must preserve a real non-synthetic first-person render target");
    }

    {
        KnifePhase2TargetSelectionProbeInput input;
        input.no_exposed_non_synthetic_render_owner = true;
        input.viewmodel_absence_result = KnifeViewmodelAbsenceAuditResult::MissingHandle;
        input.consumer_clusters = 1;
        input.entity_bound_clusters = 1;
        input.anonymous_clusters = 0;
        input.single_consumer_is_entity_bound_world = true;
        input.caller_matches_valid_hit = true;

        Expect(
            ProbeResolveKnifePhase2TargetSelectionForTests(input) ==
                KnifePhase2TargetMode::WorldOnlyConsumer,
            "missing-handle single-world-consumer evidence must arm the world-only phase2 target");
    }

    {
        KnifePhase2TargetSelectionProbeInput input;
        input.no_exposed_non_synthetic_render_owner = true;
        input.viewmodel_absence_result = KnifeViewmodelAbsenceAuditResult::MissingHandle;
        input.consumer_clusters = 2;
        input.entity_bound_clusters = 1;
        input.anonymous_clusters = 1;
        input.single_consumer_is_entity_bound_world = false;
        input.caller_matches_valid_hit = true;

        Expect(
            ProbeResolveKnifePhase2TargetSelectionForTests(input) ==
                KnifePhase2TargetMode::None,
            "any anonymous knife query consumer must block world-only phase2 arming");
    }

    {
        KnifeWorldOnlyRenderVerificationProbeInput input;
        input.world_render_state_resolved = true;
        input.world_render_expected_model_resolved = true;
        input.world_render_model_matches_target = true;
        input.world_render_handle_matches_target_or_donor = true;
        input.world_render_mesh_matches_target = true;
        input.resource_hits = 14;
        input.resource_readback = true;

        Expect(
            ProbeEvaluateKnifeWorldOnlyRenderVerificationForTests(input),
            "world-only verification must pass only when world model, handle, mesh, hits and readback all align");

        input.resource_hits = 0;
        Expect(
            !ProbeEvaluateKnifeWorldOnlyRenderVerificationForTests(input),
            "world-only verification must fail once resource hits disappear");

        input.resource_hits = 14;
        input.resource_readback = false;
        Expect(
            !ProbeEvaluateKnifeWorldOnlyRenderVerificationForTests(input),
            "world-only verification must fail without resource readback");

        input.resource_readback = true;
        input.world_render_handle_matches_target_or_donor = false;
        Expect(
            !ProbeEvaluateKnifeWorldOnlyRenderVerificationForTests(input),
            "world-only verification must fail when the live world handle matches neither target nor donor");
    }

    {
        KnifePhase2ArmProbeInput input;
        input.target_mode = KnifePhase2TargetMode::WorldOnlyConsumer;
        input.world_render_entity_index = 215;
        input.first_person_render_entity_index = -73;
        input.first_person_render_entity_synthetic = true;

        const auto result = ProbeKnifePhase2ArmGateForTests(input);
        Expect(result.armed, "world-only phase2 target should arm once the census path is confirmed");
        Expect(
            result.target_mode == KnifePhase2TargetMode::WorldOnlyConsumer,
            "world-only phase2 arm must preserve the explicit target mode");
        Expect(
            result.render_entity_index == 215,
            "world-only phase2 arm must bind the real world render entity instead of a synthetic first-person proxy");
        Expect(
            !result.synthetic_render_proxy,
            "world-only phase2 arm must not mark the target as synthetic");
    }
}

void TestKnifeExactWorldBindingModule()
{
    using nskinzcs2::runtime::ExactWorldWeaponBindingProbeInput;
    using nskinzcs2::runtime::KnifeFirstPersonOwnerKind;
    using nskinzcs2::runtime::KnifeModelPathBindingMatchProbeInput;
    using nskinzcs2::runtime::KnifePersistentOwnerReacquireProbeInput;
    using nskinzcs2::runtime::ProbeExactWorldWeaponApplicationBindingForTests;
    using nskinzcs2::runtime::ProbeKnifeModelPathStateMatchesBindingForTests;
    using nskinzcs2::runtime::ProbeKnifePersistentOwnerReacquireForTests;

    {
        ExactWorldWeaponBindingProbeInput input;
        input.owned_weapon_entity_index = 68;
        input.owned_weapon_has_item_view = true;
        input.explicit_world_child_count = 0;
        input.hud_entity_index = 600;

        const auto result = ProbeExactWorldWeaponApplicationBindingForTests(input);
        Expect(result.success, "exact world binding should resolve for a plain owned knife entity");
        Expect(result.carrier_entity_index == 68, "without an explicit child the owned knife stays as the carrier");
        Expect(result.render_entity_index == 68, "without an explicit child the owned knife stays as the render consumer");
        Expect(result.hud_entity_index == 600, "exact world binding should preserve the HUD entity index");
    }

    {
        ExactWorldWeaponBindingProbeInput input;
        input.owned_weapon_entity_index = 68;
        input.owned_weapon_has_item_view = true;
        input.explicit_world_child_count = 1;
        input.explicit_world_child_entity_index = 412;
        input.explicit_world_child_has_item_view = true;
        input.explicit_world_child_item_view_shared_with_owned = true;

        const auto result = ProbeExactWorldWeaponApplicationBindingForTests(input);
        Expect(result.success, "exact world binding should resolve for a unique explicit child");
        Expect(result.carrier_entity_index == 68, "a shared explicit child should keep the owned knife as the carrier");
        Expect(result.render_entity_index == 412, "a shared explicit child should become the world render consumer");
        Expect(
            result.render_entity_is_explicit_owner_child && !result.carrier_entity_is_explicit_owner_child,
            "shared explicit children should only replace the render consumer");
    }

    {
        ExactWorldWeaponBindingProbeInput input;
        input.owned_weapon_entity_index = 68;
        input.owned_weapon_has_item_view = true;
        input.explicit_world_child_count = 1;
        input.explicit_world_child_entity_index = 412;
        input.explicit_world_child_has_item_view = true;
        input.explicit_world_child_item_view_shared_with_owned = false;

        const auto result = ProbeExactWorldWeaponApplicationBindingForTests(input);
        Expect(result.success, "exact world binding should resolve when the explicit child has its own item view");
        Expect(result.carrier_entity_index == 412, "an explicit child with its own item view should become the carrier");
        Expect(result.render_entity_index == 412, "an explicit child with its own item view should also become the render consumer");
        Expect(
            result.render_entity_is_explicit_owner_child && result.carrier_entity_is_explicit_owner_child,
            "an independent explicit child should replace both carrier and render consumer");
    }

    {
        KnifeModelPathBindingMatchProbeInput input;
        input.state_active = true;
        input.state_xuid = 76561198086901534ull;
        input.state_world_entity_index = 68;
        input.state_world_render_entity_index = 412;
        input.state_first_person_entity_index = 900;
        input.state_target_definition_index = 507;
        input.state_owner_kind = KnifeFirstPersonOwnerKind::ViewModel;
        input.binding_xuid = 76561198086901534ull;
        input.binding_world_entity_index = 68;
        input.binding_world_render_entity_index = 412;
        input.binding_first_person_entity_index = 900;
        input.binding_target_definition_index = 507;
        input.binding_owner_kind = KnifeFirstPersonOwnerKind::ViewModel;

        Expect(
            ProbeKnifeModelPathStateMatchesBindingForTests(input),
            "matching world render bindings should preserve the knife model path state");

        input.binding_world_render_entity_index = 413;
        Expect(
            !ProbeKnifeModelPathStateMatchesBindingForTests(input),
            "changing only the world render entity should invalidate the knife model path state");

        input.binding_world_render_entity_index = 412;
        input.state_first_person_render_only = true;
        input.state_first_person_direct_render_source = true;
        input.binding_first_person_render_only = true;
        input.binding_first_person_direct_render_source = true;
        input.state_first_person_entity_index = -150926616;
        input.binding_first_person_entity_index = -150926616;
        Expect(
            ProbeKnifeModelPathStateMatchesBindingForTests(input),
            "guarded synthetic render-only first-person proxies should preserve the knife model path state while the binding stays stable");

        input.state_first_person_render_only = false;
        input.state_first_person_direct_render_source = false;
        input.binding_first_person_render_only = false;
        input.binding_first_person_direct_render_source = false;
        input.state_owner_kind = KnifeFirstPersonOwnerKind::ViewModel;
        input.binding_owner_kind = KnifeFirstPersonOwnerKind::ViewModel;
        input.state_first_person_entity_index = 241;
        input.binding_first_person_entity_index = 241;
        input.state_first_person_render_entity_index = -513497560;
        input.binding_first_person_render_entity_index = -495472580;
        input.state_first_person_render_entity_ptr = 0x2E106A45800ull;
        input.binding_first_person_render_entity_ptr = 0x2E106A45800ull;
        input.state_first_person_render_item_view_ptr = 0x2E106A46BC8ull;
        input.binding_first_person_render_item_view_ptr = 0x2E106A46BC8ull;
        input.state_first_person_render_model_state = 0x2E106A44000ull;
        input.binding_first_person_render_model_state = 0x2E106A44000ull;
        Expect(
            ProbeKnifeModelPathStateMatchesBindingForTests(input),
            "persistent real viewmodel owners should survive synthetic render proxy index churn when the render object identity stays stable");

        input.binding_first_person_render_entity_ptr = 0x2E106A45808ull;
        input.binding_first_person_render_model_state = 0x2E106A44008ull;
        Expect(
            !ProbeKnifeModelPathStateMatchesBindingForTests(input),
            "changing the synthetic render-object identity should invalidate the cached knife model path state");
    }

    {
        KnifePersistentOwnerReacquireProbeInput input;
        input.xuid = 76561198086901534ull;
        input.world_entity_index = 68;
        input.candidate_entity_index = 241;
        input.candidate_definition_index = 515;
        input.candidate_owner_kind = KnifeFirstPersonOwnerKind::ViewModel;
        input.first_observation_frame = 100;
        input.second_observation_frame = 101;

        const auto result = ProbeKnifePersistentOwnerReacquireForTests(input);
        Expect(
            result.first_pending && !result.first_promoted && result.first_confirm_hits == 1,
            "the first validated direct-render owner clue should stay pending until the second poll");
        Expect(
            !result.second_pending && result.second_promoted && result.second_confirm_hits == 2,
            "the second consecutive owner observation should promote the clue into a persistent first-person binding");
        Expect(
            result.first_person_entity_index == input.candidate_entity_index &&
                result.animation_entity_index == input.candidate_entity_index,
            "model-path and animation-path owner reacquire should converge on the same first-person entity");
        Expect(
            !result.direct_render_source && !result.render_only,
            "promoted direct-render owner clues should normalize into persistent non-render-only bindings");
        Expect(
            result.owner_kind == KnifeFirstPersonOwnerKind::ViewModel,
            "the normalized owner reacquire binding should keep a recoverable owner kind");
    }
}

void TestKnifeAnimationModule()
{
    using nskinzcs2::runtime::DetectKnifeAnimationTrigger;
    using nskinzcs2::runtime::KnifeAnimationAttackClassifierReady;
    using nskinzcs2::runtime::KnifeAnimationGameplayStateKnown;
    using nskinzcs2::runtime::KnifeAnimationInspectActive;
    using nskinzcs2::runtime::KnifeRuntimeContext;
    using nskinzcs2::runtime::KnifeAnimationStateSnapshot;
    using nskinzcs2::runtime::KnifeAnimationTrigger;
    using nskinzcs2::runtime::ProbePassiveWatchedKnifeTraceGateForTests;

    KnifeAnimationStateSnapshot first_snapshot;
    first_snapshot.valid = true;
    first_snapshot.xuid = 42;
    first_snapshot.apply_signature = 0x100ull;
    first_snapshot.world_entity_index = 12;
    first_snapshot.first_person_entity_index = 21;
    first_snapshot.source_definition_index = 42;
    first_snapshot.target_definition_index = 507;

    Expect(
        DetectKnifeAnimationTrigger({false, {}, first_snapshot}) ==
            KnifeAnimationTrigger::Deploy,
        "the first watched knife snapshot should trigger a deploy remap");

    auto second_snapshot = first_snapshot;
    second_snapshot.target_definition_index = 515;
    second_snapshot.apply_signature = 0x101ull;
    Expect(
        DetectKnifeAnimationTrigger({true, first_snapshot, second_snapshot}) ==
            KnifeAnimationTrigger::Deploy,
        "changing the knife target should retrigger deploy");

    auto inspect_snapshot = first_snapshot;
    inspect_snapshot.inspect_pending = true;
    inspect_snapshot.is_looking_at_weapon = true;
    Expect(
        KnifeAnimationInspectActive(inspect_snapshot),
        "inspect helper should treat pending look-at state as active inspect");
    Expect(
        DetectKnifeAnimationTrigger({true, first_snapshot, inspect_snapshot}) ==
            KnifeAnimationTrigger::Inspect,
        "inspect rising edge should trigger the inspect remap path");

    auto steady_snapshot = inspect_snapshot;
    Expect(
        DetectKnifeAnimationTrigger({true, inspect_snapshot, steady_snapshot}) ==
            KnifeAnimationTrigger::None,
        "steady knife animation state should not retrigger deploy or inspect");

    Expect(
        KnifeAnimationAttackClassifierReady(),
        "knife attack classifier should report ready once gameplay-state calibration is filled");

    auto light_attack_snapshot = first_snapshot;
    light_attack_snapshot.gameplay_state = 1;
    light_attack_snapshot.gameplay_state_timestamp = 1.0f;
    Expect(
        KnifeAnimationGameplayStateKnown(light_attack_snapshot),
        "calibrated light-attack gameplay states should remain observable");
    Expect(
        DetectKnifeAnimationTrigger({true, first_snapshot, light_attack_snapshot}) ==
            KnifeAnimationTrigger::LightAttack,
        "calibrated light-attack gameplay states should trigger attack remaps");

    auto light_attack_repeat = light_attack_snapshot;
    light_attack_repeat.gameplay_state_timestamp = 2.0f;
    Expect(
        DetectKnifeAnimationTrigger({true, light_attack_snapshot, light_attack_repeat}) ==
            KnifeAnimationTrigger::LightAttack,
        "timestamp changes should retrigger calibrated light-attack remaps");

    auto heavy_attack_snapshot = first_snapshot;
    heavy_attack_snapshot.gameplay_state = 2;
    heavy_attack_snapshot.gameplay_state_timestamp = 1.5f;
    Expect(
        KnifeAnimationGameplayStateKnown(heavy_attack_snapshot),
        "calibrated heavy-attack gameplay states should remain observable");
    Expect(
        DetectKnifeAnimationTrigger({true, first_snapshot, heavy_attack_snapshot}) ==
            KnifeAnimationTrigger::HeavyAttack,
        "calibrated heavy-attack remaps should trigger attack remaps");

    auto unknown_attack_snapshot = first_snapshot;
    unknown_attack_snapshot.gameplay_state = 99;
    Expect(
        !KnifeAnimationGameplayStateKnown(unknown_attack_snapshot),
        "unknown knife gameplay states should remain fail-closed once the attack classifier is calibrated");

    KnifeRuntimeContext passive_trace_context;
    passive_trace_context.demo_playing = true;
    passive_trace_context.schema_ready = true;
    passive_trace_context.entity_system_ready = true;
    passive_trace_context.watched_target = true;
    passive_trace_context.active_weapon_is_knife = true;
    passive_trace_context.watched_xuid = 42;
    passive_trace_context.knife_sync_ready = false;
    Expect(
        ProbePassiveWatchedKnifeTraceGateForTests(passive_trace_context, 42),
        "passive watched knife tracing should stay available before full model-sync readiness");

    passive_trace_context.watched_target = false;
    Expect(
        !ProbePassiveWatchedKnifeTraceGateForTests(passive_trace_context, 42),
        "passive watched knife tracing should stay blocked for non-watched targets");

    passive_trace_context.watched_target = true;
    passive_trace_context.active_weapon_is_knife = false;
    Expect(
        !ProbePassiveWatchedKnifeTraceGateForTests(passive_trace_context, 42),
        "passive watched knife tracing should stay blocked for non-knife weapons");

    passive_trace_context.active_weapon_is_knife = true;
    passive_trace_context.bridged_pawn_entity_index = 271;
    passive_trace_context.bridge_active_weapon_is_knife = false;
    Expect(
        !ProbePassiveWatchedKnifeTraceGateForTests(passive_trace_context, 42),
        "passive watched knife tracing should honor the bridge active-weapon knife gate");

    passive_trace_context.bridge_active_weapon_is_knife = true;
    Expect(
        !ProbePassiveWatchedKnifeTraceGateForTests(passive_trace_context, 43),
        "passive watched knife tracing should stay blocked when the runtime xuid does not match the watched target");
}

void TestKnifeHudAliasSyntheticProxyExperiment()
{
    using nskinzcs2::runtime::KnifeFirstPersonOwnerKind;
    using nskinzcs2::runtime::KnifeModelPathBindingMatchProbeInput;
    using nskinzcs2::runtime::ProbeAllowGuardedKnifeSyntheticRenderProxyForTests;
    using nskinzcs2::runtime::ProbeAllowGuardedKnifeSyntheticWorldRenderRebindForTests;
    using nskinzcs2::runtime::ProbeAllowKnifeModelOnlyAnimationHookForTests;
    using nskinzcs2::runtime::ProbeAllowKnifeVisualOnlyFallbackForTests;
    using nskinzcs2::runtime::ProbeAllowKnifeSyntheticWorldVisualIsolationForTests;
    using nskinzcs2::runtime::ProbeResolveKnifeSyntheticProxyDesiredMeshGroupMaskForTests;
    using nskinzcs2::runtime::ProbeShouldBlockUnsafeKnifeSyntheticRenderProxyCarrierWriteForTests;
    using nskinzcs2::runtime::ProbeShouldRollbackGuardedKnifeSyntheticFirstPersonRebindForTests;
    constexpr std::uint64_t kAllMeshGroupsMask = ~std::uint64_t {0};

    Expect(
        !ProbeAllowGuardedKnifeSyntheticRenderProxyForTests(true, true, true, false),
        "HUD-alias synthetic render proxies should stay blocked until the experiment is enabled");
    Expect(
        ProbeAllowGuardedKnifeSyntheticRenderProxyForTests(true, true, true, true),
        "HUD-alias synthetic render proxies should be allowed when the experiment is enabled");
    Expect(
        ProbeAllowGuardedKnifeSyntheticRenderProxyForTests(true, false, true, false),
        "non-HUD guarded synthetic render proxies should keep the existing guarded allow path");
    Expect(
        !ProbeAllowGuardedKnifeSyntheticRenderProxyForTests(true, true, false, true),
        "the experiment should still require readable restore state before allowing writes");
    Expect(
        ProbeResolveKnifeSyntheticProxyDesiredMeshGroupMaskForTests(
            true,
            kAllMeshGroupsMask,
            true,
            6,
            true,
            1) == kAllMeshGroupsMask,
        "guarded synthetic proxy rebinds should preserve the original raw mesh-group mask when the proxy exposed all groups before the swap");
    Expect(
        ProbeResolveKnifeSyntheticProxyDesiredMeshGroupMaskForTests(
            false,
            0,
            true,
            6,
            true,
            1) == 6,
        "guarded synthetic proxy rebinds should reuse the live proxy raw mesh-group mask before falling back to the world mesh-group mask");
    Expect(
        ProbeResolveKnifeSyntheticProxyDesiredMeshGroupMaskForTests(
            false,
            0,
            false,
            0,
            true,
            1) == 1,
        "guarded synthetic proxy rebinds should fall back to the readable world mesh-group mask when no proxy raw mask is available");
    Expect(
        ProbeResolveKnifeSyntheticProxyDesiredMeshGroupMaskForTests(
            false,
            0,
            false,
            0,
            false,
            0) == 1,
        "guarded synthetic proxy rebinds should default to mesh-group mask 1 only after both proxy raw and world masks are unavailable");
    Expect(
        ProbeShouldBlockUnsafeKnifeSyntheticRenderProxyCarrierWriteForTests(
            false,
            true,
            -428373316,
            0x50057B57000ull,
            0x50057B583C8ull),
        "providee-owner guarded synthetic paths should stop at the unsafe carrier-write block instead of mirroring world appearance scalars into the separate first-person render target");
    Expect(
        ProbeShouldBlockUnsafeKnifeSyntheticRenderProxyCarrierWriteForTests(
            true,
            false,
            -428373316,
            0x50057B57000ull,
            0x50057B583C8ull),
        "render-only synthetic first-person targets should also stay blocked from carrier and item-view writes");
    Expect(
        !ProbeShouldBlockUnsafeKnifeSyntheticRenderProxyCarrierWriteForTests(
            false,
            false,
            -428373316,
            0x50057B57000ull,
            0x50057B583C8ull),
        "synthetic carrier-write blocking should stay off when the path is neither render-only nor owner-write-skipped");
    Expect(
        !ProbeShouldBlockUnsafeKnifeSyntheticRenderProxyCarrierWriteForTests(
            false,
            true,
            511,
            0x50057B57000ull,
            0x50057B583C8ull),
        "non-synthetic separate first-person render targets should not trip the synthetic carrier-write block");
    Expect(
        !ProbeShouldBlockUnsafeKnifeSyntheticRenderProxyCarrierWriteForTests(
            false,
            true,
            -428373316,
            0x50057B57000ull,
            0ull),
        "synthetic carrier-write blocking should stay off when the separate render target has no readable item-view pointer");
    Expect(
        ProbeAllowKnifeVisualOnlyFallbackForTests(
            false,
            true,
            true,
            false,
            true,
            true),
        "providee/synthetic knife traces should keep a visual-only fallback when a separate first-person render target survives but the owner is unsafe for animation");
    Expect(
        !ProbeAllowKnifeVisualOnlyFallbackForTests(
            true,
            true,
            true,
            false,
            true,
            true),
        "visual-only fallback should stay off once a safe verified animation owner exists");
    Expect(
        !ProbeAllowKnifeVisualOnlyFallbackForTests(
            false,
            false,
            true,
            false,
            true,
            true),
        "visual-only fallback still requires a readable first-person render target");
    Expect(
        ProbeAllowKnifeVisualOnlyFallbackForTests(
            false,
            true,
            false,
            true,
            false,
            false),
        "render-only synthetic knife shells should stay eligible even when the shell and render target collapse to the same synthetic entity");

    KnifeModelPathBindingMatchProbeInput input;
    input.state_active = true;
    input.state_direct_render_rebind_attempted = true;
    input.state_direct_render_rebind_succeeded = true;
    input.state_xuid = 76561198086901534ull;
    input.state_world_entity_index = 215;
    input.state_world_render_entity_index = 215;
    input.state_first_person_entity_index = 511;
    input.state_first_person_render_entity_index = -985183000;
    input.state_first_person_render_entity_ptr = 0x246DF1C3400ull;
    input.state_first_person_render_item_view_ptr = 0x246DF1C47C8ull;
    input.state_first_person_render_model_state = 0x246DE8FF9E0ull;
    input.state_target_definition_index = 507;
    input.state_owner_kind = KnifeFirstPersonOwnerKind::ViewModel;
    input.binding_xuid = 76561198086901534ull;
    input.binding_world_entity_index = 215;
    input.binding_world_render_entity_index = 215;
    input.binding_first_person_entity_index = 511;
    input.binding_first_person_render_entity_index = -967550212;
    input.binding_first_person_render_entity_ptr = 0x246DF1C3400ull;
    input.binding_first_person_render_item_view_ptr = 0x246DF1C47C8ull;
    input.binding_first_person_render_model_state = 0x246DE8FF9E0ull;
    input.binding_first_person_owner_weapon_like = true;
    input.binding_target_definition_index = 507;
    input.binding_owner_kind = KnifeFirstPersonOwnerKind::ViewModel;
    Expect(
        !ProbeAllowKnifeModelOnlyAnimationHookForTests(input, true),
        "deploy/inspect remap must stay blocked until strict knife verification is recorded");
    Expect(
        !ProbeAllowKnifeModelOnlyAnimationHookForTests(input, false),
        "the synthetic animation-remap experiment should stay blocked until the HUD-alias experiment is enabled");

    input.state_strict_verified = true;
    Expect(
        !ProbeAllowKnifeModelOnlyAnimationHookForTests(input, true),
        "strict render verification alone must not unlock deploy/inspect before the animation owner is verified");
    input.state_animation_owner_verified = true;
    Expect(
        !ProbeAllowKnifeModelOnlyAnimationHookForTests(input, true),
        "animation-owner verification alone must not unlock deploy/inspect before the real viewmodel path is marked verified");
    input.state_real_viewmodel_verified = true;
    Expect(
        !ProbeAllowKnifeModelOnlyAnimationHookForTests(input, true),
        "deploy/inspect remap must stay blocked until the verified target definition matches the swap");
    input.state_verified_target_definition_index = 507;
    input.state_verified_viewmodel_carrier_entity_index = 511;
    input.state_verified_viewmodel_render_entity_index = -967550212;
    Expect(
        ProbeAllowKnifeModelOnlyAnimationHookForTests(input, true),
        "verified synthetic knife render paths may permit deploy/inspect only after strict verification matches the safe owner and target definition");

    input.binding_first_person_render_entity_ptr = 0x246DF1C3408ull;
    input.binding_first_person_render_model_state = 0x246DE8FF9E8ull;
    Expect(
        !ProbeAllowKnifeModelOnlyAnimationHookForTests(input, true),
        "render target drift must clear the verified deploy/inspect-ready state");
    input.binding_first_person_render_entity_ptr = 0x246DF1C3400ull;
    input.binding_first_person_render_model_state = 0x246DE8FF9E0ull;

    input.binding_owner_kind = KnifeFirstPersonOwnerKind::Providee;
    Expect(
        !ProbeAllowKnifeModelOnlyAnimationHookForTests(input, true),
        "animation-owner drift must clear the verified deploy/inspect-ready state");
    input.binding_owner_kind = KnifeFirstPersonOwnerKind::ViewModel;

    input.binding_target_definition_index = 515;
    Expect(
        !ProbeAllowKnifeModelOnlyAnimationHookForTests(input, true),
        "target-definition drift must clear the verified deploy/inspect-ready state");
    input.binding_target_definition_index = 507;

    input.state_owner_kind = KnifeFirstPersonOwnerKind::Providee;
    input.binding_owner_kind = KnifeFirstPersonOwnerKind::Providee;
    input.state_first_person_entity_index = 241;
    input.state_first_person_render_entity_index = input.binding_first_person_render_entity_index;
    input.binding_first_person_entity_index = 241;
    input.binding_first_person_owner_weapon_like = false;
    Expect(
        !ProbeAllowKnifeModelOnlyAnimationHookForTests(input, true),
        "synthetic providee owner paths must not enter the verified deploy/inspect state");
    input.state_synthetic_world_visual_isolation_active = true;
    Expect(
        !ProbeAllowKnifeModelOnlyAnimationHookForTests(input, true),
        "synthetic providee owner paths must stay blocked even after guarded world isolation");
    Expect(
        !ProbeAllowKnifeSyntheticWorldVisualIsolationForTests(input, true),
        "aggressive providee/synthetic fallback should stop preferring world isolation once we choose to show the target knife model");
    Expect(
        !ProbeAllowGuardedKnifeSyntheticWorldRenderRebindForTests(input, true),
        "providee/synthetic diagnostics must not become a formal guarded world-render rebind path");
    Expect(
        !ProbeShouldRollbackGuardedKnifeSyntheticFirstPersonRebindForTests(false, true),
        "guarded synthetic first-person-only paths should not roll back the proxy when world render was intentionally skipped");
    Expect(
        ProbeShouldRollbackGuardedKnifeSyntheticFirstPersonRebindForTests(false, false),
        "guarded synthetic atomic paths should still roll back when world render really failed");

    input.binding_first_person_entity_index = -967550212;
    Expect(
        !ProbeAllowKnifeModelOnlyAnimationHookForTests(input, true),
        "model-only animation remap should stay blocked when the only first-person owner candidate is synthetic");

    input.binding_owner_kind = KnifeFirstPersonOwnerKind::ViewModel;
    input.binding_first_person_entity_index = 511;
    input.binding_first_person_owner_weapon_like = true;
    Expect(
        ProbeAllowKnifeSyntheticWorldVisualIsolationForTests(input, true),
        "model-only guarded synthetic render proxies may keep world isolation when a safe weapon-like viewmodel owner survives behind them");
    Expect(
        !ProbeAllowGuardedKnifeSyntheticWorldRenderRebindForTests(input, true),
        "safe weapon-like guarded synthetic paths should prefer isolation over world render rebind when isolation is already available");
}

void TestKnifeCarrierDiscoveryReadOnlyDiscovery()
{
    using nskinzcs2::runtime::ClientClassDescriptorProbeResult;
    using nskinzcs2::runtime::KnifeCarrierPromotionProbeInput;
    using nskinzcs2::runtime::KnifeCarrierDiscoveryDecisionProbeInput;
    using nskinzcs2::runtime::KnifeCarrierDiscoveryResult;
    using nskinzcs2::runtime::KnifeCarrierFactoryFeasibility;
    using nskinzcs2::runtime::KnifeViewmodelAbsenceAuditResult;
    using nskinzcs2::runtime::ProbeBuildKnifeCarrierRuntimeReasonTextForTests;
    using nskinzcs2::runtime::ProbeClassifyKnifeViewmodelAbsenceAuditForTests;
    using nskinzcs2::runtime::ProbeClientClassDescriptorClassificationForTests;
    using nskinzcs2::runtime::ProbeKnifeCarrierDiscoveryDecisionForTests;
    using nskinzcs2::runtime::ProbeResolveKnifeCarrierPromotionForTests;

    {
        KnifeCarrierDiscoveryDecisionProbeInput input;
        input.complete_candidate_found = true;
        const auto result = ProbeKnifeCarrierDiscoveryDecisionForTests(input);
        Expect(
            result.carrier_result == KnifeCarrierDiscoveryResult::RealCarrierFound,
            "complete discovery candidates should resolve to a real carrier");
        Expect(
            result.factory_feasibility == KnifeCarrierFactoryFeasibility::Unknown,
            "real carriers should not force a factory-feasibility conclusion");
        Expect(
            !result.no_exposed_non_synthetic_render_owner,
            "real carriers should clear the no-exposed-owner flag");
    }

    {
        KnifeCarrierDiscoveryDecisionProbeInput input;
        input.related_incomplete_candidate_found = true;
        const auto result = ProbeKnifeCarrierDiscoveryDecisionForTests(input);
        Expect(
            result.carrier_result == KnifeCarrierDiscoveryResult::DiscoverableIncompleteCarrier,
            "related incomplete candidates should stay in the incomplete discovery bucket");
        Expect(
            result.factory_feasibility == KnifeCarrierFactoryFeasibility::NoViewmodelFactoryEvidence,
            "incomplete discovery without class evidence should report no factory evidence");
        Expect(
            result.no_exposed_non_synthetic_render_owner,
            "incomplete discovery should still conclude that no complete non-synthetic owner is exposed");
    }

    {
        KnifeCarrierDiscoveryDecisionProbeInput input;
        input.factory_observed = true;
        const auto result = ProbeKnifeCarrierDiscoveryDecisionForTests(input);
        Expect(
            result.carrier_result == KnifeCarrierDiscoveryResult::NoDiscoverableCompleteCarrier,
            "missing complete candidates should stay in the no-discoverable-complete-carrier bucket");
        Expect(
            result.factory_feasibility == KnifeCarrierFactoryFeasibility::ViewmodelFactoryPresentWithoutInstance,
            "trusted viewmodel class discovery without a live instance should report factory presence without an instance");
        Expect(
            result.no_exposed_non_synthetic_render_owner,
            "missing complete candidates should keep the no-exposed-owner flag");
    }

    {
        KnifeCarrierPromotionProbeInput input;
        input.safe_linked_item_view = true;
        input.shell_relation_to_render_object = true;
        const auto result = ProbeResolveKnifeCarrierPromotionForTests(input);
        Expect(
            result.promotable_relation &&
                result.discovery_relevant &&
                result.accept_writable_carrier,
            "shell-backed render-object matches should promote safe writable carriers even without complete demo item fields");
    }

    {
        KnifeCarrierPromotionProbeInput input;
        input.safe_linked_item_view = true;
        input.world_neighborhood_match = true;
        const auto result = ProbeResolveKnifeCarrierPromotionForTests(input);
        Expect(
            !result.promotable_relation &&
                result.discovery_relevant &&
                !result.accept_writable_carrier,
            "world-neighborhood-only candidates should stay diagnostic until a promotable shell or render relation appears");
    }

    {
        KnifeCarrierPromotionProbeInput input;
        input.safe_linked_item_view = true;
        input.shell_owner_chain_matches_player = true;
        input.world_owned_proxy = true;
        const auto result = ProbeResolveKnifeCarrierPromotionForTests(input);
        Expect(
            result.promotable_relation &&
                result.discovery_relevant &&
                !result.accept_writable_carrier,
            "world-owned proxies must stay blocked even when their shells look like writable viewmodel carriers");
    }

    {
        const auto wearable = ProbeClientClassDescriptorClassificationForTests("CEconWearable", true);
        Expect(!wearable.viewmodel_like, "CEconWearable should not be classified as viewmodel-like");
        Expect(!wearable.weapon_like, "CEconWearable should not be classified as weapon-like");
        Expect(wearable.econ_like, "CEconWearable should be classified as econ-like");
        Expect(!wearable.factory_observed, "wearable-only classes should not be treated as viewmodel factory evidence");

        const auto viewmodel = ProbeClientClassDescriptorClassificationForTests("C_CSGOViewModel", true);
        Expect(viewmodel.viewmodel_like, "C_CSGOViewModel should be classified as viewmodel-like");
        Expect(viewmodel.weapon_like, "C_CSGOViewModel should also be classified as weapon-like");
        Expect(!viewmodel.econ_like, "C_CSGOViewModel should not be classified as econ-like");
        Expect(viewmodel.factory_observed, "trusted viewmodel classes should count as factory evidence");

        const auto weapon = ProbeClientClassDescriptorClassificationForTests("C_CSWeaponBase", true);
        Expect(!weapon.viewmodel_like, "weapon classes should stay distinct from viewmodel classes");
        Expect(weapon.weapon_like, "weapon classes should be classified as weapon-like");
        Expect(!weapon.econ_like, "weapon classes should not be classified as econ-like");
        Expect(weapon.factory_observed, "trusted weapon classes should count as factory evidence");
    }

    Expect(
        ProbeClassifyKnifeViewmodelAbsenceAuditForTests(
            false,
            0,
            false,
            false,
            false,
            false,
            false,
            0) == KnifeViewmodelAbsenceAuditResult::MissingHandle,
        "absent viewmodel handles should classify as missing handles");
    Expect(
        ProbeClassifyKnifeViewmodelAbsenceAuditForTests(
            true,
            0x1234u,
            false,
            false,
            false,
            false,
            false,
            0) == KnifeViewmodelAbsenceAuditResult::StaleHandle,
        "non-visible handles should classify as stale handles");
    Expect(
        ProbeClassifyKnifeViewmodelAbsenceAuditForTests(
            true,
            0x1234u,
            true,
            true,
            true,
            false,
            false,
            0) == KnifeViewmodelAbsenceAuditResult::SyntheticAliasOnly,
        "shared synthetic aliases should classify as synthetic-alias-only");
    Expect(
        ProbeClassifyKnifeViewmodelAbsenceAuditForTests(
            true,
            0x1234u,
            true,
            false,
            false,
            true,
            false,
            1) == KnifeViewmodelAbsenceAuditResult::ShellWithoutWritableCarrier,
        "visible viewmodel shells without a writable carrier should classify separately from no-viewmodel cases");
    Expect(
        ProbeClassifyKnifeViewmodelAbsenceAuditForTests(
            true,
            0x1234u,
            true,
            false,
            false,
            false,
            false,
            0) == KnifeViewmodelAbsenceAuditResult::NonQualifyingCandidate,
        "visible handles with no shell evidence should stay non-qualifying");

    {
        const auto reason = ProbeBuildKnifeCarrierRuntimeReasonTextForTests(
            true,
            KnifeCarrierDiscoveryResult::DiscoverableIncompleteCarrier,
            KnifeCarrierFactoryFeasibility::NoViewmodelFactoryEvidence);
        Expect(
            reason.find(L"writable real viewmodel carrier") != std::wstring::npos,
            "incomplete carrier reason text should describe missing writable real viewmodel carriers");
    }

    {
        const auto reason = ProbeBuildKnifeCarrierRuntimeReasonTextForTests(
            true,
            KnifeCarrierDiscoveryResult::NoDiscoverableCompleteCarrier,
            KnifeCarrierFactoryFeasibility::ViewmodelFactoryPresentWithoutInstance);
        Expect(
            reason.find(L"without a live instance") != std::wstring::npos,
            "factory-present reason text should mention the missing live instance");
    }
}

void TestGloveAppearanceModule()
{
    using nskinzcs2::runtime::BuildGloveAppearanceRequest;
    using nskinzcs2::runtime::BuildSyntheticGloveEconItemIdentity;
    using nskinzcs2::runtime::GloveCarrierKind;
    using nskinzcs2::runtime::EvaluateGloveCompatibility;
    using nskinzcs2::runtime::EvaluateGloveRenderVerification;
    using nskinzcs2::runtime::GloveCompatibilityReason;
    using nskinzcs2::runtime::GloveProfileRequiresModelDefinition;
    using nskinzcs2::runtime::GloveRenderVerificationReason;
    using nskinzcs2::runtime::GloveSlotMode;
    using nskinzcs2::runtime::GloveWearableRetryDelayMillis;
    using nskinzcs2::runtime::IsStockTeamGloveRestoreRequest;
    using nskinzcs2::runtime::ResolveCanonicalGloveCarrierKind;
    using nskinzcs2::runtime::ResolveDefaultGloveDefinitionForTeam;
    using nskinzcs2::runtime::ResolveGloveSlotMode;
    using nskinzcs2::runtime::SelectReusableWearableHandleSlot;

    {
        constexpr std::uint64_t xuid = 76561199674674473ull;
        const auto specialist = BuildSyntheticGloveEconItemIdentity(xuid, 5034);
        const auto specialist_again = BuildSyntheticGloveEconItemIdentity(xuid, 5034);
        const auto sport = BuildSyntheticGloveEconItemIdentity(xuid, 5030);
        const auto other_player = BuildSyntheticGloveEconItemIdentity(xuid + 1, 5034);
        Expect(specialist.IsValid(), "synthetic glove identities must avoid invalid econ sentinels");
        Expect(
            specialist.high == (0xC5210000u | 5034u),
            "synthetic glove high word should encode the observable namespace and definition");
        Expect(
            specialist.high == specialist_again.high && specialist.low == specialist_again.low,
            "synthetic glove identities must be stable for one player and definition");
        Expect(
            specialist.high != sport.high && specialist.low != sport.low,
            "different glove definitions should not share a synthetic identity");
        Expect(
            specialist.low != other_player.low,
            "different players should not share the synthetic glove low word");
    }

    Expect(
        ResolveDefaultGloveDefinitionForTeam(2) == 5028,
        "T-side vanilla gloves should resolve to the stock Terrorist definition");
    Expect(
        ResolveDefaultGloveDefinitionForTeam(3) == 5029,
        "CT-side vanilla gloves should resolve to the stock Counter-Terrorist definition");
    Expect(
        ResolveDefaultGloveDefinitionForTeam(0) == 0,
        "unknown teams must not guess a stock glove definition");

    {
        nskinzcs2::runtime::GloveAppearanceRequest request;
        request.mode = GloveSlotMode::ModelAndFinish;
        request.model_definition = 5028;
        request.paint_kit = 0;
        request.explicit_vanilla_finish = true;
        Expect(
            IsStockTeamGloveRestoreRequest(request, 2),
            "a vanilla T request must use the Pawn-native stock glove path");
        Expect(
            !IsStockTeamGloveRestoreRequest(request, 3),
            "a T stock definition must not be accepted for a CT Pawn");
        request.paint_kit = 10006;
        Expect(
            !IsStockTeamGloveRestoreRequest(request, 2),
            "a painted glove is not a stock restore even though both use the Pawn-native visual path");
        request.paint_kit = 0;
        request.explicit_vanilla_finish = false;
        Expect(
            !IsStockTeamGloveRestoreRequest(request, 2),
            "an unselected finish must not be mistaken for an explicit stock restore");
    }

    {
        nskinzcs2::model::GloveProfile profile;
        profile.mode = nskinzcs2::model::GloveProfileMode::FinishOnly;

        Expect(
            ResolveGloveSlotMode(profile) == GloveSlotMode::FinishOnly,
            "finish-only glove profiles should resolve to the finish-only slot mode");
        Expect(
            !GloveProfileRequiresModelDefinition(ResolveGloveSlotMode(profile)),
            "finish-only glove profiles should not require a model definition");
    }

    {
        nskinzcs2::model::GloveProfile profile;
        profile.xuid = 76561198000000000ull;
        profile.mode = nskinzcs2::model::GloveProfileMode::ModelAndFinish;
        profile.model_definition = 5030;
        profile.paint_kit = 10018;
        profile.explicit_vanilla_finish = true;
        profile.quality = 3;
        profile.seed = 7;
        profile.wear = 0.15f;

        const auto request = BuildGloveAppearanceRequest(profile);
        Expect(request.xuid == profile.xuid, "glove request should preserve XUID");
        Expect(request.mode == GloveSlotMode::ModelAndFinish, "glove request should preserve mode");
        Expect(request.model_definition == 5030, "glove request should preserve model definition");
        Expect(request.paint_kit == 10018, "glove request should preserve paint");
        Expect(request.explicit_vanilla_finish, "glove request should preserve explicit finish intent");
        Expect(GloveProfileRequiresModelDefinition(request.mode), "model-and-finish mode requires a model definition");
    }

    {
        nskinzcs2::model::GloveProfile profile;
        profile.mode = nskinzcs2::model::GloveProfileMode::ModelAndFinish;
        profile.model_definition = 5032;
        profile.paint_kit = 10009;
        profile.wear = 0.0f;

        const auto request = BuildGloveAppearanceRequest(profile);
        Expect(
            nskinzcs2::model::CosmeticWearMatchesExactly(
                request.wear,
                nskinzcs2::model::kMinimumCosmeticWear),
            "glove requests below the supported wear floor should use exact minimum wear");
    }

    Expect(
        ResolveCanonicalGloveCarrierKind(GloveSlotMode::FinishOnly, true, false) == GloveCarrierKind::None,
        "finish-only should not accept wearable-only resolution");
    Expect(
        ResolveCanonicalGloveCarrierKind(GloveSlotMode::FinishOnly, true, true) == GloveCarrierKind::PawnEconGloves,
        "finish-only should always resolve to pawn_econ_gloves when available");
    Expect(
        ResolveCanonicalGloveCarrierKind(GloveSlotMode::ModelAndFinish, true, true) == GloveCarrierKind::PawnEconGloves,
        "model-and-finish should prefer the Pawn input consumed by the native visual updater");
    Expect(
        ResolveCanonicalGloveCarrierKind(GloveSlotMode::ModelAndFinish, false, true) ==
            GloveCarrierKind::PawnEconGloves,
        "model-and-finish should not require a duplicate wearable when Pawn econ-gloves is available");
    Expect(
        ResolveCanonicalGloveCarrierKind(GloveSlotMode::ModelAndFinish, true, false) ==
            GloveCarrierKind::Wearable,
        "model-and-finish should retain the wearable compatibility fallback when pawn econ-gloves is unavailable");

    using nskinzcs2::runtime::CanUsePawnEconGlovesCarrier;
    Expect(
        CanUsePawnEconGlovesCarrier(GloveSlotMode::ModelAndFinish, true, 0, false, true),
        "an empty demo Pawn glove slot should accept a known glove definition for native materialisation");
    Expect(
        !CanUsePawnEconGlovesCarrier(GloveSlotMode::ModelAndFinish, true, 0, false, false),
        "an empty pawn econ-gloves slot must reject an unknown target definition");
    Expect(
        !CanUsePawnEconGlovesCarrier(GloveSlotMode::FinishOnly, true, 0, false, false),
        "finish-only must not invent a glove model for an empty pawn econ-gloves slot");
    Expect(
        CanUsePawnEconGlovesCarrier(GloveSlotMode::FinishOnly, true, 5030, true, false),
        "finish-only should keep using a known live pawn glove definition");
    Expect(
        CanUsePawnEconGlovesCarrier(GloveSlotMode::ModelAndFinish, true, 5030, true, true),
        "model-and-finish should replace an existing Pawn glove definition through the native path");
    Expect(
        !CanUsePawnEconGlovesCarrier(GloveSlotMode::ModelAndFinish, true, 7, false, true),
        "model-and-finish must reject a positive non-glove embedded item definition");
    Expect(
        !CanUsePawnEconGlovesCarrier(GloveSlotMode::ModelAndFinish, false, 0, false, true),
        "an unreadable pawn econ-gloves slot must fail closed");

    using nskinzcs2::runtime::BuildGloveWearableCreationEpoch;
    const auto creation_epoch = BuildGloveWearableCreationEpoch(7, 0x12348001u, 0x100000ull);
    Expect(
        creation_epoch == BuildGloveWearableCreationEpoch(7, 0x12348001u, 0x100000ull),
        "the same provider and Pawn epoch should retain duplicate-creation protection");
    Expect(
        creation_epoch != BuildGloveWearableCreationEpoch(7, 0x12350001u, 0x100000ull) &&
            creation_epoch != BuildGloveWearableCreationEpoch(7, 0x12348001u, 0x200000ull) &&
            creation_epoch != BuildGloveWearableCreationEpoch(8, 0x12348001u, 0x100000ull),
        "a rebuilt Pawn or provider must receive a fresh wearable creation epoch");
    Expect(
        GloveWearableRetryDelayMillis(0) == 0 &&
            GloveWearableRetryDelayMillis(1) == 125 &&
            GloveWearableRetryDelayMillis(2) == 250 &&
            GloveWearableRetryDelayMillis(3) == 500 &&
            GloveWearableRetryDelayMillis(4) == 1000 &&
            GloveWearableRetryDelayMillis(5) == 2000 &&
            GloveWearableRetryDelayMillis(100) == 2000,
        "transient wearable creation retries must use a bounded exponential delay");

    {
        nskinzcs2::runtime::GloveCompatibilityInputs inputs;
        inputs.mode = GloveSlotMode::ModelAndFinish;
        inputs.carrier_kind = GloveCarrierKind::PawnEconGloves;
        inputs.current_definition_index = 5028;
        inputs.requested_model_definition = 5030;
        inputs.compatible_paint_definition = 5030;
        inputs.carrier_valid = true;
        inputs.target_model_known = true;
        const auto compatibility = EvaluateGloveCompatibility(inputs);
        Expect(
            compatibility.compatible,
            "model-and-finish should accept the current pawn econ-gloves reapply path");
    }

    {
        nskinzcs2::runtime::GloveCompatibilityInputs inputs;
        inputs.mode = GloveSlotMode::ModelAndFinish;
        inputs.carrier_kind = GloveCarrierKind::PawnEconGloves;
        inputs.current_definition_index = 5029;
        inputs.requested_model_definition = 5032;
        inputs.compatible_paint_definition = 5032;
        inputs.carrier_valid = true;
        inputs.target_model_known = true;
        const auto compatibility = EvaluateGloveCompatibility(inputs);
        Expect(
            compatibility.compatible,
            "model-and-finish should accept the default Counter-Terrorist pawn glove definition");
    }

    {
        nskinzcs2::runtime::GloveCompatibilityInputs inputs;
        inputs.mode = GloveSlotMode::FinishOnly;
        inputs.carrier_kind = GloveCarrierKind::PawnEconGloves;
        inputs.current_definition_index = 5030;
        inputs.compatible_paint_definition = 5030;
        inputs.carrier_valid = true;
        const auto compatibility = EvaluateGloveCompatibility(inputs);
        Expect(compatibility.compatible, "finish-only should accept a compatible live glove definition");
    }

    {
        nskinzcs2::runtime::GloveCompatibilityInputs inputs;
        inputs.mode = GloveSlotMode::FinishOnly;
        inputs.carrier_kind = GloveCarrierKind::PawnEconGloves;
        inputs.current_definition_index = 5030;
        inputs.compatible_paint_definition = 5027;
        inputs.carrier_valid = true;
        const auto compatibility = EvaluateGloveCompatibility(inputs);
        Expect(!compatibility.compatible, "finish-only should reject incompatible paint-to-definition pairs");
        Expect(
            compatibility.reason == GloveCompatibilityReason::PaintIncompatible,
            "finish-only incompatibility should report a paint mismatch");
    }

    {
        nskinzcs2::runtime::GloveCompatibilityInputs inputs;
        inputs.mode = GloveSlotMode::ModelAndFinish;
        inputs.carrier_kind = GloveCarrierKind::Wearable;
        inputs.current_definition_index = 5030;
        inputs.compatible_paint_definition = 5030;
        inputs.carrier_valid = true;
        inputs.render_binding_valid = true;
        inputs.live_glove_model_state_resolved = true;
        inputs.first_person_arms_model_state_resolved = true;
        inputs.live_glove_model_handle_resolved = true;
        inputs.first_person_arms_model_handle_resolved = true;
        inputs.live_glove_material_group_resolved = true;
        inputs.first_person_arms_material_group_resolved = true;
        inputs.live_glove_skeleton_identity_resolved = true;
        inputs.first_person_arms_skeleton_identity_resolved = true;
        inputs.target_view_skeleton_identity_resolved = true;
        inputs.target_model_known = true;
        const auto compatibility = EvaluateGloveCompatibility(inputs);
        Expect(!compatibility.compatible, "model-and-finish should require a model definition");
        Expect(
            compatibility.reason == GloveCompatibilityReason::MissingModelDefinition,
            "missing model definition should be reported");
    }

    {
        nskinzcs2::runtime::GloveCompatibilityInputs inputs;
        inputs.mode = GloveSlotMode::ModelAndFinish;
        inputs.carrier_kind = GloveCarrierKind::Wearable;
        inputs.current_definition_index = 5030;
        inputs.requested_model_definition = 5030;
        inputs.compatible_paint_definition = 5030;
        inputs.carrier_valid = true;
        inputs.render_binding_valid = true;
        inputs.live_glove_model_state_resolved = true;
        inputs.first_person_arms_model_state_resolved = true;
        inputs.live_glove_model_handle_resolved = true;
        inputs.first_person_arms_model_handle_resolved = true;
        inputs.live_glove_material_group_resolved = true;
        inputs.first_person_arms_material_group_resolved = true;
        inputs.live_glove_skeleton_identity_resolved = true;
        inputs.first_person_arms_skeleton_identity_resolved = true;
        inputs.target_view_skeleton_identity_resolved = true;
        inputs.target_view_matches_first_person_skeleton = false;
        inputs.target_model_known = true;
        const auto compatibility = EvaluateGloveCompatibility(inputs);
        Expect(!compatibility.compatible, "model-and-finish should reject skeleton mismatches");
        Expect(
            compatibility.reason == GloveCompatibilityReason::SkeletonMismatch,
            "strict same-skeleton mismatches should be reported");
    }

    {
        nskinzcs2::runtime::GloveCompatibilityInputs inputs;
        inputs.mode = GloveSlotMode::ModelAndFinish;
        inputs.carrier_kind = GloveCarrierKind::Wearable;
        inputs.current_definition_index = 5030;
        inputs.requested_model_definition = 5030;
        inputs.compatible_paint_definition = 5030;
        inputs.carrier_valid = true;
        inputs.render_binding_valid = true;
        inputs.live_glove_model_state_resolved = true;
        inputs.first_person_arms_model_state_resolved = true;
        inputs.live_glove_model_handle_resolved = true;
        inputs.first_person_arms_model_handle_resolved = true;
        inputs.live_glove_material_group_resolved = true;
        inputs.first_person_arms_material_group_resolved = true;
        inputs.live_glove_skeleton_identity_resolved = true;
        inputs.first_person_arms_skeleton_identity_resolved = true;
        inputs.target_view_skeleton_identity_resolved = true;
        inputs.target_view_matches_first_person_skeleton = true;
        inputs.target_model_known = true;
        const auto compatibility = EvaluateGloveCompatibility(inputs);
        Expect(compatibility.compatible, "model-and-finish should accept a fully resolved glove binding");
    }

    {
        nskinzcs2::runtime::GloveCompatibilityInputs inputs;
        inputs.mode = GloveSlotMode::ModelAndFinish;
        inputs.carrier_kind = GloveCarrierKind::Wearable;
        inputs.current_definition_index = 5030;
        inputs.requested_model_definition = 5030;
        inputs.compatible_paint_definition = 5030;
        inputs.carrier_valid = true;
        inputs.render_binding_valid = false;
        inputs.live_glove_model_state_resolved = false;
        inputs.live_glove_model_handle_resolved = false;
        inputs.live_glove_material_group_resolved = false;
        inputs.live_glove_skeleton_identity_resolved = false;
        inputs.first_person_arms_model_state_resolved = true;
        inputs.first_person_arms_model_handle_resolved = true;
        inputs.first_person_arms_skeleton_identity_resolved = true;
        inputs.target_view_skeleton_identity_resolved = true;
        inputs.target_view_matches_first_person_skeleton = true;
        inputs.target_model_known = true;
        const auto compatibility = EvaluateGloveCompatibility(inputs);
        Expect(
            !compatibility.compatible &&
                compatibility.reason == GloveCompatibilityReason::MissingRenderBinding,
            "model-and-finish should require a real wearable render binding before apply");
    }

    {
        nskinzcs2::runtime::GloveCompatibilityInputs inputs;
        inputs.mode = GloveSlotMode::ModelAndFinish;
        inputs.carrier_kind = GloveCarrierKind::Wearable;
        inputs.current_definition_index = 5030;
        inputs.requested_model_definition = 5030;
        inputs.compatible_paint_definition = 5030;
        inputs.carrier_valid = true;
        inputs.render_binding_valid = false;
        inputs.allow_render_binding_after_apply = true;
        inputs.live_glove_model_state_resolved = false;
        inputs.live_glove_model_handle_resolved = false;
        inputs.live_glove_skeleton_identity_resolved = false;
        inputs.first_person_arms_model_state_resolved = false;
        inputs.first_person_arms_model_handle_resolved = false;
        inputs.first_person_arms_skeleton_identity_resolved = false;
        inputs.target_view_skeleton_identity_resolved = false;
        inputs.target_view_matches_first_person_skeleton = false;
        inputs.target_model_known = true;
        const auto compatibility = EvaluateGloveCompatibility(inputs);
        Expect(
            compatibility.compatible,
            "runtime-created real wearables should establish render and model identity after apply");
    }

    {
        nskinzcs2::runtime::GloveCompatibilityInputs inputs;
        inputs.mode = GloveSlotMode::ModelAndFinish;
        inputs.carrier_kind = GloveCarrierKind::Wearable;
        inputs.current_definition_index = 5028;
        inputs.requested_model_definition = 5032;
        inputs.compatible_paint_definition = 5032;
        inputs.carrier_valid = true;
        inputs.render_binding_valid = false;
        inputs.allow_render_binding_after_apply = true;
        inputs.target_model_known = false;
        const auto compatibility = EvaluateGloveCompatibility(inputs);
        Expect(
            !compatibility.compatible &&
                compatibility.reason == GloveCompatibilityReason::MissingModelIdentity,
            "runtime-created wearables must reject deferred model identity when the target model is unknown");
    }

    {
        const std::array<std::uint32_t, 2> current_handles {0u, 0xC47A0000u};
        Expect(
            SelectReusableWearableHandleSlot(current_handles.data(), current_handles.size(), 0x8200u) == 0,
            "wearable binding should reuse an existing entity-index-zero slot instead of appending past capacity");
    }

    {
        const std::array<std::uint32_t, 2> current_handles {0x8101u, 0x8202u};
        Expect(
            SelectReusableWearableHandleSlot(current_handles.data(), current_handles.size(), 0x8303u) == -1,
            "wearable binding must fail closed when every existing slot is occupied");
        Expect(
            SelectReusableWearableHandleSlot(nullptr, 0, 0x8303u) == -1,
            "wearable binding must not invent an append slot for an empty vector");
    }

    {
        using nskinzcs2::runtime::ShouldInitializeWearableModel;
        Expect(
            ShouldInitializeWearableModel(
                GloveSlotMode::ModelAndFinish,
                true,
                512,
                true,
                0,
                true),
            "a real CEconWearable with an empty model handle should use the formal skeleton model binder");
        Expect(
            !ShouldInitializeWearableModel(
                GloveSlotMode::ModelAndFinish,
                false,
                -2,
                true,
                0,
                true),
            "synthetic/Pawn render objects must never enter the formal wearable model path");
        Expect(
            !ShouldInitializeWearableModel(
                GloveSlotMode::ModelAndFinish,
                true,
                512,
                true,
                0x1234,
                false),
            "an already initialized Wearable must not be force-rebound by the empty-model path");
        Expect(
            !ShouldInitializeWearableModel(
                GloveSlotMode::ModelAndFinish,
                true,
                512,
                true,
                0,
                false),
            "formal wearable binding must fail closed when its scene/model context is unverified");
    }

    {
        nskinzcs2::runtime::GloveCompatibilityInputs inputs;
        inputs.mode = GloveSlotMode::ModelAndFinish;
        inputs.carrier_kind = GloveCarrierKind::PawnEconGlovesRenderFallback;
        inputs.current_definition_index = 5030;
        inputs.requested_model_definition = 5030;
        inputs.compatible_paint_definition = 5030;
        inputs.carrier_valid = true;
        inputs.render_binding_valid = true;
        inputs.live_glove_model_state_resolved = true;
        inputs.first_person_arms_model_state_resolved = true;
        inputs.live_glove_model_handle_resolved = true;
        inputs.first_person_arms_model_handle_resolved = true;
        inputs.live_glove_material_group_resolved = true;
        inputs.first_person_arms_material_group_resolved = true;
        inputs.live_glove_skeleton_identity_resolved = true;
        inputs.first_person_arms_skeleton_identity_resolved = true;
        inputs.target_view_skeleton_identity_resolved = true;
        inputs.target_view_matches_first_person_skeleton = true;
        inputs.target_model_known = true;
        const auto compatibility = EvaluateGloveCompatibility(inputs);
        Expect(
            compatibility.compatible,
            "pawn glove model replacement should accept a known target for native output verification");
    }

    {
        nskinzcs2::runtime::GloveCompatibilityInputs inputs;
        inputs.mode = GloveSlotMode::ModelAndFinish;
        inputs.carrier_kind = GloveCarrierKind::PawnEconGlovesRenderFallback;
        inputs.current_definition_index = 5030;
        inputs.requested_model_definition = 5030;
        inputs.compatible_paint_definition = 5030;
        inputs.carrier_valid = true;
        inputs.render_binding_valid = true;
        inputs.guarded_first_person_model_rebind_verified = true;
        inputs.target_model_known = true;
        const auto compatibility = EvaluateGloveCompatibility(inputs);
        Expect(
            compatibility.compatible,
            "pawn glove model replacement should not depend on a manual first-person resource rebind");
    }

    {
        nskinzcs2::runtime::GloveCompatibilityInputs inputs;
        inputs.mode = GloveSlotMode::ModelAndFinish;
        inputs.carrier_kind = GloveCarrierKind::PawnEconGlovesRenderFallback;
        inputs.current_definition_index = 5030;
        inputs.requested_model_definition = 5030;
        inputs.compatible_paint_definition = 5030;
        inputs.carrier_valid = true;
        inputs.render_binding_valid = false;
        inputs.live_glove_model_state_resolved = true;
        inputs.first_person_arms_model_state_resolved = true;
        inputs.live_glove_model_handle_resolved = true;
        inputs.first_person_arms_model_handle_resolved = true;
        inputs.live_glove_material_group_resolved = true;
        inputs.first_person_arms_material_group_resolved = true;
        inputs.live_glove_skeleton_identity_resolved = true;
        inputs.first_person_arms_skeleton_identity_resolved = true;
        inputs.target_view_skeleton_identity_resolved = true;
        inputs.target_view_matches_first_person_skeleton = true;
        inputs.target_model_known = true;
        const auto compatibility = EvaluateGloveCompatibility(inputs);
        Expect(
            compatibility.compatible,
            "pawn glove native rebuild should establish its render output after apply");
    }

    {
        nskinzcs2::runtime::GloveCompatibilityInputs inputs;
        inputs.mode = GloveSlotMode::ModelAndFinish;
        inputs.carrier_kind = GloveCarrierKind::PawnEconGlovesRenderFallback;
        inputs.current_definition_index = 5030;
        inputs.requested_model_definition = 5030;
        inputs.compatible_paint_definition = 5030;
        inputs.carrier_valid = true;
        inputs.render_binding_valid = false;
        inputs.allow_render_binding_after_apply = true;
        inputs.target_model_known = true;
        const auto compatibility = EvaluateGloveCompatibility(inputs);
        Expect(
            compatibility.compatible,
            "pawn glove native rebuild should accept a known target without a pre-existing render binding");
    }

    {
        nskinzcs2::runtime::GloveCompatibilityInputs inputs;
        inputs.mode = GloveSlotMode::ModelAndFinish;
        inputs.carrier_kind = GloveCarrierKind::PawnEconGlovesRenderFallback;
        inputs.current_definition_index = 5030;
        inputs.requested_model_definition = 5030;
        inputs.compatible_paint_definition = 5030;
        inputs.carrier_valid = true;
        inputs.render_binding_valid = false;
        inputs.allow_render_binding_after_apply = true;
        inputs.target_model_known = false;
        const auto compatibility = EvaluateGloveCompatibility(inputs);
        Expect(
            !compatibility.compatible &&
                compatibility.reason == GloveCompatibilityReason::MissingModelIdentity,
            "pawn glove render fallback must reject an unknown target model");
    }

    {
        nskinzcs2::runtime::GloveRenderVerificationInputs inputs;
        inputs.mode = GloveSlotMode::FinishOnly;
        inputs.carrier_field_readback_matched = true;
        inputs.reapply_triggered = true;
        const auto verification = EvaluateGloveRenderVerification(inputs);
        Expect(
            verification.verified,
            "finish-only should verify from carrier readback plus reapply trigger");
    }

    {
        nskinzcs2::runtime::GloveRenderVerificationInputs inputs;
        inputs.mode = GloveSlotMode::ModelAndFinish;
        inputs.pawn_econ_gloves_reapply_path = true;
        inputs.carrier_field_readback_matched = true;
        inputs.reapply_triggered = true;
        const auto verification = EvaluateGloveRenderVerification(inputs);
        Expect(
            !verification.verified &&
                verification.reason == GloveRenderVerificationReason::NativeVisualRefreshPending,
            "model-and-finish pawn econ-gloves must wait for native world/HUD model readback");
    }

    {
        nskinzcs2::runtime::GloveRenderVerificationInputs inputs;
        inputs.mode = GloveSlotMode::ModelAndFinish;
        inputs.has_first_person_render = true;
        inputs.requested_model_changed = true;
        inputs.requested_finish_changed = true;
        inputs.live_render_state_resolved = true;
        inputs.first_person_render_state_resolved = true;
        inputs.live_model_binding_changed = true;
        inputs.first_person_model_binding_changed = true;
        inputs.finish_binding_confirmed = true;
        inputs.live_material_resource_matches_target = true;
        inputs.first_person_material_resource_matches_target = true;
        const auto verification = EvaluateGloveRenderVerification(inputs);
        Expect(verification.verified, "model-and-finish should accept a coherent render-side model change");
    }

    {
        nskinzcs2::runtime::GloveRenderVerificationInputs inputs;
        inputs.mode = GloveSlotMode::ModelAndFinish;
        inputs.has_first_person_render = true;
        inputs.requested_finish_changed = true;
        inputs.live_render_state_resolved = true;
        inputs.first_person_render_state_resolved = false;
        const auto verification = EvaluateGloveRenderVerification(inputs);
        Expect(
            !verification.verified &&
                verification.reason == GloveRenderVerificationReason::MissingHudRenderState,
            "model-and-finish should fail when first-person render state is missing");
    }

    {
        nskinzcs2::runtime::GloveRenderVerificationInputs inputs;
        inputs.mode = GloveSlotMode::FinishOnly;
        inputs.carrier_field_readback_matched = true;
        const auto verification = EvaluateGloveRenderVerification(inputs);
        Expect(
            !verification.verified &&
                verification.reason == GloveRenderVerificationReason::ReapplyNotTriggered,
            "finish-only should fail when glove reapply was not triggered");
    }

    {
        nskinzcs2::runtime::GloveRenderVerificationInputs inputs;
        inputs.mode = GloveSlotMode::FinishOnly;
        inputs.reapply_triggered = true;
        const auto verification = EvaluateGloveRenderVerification(inputs);
        Expect(
            !verification.verified &&
                verification.reason == GloveRenderVerificationReason::CarrierFieldMismatch,
            "finish-only should fail when carrier readback does not match the requested finish");
    }
}

void TestBuildWeaponRefreshAttributes()
{
    {
        nskinzcs2::model::CosmeticProfile profile;
        profile.base_item = 9;
        profile.paint_kit = 344;
        profile.seed = 123;
        profile.wear = 0.0000001f;

        const auto attributes = nskinzcs2::runtime::BuildWeaponRefreshAttributes(profile);
        Expect(attributes.size() == 3, "standard refresh should emit paint, seed and wear attributes");
        Expect(ContainsRefreshAttribute(attributes, 6, 344.0f), "standard weapon refresh should include paint");
        Expect(ContainsRefreshAttribute(attributes, 7, 123.0f), "standard weapon refresh should include the configured seed");
        Expect(
            ContainsRefreshAttribute(attributes, 8, 0.0000001f, 0.000001f),
            "standard weapon refresh should include the configured wear");
    }

    {
        nskinzcs2::model::CosmeticProfile profile;
        profile.base_item = 7;
        profile.paint_kit = 180;
        profile.wear = 0.0f;

        const auto attributes = nskinzcs2::runtime::BuildWeaponRefreshAttributes(profile);
        Expect(
            ContainsExactWearAttribute(attributes, nskinzcs2::model::kMinimumCosmeticWear),
            "a default-skinned weapon request below the supported wear floor should emit exact minimum wear");
    }

    {
        nskinzcs2::model::CosmeticProfile profile;
        profile.base_item = 42;
        profile.override_item = 0;
        profile.paint_kit = 0;
        profile.explicit_vanilla_finish = true;
        profile.wear = 0.01f;

        const auto attributes = nskinzcs2::runtime::BuildWeaponRefreshAttributes(profile);
        Expect(
            ContainsRefreshAttribute(attributes, 6, 0.0f),
            "a finish-only vanilla knife request should explicitly clear an existing paint attribute");
        Expect(
            ContainsExactWearAttribute(attributes, 0.01f),
            "a finish-only vanilla knife request should preserve its configured wear payload");
    }

    {
        nskinzcs2::model::CosmeticProfile profile;
        profile.base_item = 42;
        profile.override_item = 507;
        profile.stickers[0].kit_id = 3;
        profile.stickers[0].wear = 0.2f;
        profile.stickers[0].scale = 1.1f;
        profile.stickers[0].rotation = 45.0f;

        const auto attributes = nskinzcs2::runtime::BuildWeaponRefreshAttributes(profile);
        Expect(attributes.size() == 4, "sticker refresh should emit the full sticker attribute block");
        Expect(ContainsRefreshAttribute(attributes, 113, 3.0f), "sticker refresh should include sticker kit");
        Expect(ContainsRefreshAttribute(attributes, 114, 0.2f), "sticker refresh should include sticker wear");
        Expect(ContainsRefreshAttribute(attributes, 115, 1.1f), "sticker refresh should include sticker scale");
        Expect(ContainsRefreshAttribute(attributes, 116, 45.0f), "sticker refresh should include sticker rotation");
    }

    {
        nskinzcs2::model::CosmeticProfile profile;
        profile.base_item = 9;
        profile.paint_kit = 344;
        profile.seed = 0;
        profile.wear = 0.2f;
        profile.stat_trak = 9;
        profile.stickers[0].kit_id = 3;
        profile.stickers[0].wear = 0.2f;
        profile.stickers[0].scale = 1.1f;
        profile.stickers[0].rotation = 45.0f;

        const auto attributes = nskinzcs2::runtime::BuildWeaponRefreshAttributes(profile);
        Expect(ContainsRefreshAttribute(attributes, 6, 344.0f), "standard weapon refresh should include paint");
        Expect(ContainsRefreshAttribute(attributes, 7, 0.0f), "standard weapon refresh should include seed");
        Expect(ContainsRefreshAttribute(attributes, 8, 0.2f), "standard weapon refresh should include wear");
        Expect(ContainsRefreshAttribute(attributes, 80, 9.0f), "standard weapon refresh should include StatTrak");
        Expect(ContainsRefreshAttribute(attributes, 113, 3.0f), "standard weapon refresh should include sticker kit");
        Expect(ContainsRefreshAttribute(attributes, 114, 0.2f), "standard weapon refresh should include sticker wear");
        Expect(ContainsRefreshAttribute(attributes, 115, 1.1f), "standard weapon refresh should include sticker scale");
        Expect(ContainsRefreshAttribute(attributes, 116, 45.0f), "standard weapon refresh should include sticker rotation");
    }

    {
        nskinzcs2::model::CosmeticProfile profile;
        profile.custom_name = "demo only";

        const auto attributes = nskinzcs2::runtime::BuildWeaponRefreshAttributes(profile);
        Expect(attributes.empty(), "custom-name-only profiles should not force a weapon refresh attribute payload");
    }

    {
        nskinzcs2::model::CosmeticProfile profile;
        profile.base_item = 7;
        nskinzcs2::model::KeychainProfile keychain_profile;
        keychain_profile.xuid = 10;
        keychain_profile.base_item = 7;
        keychain_profile.keychain_kit = 101;

        const auto attributes = nskinzcs2::runtime::BuildWeaponRefreshAttributes(profile, &keychain_profile);
        Expect(attributes.size() == 7, "keychain refresh should emit the full slot 0 attribute block");
        ExpectContainsKeychainSlotZeroAttributes(attributes, 101.0f);
    }
}

void TestBuildMissingWeaponAttributeDefinitionIndices()
{
    {
        nskinzcs2::model::CosmeticProfile profile;
        profile.base_item = 9;
        profile.paint_kit = 344;
        profile.seed = 0;
        profile.wear = 0.0000001f;

        const auto missing =
            nskinzcs2::runtime::BuildMissingWeaponAttributeDefinitionIndices(profile, {6});
        Expect(missing.size() == 2, "paint-only profiles missing seed and wear should request two appended attributes");
        Expect(ContainsDefinitionIndex(missing, 7), "missing paint-only attributes should include seed");
        Expect(ContainsDefinitionIndex(missing, 8), "missing paint-only attributes should include wear");
    }

    {
        nskinzcs2::model::CosmeticProfile profile;
        profile.base_item = 42;
        profile.override_item = 507;
        profile.paint_kit = 344;
        profile.stat_trak = 9;
        profile.stickers[0].kit_id = 3;
        profile.stickers[0].wear = 0.2f;
        profile.stickers[0].scale = 1.1f;
        profile.stickers[0].rotation = 45.0f;

        const auto missing =
            nskinzcs2::runtime::BuildMissingWeaponAttributeDefinitionIndices(profile, {6, 7, 8});
        Expect(ContainsDefinitionIndex(missing, 80), "missing attribute set should include stattrak");
        Expect(ContainsDefinitionIndex(missing, 113), "missing attribute set should include sticker kit");
        Expect(ContainsDefinitionIndex(missing, 114), "missing attribute set should include sticker wear");
        Expect(ContainsDefinitionIndex(missing, 115), "missing attribute set should include sticker scale");
        Expect(ContainsDefinitionIndex(missing, 116), "missing attribute set should include sticker rotation");
    }

    {
        nskinzcs2::model::CosmeticProfile profile;
        profile.base_item = 7;
        nskinzcs2::model::KeychainProfile keychain_profile;
        keychain_profile.xuid = 10;
        keychain_profile.base_item = 7;
        keychain_profile.keychain_kit = 101;

        const auto missing =
            nskinzcs2::runtime::BuildMissingWeaponAttributeDefinitionIndices(profile, {}, &keychain_profile);
        Expect(missing.size() == 7, "missing attribute set should include the full keychain slot 0 attribute block");
        ExpectContainsKeychainSlotZeroDefinitionIndices(missing);
    }
}

void TestWeaponRefreshTargetMaterialRefreshGate()
{
    using nskinzcs2::runtime::CanonicalWeaponCarrierDecisionInputs;
    using nskinzcs2::runtime::CanonicalWeaponCarrierDecisionIsShared;
    using nskinzcs2::runtime::CanonicalWeaponCarrierKind;
    using nskinzcs2::runtime::ResolveCanonicalWeaponCarrierKind;
    using nskinzcs2::runtime::RequiresExactHudViewmodelPath;
    using nskinzcs2::runtime::ResolveWeaponMeshGroupMask;
    using nskinzcs2::runtime::ResolveLinkedWeaponRefreshTarget;
    using nskinzcs2::runtime::ShouldQueuePrimaryWeaponMaterialRefresh;
    using nskinzcs2::runtime::WeaponSkinRefreshTarget;
    using nskinzcs2::runtime::WeaponSkinRefreshTargetNeedsMaterialRefresh;

    Expect(
        WeaponSkinRefreshTargetNeedsMaterialRefresh(WeaponSkinRefreshTarget::CanonicalItemView),
        "canonical item views should trigger material refresh");
    Expect(
        WeaponSkinRefreshTargetNeedsMaterialRefresh(WeaponSkinRefreshTarget::PrimaryItemView),
        "primary item views should trigger material refresh");
    {
        CanonicalWeaponCarrierDecisionInputs inputs;
        inputs.world_valid = true;
        inputs.hud_valid = true;
        inputs.world_hud_shared = true;
        Expect(
            CanonicalWeaponCarrierDecisionIsShared(inputs),
            "world and HUD sharing an item view should mark the canonical carrier as shared");
        Expect(
            ResolveCanonicalWeaponCarrierKind(inputs) == CanonicalWeaponCarrierKind::World,
            "shared world and HUD candidates should canonicalize to the world owner");
    }
    {
        CanonicalWeaponCarrierDecisionInputs inputs;
        inputs.hud_valid = true;
        inputs.viewmodel_valid = true;
        inputs.hud_viewmodel_shared = true;
        Expect(
            ResolveCanonicalWeaponCarrierKind(inputs) == CanonicalWeaponCarrierKind::Hud,
            "shared HUD and viewmodel candidates should canonicalize to the HUD owner");
    }
    {
        CanonicalWeaponCarrierDecisionInputs inputs;
        inputs.world_valid = true;
        Expect(
            ResolveCanonicalWeaponCarrierKind(inputs) == CanonicalWeaponCarrierKind::World,
            "a lone world candidate should still canonicalize to world");
    }
    Expect(
        WeaponSkinRefreshTargetNeedsMaterialRefresh(WeaponSkinRefreshTarget::ExactHudItemView),
        "exact HUD item views should trigger material refresh");
    Expect(
        WeaponSkinRefreshTargetNeedsMaterialRefresh(WeaponSkinRefreshTarget::ViewModelItemView),
        "resolved first-person viewmodel item views should trigger material refresh");
    Expect(
        RequiresExactHudViewmodelPath(true, true),
        "watched active weapons should require the exact HUD viewmodel path");
    Expect(
        !RequiresExactHudViewmodelPath(true, false),
        "non-active watched weapons should not require the exact HUD viewmodel path");
    Expect(
        !RequiresExactHudViewmodelPath(false, true),
        "unwatched active weapons should not require the exact HUD viewmodel path");
    Expect(
        ShouldQueuePrimaryWeaponMaterialRefresh(true, true),
        "watched active weapons must queue a primary world-weapon material refresh");
    Expect(
        ShouldQueuePrimaryWeaponMaterialRefresh(true, false),
        "non-active watched weapons may still queue a primary material refresh");
    Expect(
        ShouldQueuePrimaryWeaponMaterialRefresh(false, true),
        "unwatched active weapons may still queue a primary material refresh");

    for (const auto definition_index : StandardWeaponDefinitionIndices())
    {
        Expect(
            ResolveWeaponMeshGroupMask(definition_index, 344) == 2,
            "every painted standard weapon should select the legacy weapon mesh group");
        Expect(
            ResolveWeaponMeshGroupMask(definition_index, 0) == 1,
            "every default standard weapon should keep the default mesh group");
    }

    Expect(
        ResolveWeaponMeshGroupMask(42, 38) == 1,
        "knife finishes should not enter the standard-weapon mesh group path");
    Expect(
        ResolveWeaponMeshGroupMask(5028, 10006) == 1,
        "glove finishes should not enter the standard-weapon mesh group path");
    Expect(
        WeaponSkinRefreshTargetNeedsMaterialRefresh(WeaponSkinRefreshTarget::HeuristicViewModelItemView),
        "validated heuristic first-person viewmodel item views should trigger material refresh");
    Expect(
        !WeaponSkinRefreshTargetNeedsMaterialRefresh(WeaponSkinRefreshTarget::SecondaryItemView),
        "secondary world item views should not trigger material refresh");
    Expect(
        !WeaponSkinRefreshTargetNeedsMaterialRefresh(WeaponSkinRefreshTarget::ProvideeItemView),
        "providee-linked item views should not trigger material refresh");
    Expect(
        !WeaponSkinRefreshTargetNeedsMaterialRefresh(WeaponSkinRefreshTarget::HeuristicProvideeItemView),
        "heuristic providee-linked item views should not trigger material refresh");
    Expect(
        !WeaponSkinRefreshTargetNeedsMaterialRefresh(WeaponSkinRefreshTarget::PreviewItemView),
        "preview item views should not trigger material refresh");

    {
        nskinzcs2::model::CosmeticProfile knife_profile;
        knife_profile.base_item = 42;
        knife_profile.paint_kit = 344;
        Expect(
            ResolveLinkedWeaponRefreshTarget(knife_profile, WeaponSkinRefreshTarget::ProvideeItemView) ==
                WeaponSkinRefreshTarget::ViewModelItemView,
            "knife providee-linked item views should be promoted to a first-person refresh target");
        Expect(
            WeaponSkinRefreshTargetNeedsMaterialRefresh(
                ResolveLinkedWeaponRefreshTarget(knife_profile, WeaponSkinRefreshTarget::ProvideeItemView)),
            "knife providee-linked refresh targets should trigger material refresh");
    }

    {
        nskinzcs2::model::CosmeticProfile standard_profile;
        standard_profile.base_item = 9;
        standard_profile.paint_kit = 344;
        Expect(
            ResolveLinkedWeaponRefreshTarget(standard_profile, WeaponSkinRefreshTarget::ProvideeItemView) ==
                WeaponSkinRefreshTarget::ProvideeItemView,
            "standard weapons should keep providee-linked refresh targets unchanged");
    }
}

void TestKnifeRepeatedFieldRepair()
{
    using nskinzcs2::runtime::ShouldQueueWeaponMaterialRefresh;
    Expect(!ShouldQueueWeaponMaterialRefresh(true, true), "completed knife refresh must not repeat after scalar repair");
    Expect(ShouldQueueWeaponMaterialRefresh(true, false), "a new finish must still regenerate materials");
    Expect(ShouldQueueWeaponMaterialRefresh(true, true, true), "an explicit force refresh must remain available");
    Expect(!ShouldQueueWeaponMaterialRefresh(false, false), "no cosmetic intent must not generate materials");
    const auto result = nskinzcs2::runtime::ProbeKnifeRepeatedFieldRepairForTests();
    Expect(result.repaired_frames == 240, "all 240 demo scalar rewrites must still be repaired");
    Expect(result.redundant_refresh_requests == 0, "240 unchanged knife frames must enqueue zero redundant regenerations");
    Expect(result.completed_request_reused, "the refresh dispatcher must deduplicate already-completed requests");
    Expect(result.changed_finish_queued, "changing paint and seed must still enqueue the native refresh");
    Expect(result.new_entity_epoch_queued, "a demo seek/entity rebuild must allow material initialization again");
    Expect(result.model_and_finish_preserved, "the M9 model and newly selected finish must remain applied");
}

void TestWeaponMeshMaskNonblocking()
{
    const auto result = nskinzcs2::runtime::ProbeWeaponMeshMaskForTests();
    std::cout << "[PERF] mesh_updates=" << result.successful_updates
        << " elapsed_ms=" << result.elapsed_milliseconds << '\n';
    Expect(result.successful_updates == 240 && result.both_masks_verified,
        "mesh updates must repair both masks, including repeated demo rewrites and 1/2 transitions");
    Expect(result.matching_readonly_is_noop, "matching meshes must not attempt even one redundant write");
    Expect(result.mismatching_readonly_rejected, "unwritable mismatching meshes must fail without waiting");
    Expect(result.missing_dirty_data_rejected, "missing dirty model storage must not report a successful apply");
    // Broad enough for loaded CI hosts, but detects the old 240 * Sleep(5)
    // regression (3.7 seconds on this Windows host). Not a game FPS assertion.
    Expect(result.elapsed_milliseconds < 500.0, "240 mesh updates must not contain a game-thread sleep/retry loop");
}

void TestImmutableCatalogSnapshot()
{
    nskinzcs2::AppState state;
    nskinzcs2::model::Catalog original;
    original.weapon_paint_kits = {{0, "-"}, {617, "Doppler"}, {1040, "The Traitor"}};
    original.sticker_kits.resize(11642);
    state.ReplaceCatalog(original);
    const auto first = state.SnapshotCatalogShared();
    for (int i = 0; i < 240; ++i)
        Expect(state.SnapshotCatalogShared().get() == first.get(), "runtime catalog reads must not copy the entire catalog");
    auto editable = state.SnapshotCatalog();
    editable.weapon_paint_kits.push_back({568, "Gamma Doppler"});
    Expect(first->weapon_paint_kits.size() == 3, "editable UI copies must not mutate a runtime snapshot");
    state.ReplaceCatalog(std::move(editable));
    const auto second = state.SnapshotCatalogShared();
    Expect(second.get() != first.get() && second->weapon_paint_kits.size() == 4 &&
        second->sticker_kits.size() == 11642, "catalog publication must retain every entry, including newly loaded finishes");
    Expect(first->weapon_paint_kits.size() == 3 && first->sticker_kits.size() == 11642,
        "an active apply pass must retain its old catalog safely across replacement");
}

void TestDeferredRuntimeLogging()
{
    const auto directory = std::filesystem::temp_directory_path();
    const auto filename = L"nSkinzCS2.deferred." + std::to_wstring(GetCurrentProcessId()) + L".log";
    const auto path = directory / filename;
    nskinzcs2::Logger::Initialize(directory.wstring(), filename);
    const auto initial_size = std::filesystem::file_size(path);
    {
        nskinzcs2::Logger::DeferredScope outer;
        nskinzcs2::Logger::Info(L"queued-outer-marker");
        {
            nskinzcs2::Logger::DeferredScope nested;
            nskinzcs2::Logger::Warn(L"queued-inner-marker");
        }
        nskinzcs2::Logger::FlushDeferred();
        Expect(std::filesystem::file_size(path) == initial_size,
            "neither nested logging nor flush inside a game callback may touch the file");
        for (int i = 0; i < 600; ++i) nskinzcs2::Logger::Info(L"bounded-queue-entry");
    }
    Expect(std::filesystem::file_size(path) == initial_size, "leaving the callback must not flush on the game thread");
    nskinzcs2::Logger::FlushDeferred();
    std::wifstream stream(path);
    const std::wstring content((std::istreambuf_iterator<wchar_t>(stream)), std::istreambuf_iterator<wchar_t>());
    Expect(content.find(L"queued-outer-marker") != std::wstring::npos &&
        content.find(L"queued-inner-marker") != std::wstring::npos, "the worker must retain normal callback diagnostics");
    Expect(content.find(L"diagnostics dropped=90") != std::wstring::npos,
        "queue overflow must be bounded and reported instead of blocking gameplay");
    stream.close();
    std::error_code error;
    std::filesystem::remove(path, error);
}

void TestStandardWeaponAttributeSynchronization()
{
    using nskinzcs2::runtime::ProbeStandardWeaponAttributeSynchronizationForTests;
    const std::vector<std::pair<std::uint16_t, float>> desired {{6, 1040}, {7, 0}, {8, 0.0000001f}};
    const std::vector<std::pair<std::uint16_t, float>> recorded {{6, 1142}, {7, 913.7957f}, {8, 0.2667014f}};
    const auto usp = ProbeStandardWeaponAttributeSynchronizationForTests({}, recorded, desired);
    Expect(!usp.matched_before, "USP fallback 1040 must not hide recorded dynamic paint 1142");
    Expect(usp.synchronized && usp.matched_after && usp.changed_slots == 3, "all three recorded USP finish slots must synchronize");
    Expect(usp.repeated_update_is_noop, "repeated Update must not notify or write matching slots again");
    Expect(usp.demo_rewrite_repaired, "a demo rewrite must be repaired without replacing its vector");
    Expect(usp.vectors_and_node_metadata_preserved, "never modify vectors, definition IDs, owners or node metadata");
    Expect(usp.original_values_restored, "runtime reset must restore the recorded values in unchanged storage");

    const auto conflict = ProbeStandardWeaponAttributeSynchronizationForTests(recorded, desired, desired);
    Expect(!conflict.matched_before && conflict.synchronized && conflict.matched_after,
        "a matching dynamic list must not hide a conflicting static mirror");
    Expect(conflict.changed_slots == 3 && conflict.original_values_restored, "only the conflicting mirror should be rewritten and restored");
    const auto duplicate = ProbeStandardWeaponAttributeSynchronizationForTests({}, {{6, 1040}, {6, 1142}}, desired);
    Expect(!duplicate.matched_before && duplicate.matched_after && duplicate.changed_slots == 1,
        "all duplicate paint slots must be verified, not just the first match");
    const auto stock = ProbeStandardWeaponAttributeSynchronizationForTests({}, {}, desired, false);
    Expect(stock.synchronized && stock.matched_after && stock.changed_slots == 0,
        "in-place-only scanning accepts absent scalar slots but is not proof of material readiness");
    const auto vanilla = ProbeStandardWeaponAttributeSynchronizationForTests({}, recorded, {{6, 0}, {7, 0}, {8, 0.0000001f}});
    Expect(vanilla.synchronized && vanilla.matched_after && vanilla.original_values_restored,
        "explicit vanilla must synchronize paint zero without deleting or reallocating slots");
    const auto unchanged = ProbeStandardWeaponAttributeSynchronizationForTests({}, recorded, {}, false);
    Expect(unchanged.synchronized && unchanged.changed_slots == 0 && unchanged.original_values_restored,
        "No change must leave the original finish untouched");
    const auto no_callback = ProbeStandardWeaponAttributeSynchronizationForTests({}, recorded, desired, false);
    Expect(!no_callback.synchronized && !no_callback.matched_after && no_callback.original_values_restored,
        "missing schema notification must fail without silently applying raw attribute writes");
    const auto failed = ProbeStandardWeaponAttributeSynchronizationForTests({}, recorded, desired, true, 2);
    Expect(!failed.synchronized && failed.original_values_restored && failed.vectors_and_node_metadata_preserved,
        "a mid-transaction notification failure must roll back scalar values without damaging nodes");
    const auto sticker = ProbeStandardWeaponAttributeSynchronizationForTests({}, recorded, {{113, 12}});
    Expect(!sticker.synchronized && sticker.original_values_restored,
        "missing attributes without entity fallback storage cannot count as applied");
    const auto invalid = ProbeStandardWeaponAttributeSynchronizationForTests({}, {{6, 1040}}, {{6, std::numeric_limits<float>::quiet_NaN()}});
    Expect(!invalid.synchronized && invalid.original_values_restored, "non-finite desired attributes must not be written");
}

void TestStandardWeaponStockInitialization()
{
    using namespace nskinzcs2::runtime;
    Expect(ProbeStandardWeaponStockFinishForTests(),
        "stock AWP BOOM: engine-owned attributes, native initialization, restore, stale identity and retry bounds");
    constexpr std::array<std::uint8_t, 42> calls {
        0x83,0xBB,0xB0,0x0A,0,0,0,0x7F,9,0x83,0xBB,0xC8,0x0A,0,0,0,0x7E,0x18,
        0x48,0x8D,0x8B,0x10,6,0,0,0xB2,1,0xE8,0xF0,0xAE,0xC3,0,0x33,0xD2,0x48,0x8B,0xCB,
        0xE8,0xD6,0xA6,0xFD,0xFF
    };
    WeaponMaterialCalls decoded;
    Expect(DecodeWeaponMaterialCalls(calls, decoded), "native material loop call ABI must decode");
    Expect(0x7FF0F0 + 32 + decoded.clear_relative == 0x143A000 &&
        0x7FF0F0 + 42 + decoded.generate_relative == 0x7D97F0, "rel32 targets must preserve their signed displacement");
    for (std::size_t i = 0; i < calls.size(); ++i)
    {
        Expect(!DecodeWeaponMaterialCalls(std::span(calls).first(i), decoded), "truncated instruction window rejected");
        if ((i >= 28 && i < 32) || i >= 38) continue;
        auto corrupt = calls; corrupt[i] ^= 1;
        Expect(!DecodeWeaponMaterialCalls(corrupt, decoded), "changed gate/owner offset/call ABI rejected");
    }
    Expect(!StandardWeaponHasCompositeMaterial(0, 0) && StandardWeaponHasCompositeMaterial(1, 0) &&
        StandardWeaponHasCompositeMaterial(0, 1) && !StandardWeaponHasCompositeMaterial(-1, 1) &&
        !StandardWeaponHasCompositeMaterial(129, 0), "empty/invalid material containers cannot count as generated");
    if (const auto* client = _wgetenv(L"NSKINZCS2_KNIFE_CLIENT_DLL"); client != nullptr)
    {
        Expect(ProbeStandardWeaponMaterialCallsForTests(client), "actual installed client must resolve both native material callees");
        std::cout << "standard_weapon_material_native_evidence=PASS\n";
    }
}

void TestStandardWeaponRefreshModel()
{
    using nskinzcs2::runtime::BuildStandardWeaponMaterialCacheKey;
    using nskinzcs2::runtime::StandardWeaponNeedsItemIdHighReset;
    using nskinzcs2::runtime::UsesTemporaryWorldWeaponRefreshModel;

    {
        for (const auto definition_index : StandardWeaponDefinitionIndices())
        {
            nskinzcs2::model::CosmeticProfile profile;
            profile.base_item = definition_index;
            profile.paint_kit = 344;
            profile.seed = 0;
            profile.wear = 0.0000001f;

            Expect(
                UsesTemporaryWorldWeaponRefreshModel(profile),
                "every standard weapon profile should use the temporary world-weapon refresh model");
        }

        Expect(
            StandardWeaponNeedsItemIdHighReset(0xFFFFFFFFu, true),
            "standard weapons with item_id_high already set to -1 should force a 0 -> -1 activation transition");
        Expect(
            !StandardWeaponNeedsItemIdHighReset(0u, true),
            "standard weapons with a fresh item_id_high do not need a reset transition");

        nskinzcs2::model::CosmeticProfile cache_profile;
        cache_profile.base_item = 9;
        cache_profile.paint_kit = 756;
        const auto gungnir_key =
            BuildStandardWeaponMaterialCacheKey(cache_profile, 76561198271727979ull, 9);
        Expect(
            gungnir_key != 0 && gungnir_key != 0xFFFFFFFFu,
            "standard weapon material cache keys must avoid reserved item-id values");
        Expect(
            gungnir_key == BuildStandardWeaponMaterialCacheKey(
                cache_profile,
                76561198271727979ull,
                9),
            "the same standard weapon appearance should produce a stable material cache key");
        cache_profile.wear = 0.0f;
        Expect(
            gungnir_key == BuildStandardWeaponMaterialCacheKey(
                cache_profile,
                76561198271727979ull,
                9),
            "wear below the supported floor should normalize to the minimum material cache identity");
        cache_profile.wear = std::nextafter(
            nskinzcs2::model::kMinimumCosmeticWear,
            nskinzcs2::model::kMaximumCosmeticWear);
        Expect(
            gungnir_key != BuildStandardWeaponMaterialCacheKey(
                cache_profile,
                76561198271727979ull,
                9),
            "adjacent supported wear values should produce distinct material cache identities");
        cache_profile.wear = nskinzcs2::model::kMinimumCosmeticWear;
        cache_profile.paint_kit = 344;
        Expect(
            gungnir_key != BuildStandardWeaponMaterialCacheKey(
                cache_profile,
                76561198271727979ull,
                9),
            "changing paint kit should change the standard weapon material cache key");
    }

    {
        nskinzcs2::model::CosmeticProfile profile;
        profile.base_item = 42;
        profile.override_item = 507;
        profile.paint_kit = 344;

        Expect(
            !UsesTemporaryWorldWeaponRefreshModel(profile),
            "knife override profiles should not use the standard weapon temporary refresh model");
        Expect(
            !StandardWeaponNeedsItemIdHighReset(0xFFFFFFFFu, false),
            "non-standard weapon paths must not force the standard item_id_high reset transition");
    }

    {
        nskinzcs2::model::CosmeticProfile profile;
        profile.base_item = 5028;
        profile.paint_kit = 10006;

        Expect(
            !UsesTemporaryWorldWeaponRefreshModel(profile),
            "glove profiles should not use the standard weapon temporary refresh model");
    }
}

void TestWeaponAppearanceModule()
{
    using nskinzcs2::runtime::BuildWeaponAppearancePayload;
    using nskinzcs2::runtime::WeaponAppearanceNeedsIndependentHudApply;
    using nskinzcs2::runtime::ResolveWeaponAppearanceBinding;
    using nskinzcs2::runtime::WeaponAppearanceBindingInputs;
    using nskinzcs2::runtime::WeaponAppearanceRollbackOrder;
    using nskinzcs2::runtime::WeaponAppearanceRollbackStep;

    {
        WeaponAppearanceBindingInputs inputs;
        inputs.owner_xuid = 76561198000000000ull;
        inputs.owned_weapon_handle = 0x1234u;
        inputs.owned_weapon_entity_index = 68;
        inputs.owned_weapon_has_item_view = true;
        inputs.explicit_world_child_count = 1;
        inputs.explicit_world_child_entity_index = 412;
        inputs.explicit_world_child_has_item_view = true;
        inputs.explicit_world_child_item_view_shared_with_owned = false;
        inputs.hud_entity_index = 600;
        inputs.mesh_group_mask = 2;

        const auto binding = ResolveWeaponAppearanceBinding(inputs);
        Expect(binding.valid, "binding should resolve when the owned weapon is valid");
        Expect(binding.render_entity_index == 412, "a unique explicit owner-child should become the render entity");
        Expect(binding.carrier_entity_index == 412, "an explicit child with its own item view should become the carrier");
        Expect(binding.hud_entity_index == 600, "HUD entity should be preserved in the binding");
        Expect(binding.mesh_group_mask == 2, "mesh group mask should flow through the binding");
    }

    {
        WeaponAppearanceBindingInputs inputs;
        inputs.owned_weapon_entity_index = 68;
        inputs.owned_weapon_has_item_view = true;
        inputs.explicit_world_child_count = 0;

        const auto binding = ResolveWeaponAppearanceBinding(inputs);
        Expect(binding.valid, "binding should resolve for a plain owned weapon");
        Expect(binding.render_entity_index == 68, "without an explicit child the owned weapon remains the render entity");
        Expect(binding.carrier_entity_index == 68, "without an explicit child the owned weapon remains the carrier");
    }

    {
        nskinzcs2::model::CosmeticProfile profile;
        profile.base_item = 9;
        profile.paint_kit = 344;
        profile.seed = 0;
        profile.wear = 0.2f;
        profile.stat_trak = 9;
        profile.custom_name = "Dragon Lore";
        profile.stickers[0].kit_id = 3;
        profile.stickers[0].wear = 0.2f;
        profile.stickers[0].scale = 1.1f;
        profile.stickers[0].rotation = 45.0f;

        const auto payload = BuildWeaponAppearancePayload(profile, 123u);
        Expect(payload.fallback_paint_kit == 344, "payload should preserve fallback paint");
        Expect(payload.fallback_seed == 0, "payload should preserve fallback seed");
        Expect(std::fabs(payload.fallback_wear - 0.2f) <= 0.0001f, "payload should preserve fallback wear");
        Expect(payload.fallback_stat_trak == 9, "payload should preserve fallback StatTrak");
        Expect(payload.account_id == 123u, "payload should preserve account id");
        Expect(payload.writes_custom_name, "payload should mark custom-name writes");
        Expect(payload.needs_refresh, "payload should request refresh when temporary attributes exist");
        Expect(ContainsRefreshAttribute(payload.temporary_attributes, 6, 344.0f), "payload should include paint");
        Expect(ContainsRefreshAttribute(payload.temporary_attributes, 8, 0.2f), "payload should include wear");
        Expect(ContainsRefreshAttribute(payload.temporary_attributes, 80, 9.0f), "payload should include StatTrak");
        Expect(ContainsRefreshAttribute(payload.temporary_attributes, 113, 3.0f), "payload should include sticker kit");
    }

    {
        nskinzcs2::model::CosmeticProfile profile;
        profile.base_item = 7;
        profile.paint_kit = 180;
        profile.wear = 0.0f;

        const auto payload = BuildWeaponAppearancePayload(profile, 123u);
        Expect(
            nskinzcs2::model::CosmeticWearMatchesExactly(
                payload.fallback_wear,
                nskinzcs2::model::kMinimumCosmeticWear),
            "default-skinned weapon fallback wear should normalize to the exact minimum");
        Expect(
            ContainsExactWearAttribute(
                payload.temporary_attributes,
                nskinzcs2::model::kMinimumCosmeticWear),
            "default-skinned weapon payload should carry exact minimum wear in attribute 8");
    }

    {
        nskinzcs2::model::CosmeticProfile profile;
        profile.base_item = 7;
        profile.paint_kit = 0;
        profile.seed = 777;
        profile.wear = 0.77f;
        const auto payload = BuildWeaponAppearancePayload(profile, 123u);
        Expect(
            !ContainsRefreshAttribute(payload.temporary_attributes, 6, 0.0f),
            "dash standard-weapon profile should not write paint 0");
        Expect(!payload.needs_refresh, "dash standard-weapon profile should not rebuild materials");
        Expect(
            !ContainsRefreshAttribute(payload.temporary_attributes, 7, 777.0f) &&
                !ContainsRefreshAttribute(payload.temporary_attributes, 8, 0.77f),
            "dash standard-weapon profile should not write seed or wear");
    }

    {
        nskinzcs2::model::CosmeticProfile profile;
        profile.base_item = 7;
        profile.paint_kit = 0;
        profile.explicit_vanilla_finish = true;

        const auto payload = BuildWeaponAppearancePayload(profile, 123u);
        Expect(
            ContainsRefreshAttribute(payload.temporary_attributes, 6, 0.0f),
            "an explicit vanilla standard-weapon profile should carry paint 0");
        Expect(
            payload.needs_refresh,
            "an explicit vanilla standard-weapon profile should rebuild the cached material");
    }

    {
        nskinzcs2::model::CosmeticProfile profile;
        profile.base_item = 42;
        profile.paint_kit = 0;

        auto payload = BuildWeaponAppearancePayload(profile, 123u);
        Expect(
            !ContainsRefreshAttribute(payload.temporary_attributes, 6, 0.0f),
            "dash knife profile should not write paint 0");

        profile.explicit_vanilla_finish = true;
        payload = BuildWeaponAppearancePayload(profile, 123u);
        Expect(
            ContainsRefreshAttribute(payload.temporary_attributes, 6, 0.0f),
            "explicit vanilla knife profile should carry paint 0");
    }

    {
        nskinzcs2::model::CosmeticProfile profile;
        profile.base_item = 7;
        nskinzcs2::model::KeychainProfile keychain_profile;
        keychain_profile.xuid = 10;
        keychain_profile.base_item = 7;
        keychain_profile.keychain_kit = 101;

        const auto payload = BuildWeaponAppearancePayload(profile, 123u, &keychain_profile);
        Expect(payload.temporary_attributes.size() == 7, "payload should include the full keychain slot 0 attribute block");
        ExpectContainsKeychainSlotZeroAttributes(payload.temporary_attributes, 101.0f);
        Expect(!payload.needs_refresh, "keychain-only payload should stay off the refresh helper path");
    }

    {
        nskinzcs2::model::CosmeticProfile profile;
        profile.base_item = 7;
        nskinzcs2::model::KeychainProfile keychain_profile;
        keychain_profile.xuid = 10;
        keychain_profile.base_item = 7;

        auto payload = BuildWeaponAppearancePayload(profile, 123u, &keychain_profile);
        Expect(payload.temporary_attributes.empty(), "dash keychain profile should not write attributes");

        keychain_profile.explicit_clear = true;
        payload = BuildWeaponAppearancePayload(profile, 123u, &keychain_profile);
        Expect(payload.temporary_attributes.size() == 7, "explicit keychain clear should write slot zero");
        ExpectContainsKeychainSlotZeroAttributes(payload.temporary_attributes, 0.0f);
    }

    {
        Expect(
            WeaponAppearanceNeedsIndependentHudApply(true, 600, true, false),
            "an independent HUD item view should request its own exact first-person apply");
        Expect(
            !WeaponAppearanceNeedsIndependentHudApply(true, 600, true, true),
            "shared HUD item views should stay on the carrier-only path");
        Expect(
            !WeaponAppearanceNeedsIndependentHudApply(true, 600, false, false),
            "missing HUD item views should not request a supplemental first-person apply");
        Expect(
            !WeaponAppearanceNeedsIndependentHudApply(false, -1, true, false),
            "missing HUD entities should not request a supplemental first-person apply");
        Expect(
            !WeaponAppearanceNeedsIndependentHudApply(true, -514777624, true, false),
            "synthetic negative HUD entity indices should not request a supplemental first-person apply");
    }

    {
        const auto rollback = WeaponAppearanceRollbackOrder();
        Expect(
            rollback[0] == WeaponAppearanceRollbackStep::RestoreAttributeVector,
            "rollback should restore the attribute vector first");
        Expect(
            rollback[1] == WeaponAppearanceRollbackStep::RestorePrecacheFlag,
            "rollback should restore the precache flag second");
        Expect(
            rollback[2] == WeaponAppearanceRollbackStep::RestoreMeshState,
            "rollback should restore mesh state last");
    }
}

void TestM4A4ModernLegacyTransition()
{
    using namespace nskinzcs2::runtime;
    nskinzcs2::model::Catalog catalog;
    catalog.weapon_paint_uses_legacy_model = {{844, true}, {1353, false}};
    Expect(ResolvePaintMeshGroupMask(true, 1353, &catalog) == 1, "Full Throttle uses modern mesh");
    Expect(ResolvePaintMeshGroupMask(true, 844, &catalog) == 2, "The Emperor uses legacy mesh");
    Expect(ResolvePaintMeshGroupMask(true, 0, &catalog) == 1, "explicit vanilla uses modern mesh");
    Expect(ResolvePaintMeshGroupMask(false, 844, &catalog) == 1, "gun metadata cannot change knife/glove meshes");
    Expect(ResolvePaintMeshGroupMask(true, 844, nullptr) == 2, "old snapshots retain backward-compatible fallback");
    Expect(ResolvePaintMeshGroupMask(true, 999999, &catalog) == 2, "unknown paint is not misreported as known modern");

    constexpr std::string_view m4 = "weapons/models/rifles/m4a1/weapon_m4a1.vmdl";
    constexpr std::uint32_t handle = 0x300076A;
    const auto accepts = [&](std::wstring_view type, std::string_view model, std::uint32_t owner,
        bool under_arms = true, bool alias = false)
    { return IsStandardWeaponHudRenderCandidate(type, model, owner, handle, under_arms, alias); };
    Expect(accepts(L"C_CS2HudModelWeapon", m4, handle), "M4A4 HUD does not need a knife token or econ item view");
    Expect(!accepts(L"C_CS2HudModelArms", m4, handle), "arms must never become gun render consumer");
    Expect(!accepts(L"C_BaseModelEntity", m4, handle), "model filename alone cannot prove HUD ownership");
    Expect(!accepts(L"C_CS2HudModelWeapon", m4, handle + 0x8000), "same slot with a different serial is rejected");
    Expect(!accepts(L"C_CS2HudModelWeapon", m4, handle, false), "disconnected gun node is rejected");
    Expect(!accepts(L"C_CS2HudModelWeapon", m4, handle, true, true), "world/arms aliases are rejected");
    for (const auto model : {"characters/models/ct.vmdl", "weapons/models/shared/arms/ct.vmdl",
        "weapons/models/knife/knife_m9/weapon_knife_m9.vmdl", "weapons/models/glove.vmdl", ""})
    {
        Expect(!accepts(L"C_CS2HudModelWeapon", model, handle), "non-gun model excluded from gun HUD chain");
    }
    Expect(ProbeStandardWeaponRenderOnlyBindingForTests(), "verified unindexed HUD survives production binding; unproven pointers do not");
    Expect(ProbeStandardWeaponHudDiscoveryForTests(), "production HUD walker verifies ownership, serials, active weapon and uniqueness");
    Expect(ProbeStandardWeaponHudPendingGateForTests(), "an expected but missing active HUD defers before writes and material refresh");
    Expect(ProbeStandardWeaponAppearanceEpochForTests(), "only verified carrier/render generations invalidate completed refresh; stable or hidden HUDs do not");
    nskinzcs2::model::CosmeticProfile profile;
    profile.base_item = 16;
    for (const int paint : {1353, 844, 1353, 844})
    {
        profile.paint_kit = paint;
        profile.seed = 321;
        profile.wear = 0.17f;
        const auto payload = BuildWeaponAppearancePayload(profile, 123);
        Expect(payload.fallback_paint_kit == paint && payload.fallback_seed == 321 && payload.fallback_wear == 0.17f,
            "mesh family switching preserves requested finish/seed/wear");
        Expect(ContainsRefreshAttribute(payload.temporary_attributes, 6, static_cast<float>(paint)),
            "family switching does not turn the finish into paint zero");
    }
}

void TestGloveNativeVisualRefresh()
{
    using namespace nskinzcs2::runtime;
    Expect(ProbeGloveVisualRefreshEvidenceForTests(), "glove visual updater entry/consumer/caller/HUD evidence rejects drift");
    Expect(GloveVisualIdentityMatchesOutputHandle(0x15774231u, 0x15774231u),
        "ordinary native glove output accepts an exact full handle");
    Expect(!GloveVisualIdentityMatchesOutputHandle(0x15774231u, 0x1577FFFFu) &&
        !GloveVisualIdentityMatchesOutputHandle(0x15774231u, 0x1578FFFFu) &&
        !GloveVisualIdentityMatchesOutputHandle(0x15774231u, 0x15774232u) &&
        !GloveVisualIdentityMatchesOutputHandle(0x1577FFFFu, 0x1577FFFFu) &&
        !GloveVisualIdentityMatchesOutputHandle(0xFFFFFFFFu, 0xFFFFFFFFu),
        "glove output rejects freed sentinels, another generation, another live slot and invalid handles");
    GloveVisualRefreshKey key {1, 76561199840349579ull, 57, 0x123400, 0x83DD, 100};
    GloveVisualRefreshLatch latch;
    Expect(!latch.Begin({}, 1) && latch.Begin(key, 100), "invalid glove identity cannot dispatch; valid identity starts once");
    for (int i = 0; i < 1000; ++i)
        Expect(!latch.Begin(key, 101 + i), "repeated field repairs and Update never repeat the native glove rebuild");
    Expect(!latch.TimedOut(99) && !latch.TimedOut(3100) && latch.TimedOut(3101), "glove pending timer is bounded without retrying native calls");
    Expect(latch.Complete() && !latch.Complete() && !latch.Begin(key, 3200), "native output completion is a single transition");
    for (int field = 0; field < 6; ++field)
    {
        auto changed = key;
        switch (field)
        {
        case 0: ++changed.scene; break;
        case 1: ++changed.xuid; break;
        case 2: ++changed.signature; break;
        case 3: ++changed.pawn; break;
        case 4: ++changed.handle; break;
        case 5: ++changed.creation_tick; break;
        }
        Expect(latch.Begin(changed, 4000 + field) && !latch.Completed(), "new request/owner/entity generation gets its own glove rebuild");
        latch.Fail();
        Expect(latch.Failed() && !latch.Begin(changed, 5000 + field), "native fault does not cause an automatic rebuild loop");
    }
    if (const auto* client = _wgetenv(L"NSKINZCS2_KNIFE_CLIENT_DLL"))
        Expect(ProbeGloveVisualRefreshEvidenceForTests(client), "installed game matches native glove visual ABI; no game process loaded");
}

void TestKnifeUpdateListenerFiltering()
{
    using namespace nskinzcs2::runtime;
    constexpr std::uintptr_t controller = 0x1000, world = 0x2000, arms = 0x3000;
    Expect(ShouldDispatchKnifeWeaponListener(controller, world, world, controller, world),
        "the exact active knife controller/world pair is dispatched");
    Expect(!ShouldDispatchKnifeWeaponListener(controller, world, world, controller + 1, world),
        "an active knife never accepts another controller through its world-only fallback");
    Expect(!ShouldDispatchKnifeWeaponListener(controller, world, world, controller, world + 1),
        "an unrelated world weapon is excluded from the active knife lifecycle");
    Expect(ShouldDispatchKnifeWeaponListener(0, 0, world, controller + 1, world),
        "holstered pre-arm dispatches only its leased world independent of controller");
    Expect(!ShouldDispatchKnifeWeaponListener(0, 0, world, controller, world + 1) &&
        !ShouldDispatchKnifeWeaponListener(0, 0, 0, controller, world),
        "unpublished and unrelated native updates stay on the low-cost trampoline");
    Expect(ShouldDispatchKnifeArmsListener(arms, arms) &&
        !ShouldDispatchKnifeArmsListener(arms, arms + 1) &&
        !ShouldDispatchKnifeArmsListener(0, arms),
        "before/after arms lifecycle events require the exact active arms object");
}

void TestDirectEntityCacheScan()
{
    Expect(nskinzcs2::runtime::ProbeDirectEntityCacheScanForTests(),
        "verified chunk scanning preserves native handle checks without per-slot protected calls");
}

int RunAllTests()
{
    try
    {
        if (g_weapon_paint_only)
        {
            RunNamedTest("TestCatalogStoreRoundTrip", TestCatalogStoreRoundTrip);
            RunNamedTest("TestCatalogFromGameText", TestCatalogFromGameText);
            RunNamedTest("TestWeaponPaintCompatibility", TestWeaponPaintCompatibility);
            RunNamedTest("TestCatalogFromRealGameFilesIfProvided", TestCatalogFromRealGameFilesIfProvided);
            RunNamedTest("TestM4A4ModernLegacyTransition", TestM4A4ModernLegacyTransition);
            RunNamedTest("TestStandardWeaponAttributeSynchronization", TestStandardWeaponAttributeSynchronization);
            RunNamedTest("TestStandardWeaponStockInitialization", TestStandardWeaponStockInitialization);
            RunNamedTest("TestWeaponMeshMaskNonblocking", TestWeaponMeshMaskNonblocking);
            RunNamedTest("TestWeaponAppearanceModule", TestWeaponAppearanceModule);
            std::cout << "weapon paint regression passed (no CS2 process)\n";
            return EXIT_SUCCESS;
        }
        if (g_mesh_benchmark_only)
        {
            const auto result = nskinzcs2::runtime::ProbeWeaponMeshMaskForTests();
            std::cout << "mesh_updates=" << result.successful_updates
                << " elapsed_ms=" << result.elapsed_milliseconds
                << " readonly_noop=" << result.matching_readonly_is_noop
                << " readonly_mismatch_rejected=" << result.mismatching_readonly_rejected
                << " missing_dirty_rejected=" << result.missing_dirty_data_rejected << '\n';
            return result.both_masks_verified ? EXIT_SUCCESS : EXIT_FAILURE;
        }
        RunNamedTest("TestIndependentKnifeModel", RunKnifeModelTests);
        RunNamedTest("TestKnifeGraphParameter", RunKnifeGraphTests);
        RunNamedTest("TestKnifePairTransaction", RunKnifePairTests);
        RunNamedTest("TestKnifeUpdateListenerFiltering", TestKnifeUpdateListenerFiltering);
        RunNamedTest("TestDirectEntityCacheScan", TestDirectEntityCacheScan);
        RunNamedTest("TestGloveNativeVisualRefresh", TestGloveNativeVisualRefresh);
        if (g_knife_model_only)
        {
            return EXIT_SUCCESS;
        }
        RunNamedTest("TestCompatibilityManifest", TestCompatibilityManifest);
        RunNamedTest("TestCatalogStoreRoundTrip", TestCatalogStoreRoundTrip);
        RunNamedTest("TestInstalledCompatibilityIfRequested", TestInstalledCompatibilityIfRequested);
        RunNamedTest("TestConfigRoundTrip", TestConfigRoundTrip);
        RunNamedTest("TestWorkshopMapDiscovery", TestWorkshopMapDiscovery);
        RunNamedTest("TestCosmeticWearPolicy", TestCosmeticWearPolicy);
        RunNamedTest("TestProfileMatching", TestProfileMatching);
        RunNamedTest("TestKeychainConfigAndMatching", TestKeychainConfigAndMatching);
        RunNamedTest("TestGloveConfigAndMatching", TestGloveConfigAndMatching);
        RunNamedTest("TestDemoSkinRewritePlan", RunDemoSkinRewriteTests);
        RunNamedTest("TestKnifeProfileConfigMigration", TestKnifeProfileConfigMigration);
        RunNamedTest("TestCatalogFallback", TestCatalogFallback);
        RunNamedTest("TestCatalogFromGameFiles", TestCatalogFromGameFiles);
        RunNamedTest("TestCatalogFromGameText", TestCatalogFromGameText);
        RunNamedTest("TestWeaponPaintCompatibility", TestWeaponPaintCompatibility);
        RunNamedTest("TestCatalogFromRealGameFilesIfProvided", TestCatalogFromRealGameFilesIfProvided);
        RunNamedTest("TestDuplicateDetection", TestDuplicateDetection);
        RunNamedTest("TestVersionCandidateFiltering", TestVersionCandidateFiltering);
        RunNamedTest("TestRuntimeRebuildDecision", TestRuntimeRebuildDecision);
        RunNamedTest("TestKnifeRefreshCrashGuardPolicy", TestKnifeRefreshCrashGuardPolicy);
        RunNamedTest("TestKnifeAnimationRemapTable", TestKnifeAnimationRemapTable);
        RunNamedTest("TestPreferredKnifeTargetXuid", TestPreferredKnifeTargetXuid);
        RunNamedTest("TestWatchedKnifeTargetResolutionFallback", TestWatchedKnifeTargetResolutionFallback);
        RunNamedTest("TestRuntimeGloveBindingChoice", TestRuntimeGloveBindingChoice);
        RunNamedTest("TestWearableFactoryConfirmation", TestWearableFactoryConfirmation);
        RunNamedTest("TestKnifeAppearanceModule", TestKnifeAppearanceModule);
    RunNamedTest("TestKnifePhase2WorldOnlyModule", TestKnifePhase2WorldOnlyModule);
    RunNamedTest("TestKnifeExactWorldBindingModule", TestKnifeExactWorldBindingModule);
    RunNamedTest("TestKnifeHudAliasSyntheticProxyExperiment", TestKnifeHudAliasSyntheticProxyExperiment);
    RunNamedTest("TestKnifeCarrierDiscoveryReadOnlyDiscovery", TestKnifeCarrierDiscoveryReadOnlyDiscovery);
    RunNamedTest("TestKnifeAnimationModule", TestKnifeAnimationModule);
        RunNamedTest("TestGloveAppearanceModule", TestGloveAppearanceModule);
        RunNamedTest("TestSource2ClientGetterProbe", TestSource2ClientGetterProbe);
        RunNamedTest("TestGloveWearableCandidateRanking", TestGloveWearableCandidateRanking);
        RunNamedTest("TestDataFirstWearableClientClassInference", TestDataFirstWearableClientClassInference);
        RunNamedTest("TestBuildWeaponRefreshAttributes", TestBuildWeaponRefreshAttributes);
        RunNamedTest("TestBuildMissingWeaponAttributeDefinitionIndices", TestBuildMissingWeaponAttributeDefinitionIndices);
        RunNamedTest("TestWeaponRefreshTargetMaterialRefreshGate", TestWeaponRefreshTargetMaterialRefreshGate);
        RunNamedTest("TestStandardWeaponRefreshModel", TestStandardWeaponRefreshModel);
        RunNamedTest("TestStandardWeaponStockInitialization", TestStandardWeaponStockInitialization);
        RunNamedTest("TestStandardWeaponAttributeSynchronization", TestStandardWeaponAttributeSynchronization);
        RunNamedTest("TestKnifeRepeatedFieldRepair", TestKnifeRepeatedFieldRepair);
        RunNamedTest("TestWeaponMeshMaskNonblocking", TestWeaponMeshMaskNonblocking);
        RunNamedTest("TestImmutableCatalogSnapshot", TestImmutableCatalogSnapshot);
        RunNamedTest("TestWeaponAppearanceModule", TestWeaponAppearanceModule);
        RunNamedTest("TestM4A4ModernLegacyTransition", TestM4A4ModernLegacyTransition);
        RunNamedTest("TestDeferredRuntimeLogging", TestDeferredRuntimeLogging);
        std::cout << "nSkinzCS2 tests passed\n";
        return EXIT_SUCCESS;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "nSkinzCS2 test failure: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
}

void ParseArguments(const int argc, wchar_t** argv)
{
    for (int index = 1; index < argc; ++index)
    {
        const std::wstring_view argument(argv[index]);
        constexpr std::wstring_view prefix = L"--cs2-root=";
        if (argument.starts_with(prefix))
        {
            g_compatibility_cs2_root = std::wstring(argument.substr(prefix.size()));
        }
        else if (argument == L"--cs2-root" && index + 1 < argc)
        {
            g_compatibility_cs2_root = argv[++index];
        }
        else if (argument == L"--mesh-benchmark")
        {
            g_mesh_benchmark_only = true;
        }
        else if (argument == L"--knife-model-only")
        {
            g_knife_model_only = true;
        }
        else if (argument == L"--weapon-paint-only")
        {
            g_weapon_paint_only = true;
        }
    }
}

int wmain(const int argc, wchar_t** argv)
{
    ParseArguments(argc, argv);
    __try
    {
        return RunAllTests();
    }
    __except (HandleStructuredTestException(GetExceptionCode()))
    {
        return EXIT_FAILURE;
    }
}
