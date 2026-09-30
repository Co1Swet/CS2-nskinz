#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nskinzcs2::compatibility
{
inline constexpr std::string_view kVerifiedClientVersion = "2000919";
inline constexpr std::string_view kVerifiedPatchVersion = "1.41.8.6";
inline constexpr std::string_view kVerifiedSourceRevision = "11049427";
inline constexpr std::string_view kVerifiedHlaeVersion = "2.192.6.0";

inline constexpr char kSource2ClientInterface[] = "Source2Client002";
inline constexpr char kResourceSystemInterface[] = "ResourceSystem013";
inline constexpr char kFileSystemInterface[] = "VFileSystem017";
inline constexpr std::size_t kFrameStageNotifyVtableIndex = 36;
// ClientFrameStage_t layout re-verified with HLAE 2.192.6. Runtime polling must run at
// FRAME_RENDER_PASS; using the previous value (11) leaves every manual Update
// pending forever even though FrameStageNotify callbacks are active.
inline constexpr int kFrameStartStage = 0;
inline constexpr int kFrameRenderPassStage = 12;
// Source2EngineToClient001 layout verified on HLAE 2.192.6 / CS2 build 2000918.
inline constexpr std::size_t kEngineIsPlayingDemoVtableIndex = 42;
inline constexpr std::size_t kEngineGetDemoFilePathVtableIndex = 43;
inline constexpr std::size_t kEngineGetLevelNameShortVtableIndex = 65;
inline constexpr std::size_t kEngineGetDemoFileVtableIndex = 69;
inline constexpr std::size_t kEngineExecuteClientCmdVtableIndex = 51;
inline constexpr std::size_t kDemoFileGetTickVtableIndex = 3;
inline constexpr std::size_t kDemoFileIsPlayingVtableIndex = 11;
inline constexpr std::size_t kDemoFileIsPausedVtableIndex = 12;

inline constexpr char kSchemaGlobalPattern[] =
    "48 89 05 ?? ?? ?? ?? 4C 8D 0D ?? ?? ?? ?? 33 C0 48 C7 05";
inline constexpr char kEntitySystemRootPattern[] =
    "40 55 53 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 33 D2 E8 ?? ?? ?? ??";
inline constexpr char kGetEntityFromIndexPattern[] =
    "4C 8D 49 10 81 FA FE 7F 00 00";
// Build 2000918 CUtlVector<CEconItemAttribute>::AddToTail(const T&).  This is
// the engine-owned construction path used when a stock demo weapon has no
// paint/seed/wear records and Schema does not expose a network-vector callback.
// Runtime additionally validates the complete fixed instruction window and
// the reflected 0x48/0x30/0x34/0x38 element layout before calling it.
inline constexpr char kEconItemAttributeAppendPattern[] =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 20 4C 63 31 48 8B EA 48 8B F9 44 3B 71 10 0F 85 ?? ?? ?? ?? F7 41 14 00 00 00 40";
inline constexpr char kAttributeHelperPattern[] =
    "48 83 EC ? E8 ? ? ? ? 48 85 C0 0F 84 ? ? ? ? 48 8B 10";
inline constexpr char kResourceSystemQueryPattern[] =
    "48 83 EC 38 41 B1 01 C6 44 24 20 01 E8 ? ? ? ? 48 83 C4 38 C3";
inline constexpr char kBindModelPattern[] =
    "48 8B C4 53 57 48 83 EC 68 48 89 70 18 41 0F B6 D8 4C 89 70 20 48 8B F9 4C 89 78 E8 4C 8B F2 48 8B 41 30 45 8B F9 48 85 C0 74 33";
// Current build 2000918 no longer exposes a callable create function in the
// fd26f92 ClientClass chain. These four signatures identify one internally
// cross-referenced CGameEntitySystem creation chain; model_and_finish remains
// fail-closed unless every member is unique and its call targets agree.
inline constexpr char kWearableEntityFactoryCallsitePattern[] =
    "48 8B 0D ?? ?? ?? ?? 45 33 C9 C6 44 24 30 01 4C 8B C3 C7 44 24 28 FF FF FF FF 41 8B D6 C7 44 24 20 FF FF FF FF E8 ?? ?? ?? ??";
inline constexpr char kWearableEntityCreateByNamePattern[] =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 50 49 8B F8 44 8B F2 33 DB 4C 8D 44 24 40";
inline constexpr char kWearableEntityFormalCreatePattern[] =
    "48 89 5C 24 08 48 89 6C 24 10 56 57 41 56 48 83 EC 40 4D 8B F1 49 8B D8 8B EA 48 8B F1 4D 85 C0";
inline constexpr char kWearableEntityClassLookupPattern[] =
    "48 89 5C 24 08 48 89 74 24 18 48 89 54 24 10 57 48 83 EC 20 49 8B D8 48 8B F9 48 85 D2";
// CEconWearable's current client-think override calls the base no-op and then
// tail-calls the definition/model refresh. Runtime resolves it from the
// reflected primary vtable and validates both relative targets immediately
// before invocation; this offline signature makes a changed thunk fail the
// installed-build compatibility scan before the DLL is used in CS2.
inline constexpr char kWearableDefinitionRefreshThunkPattern[] =
    "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 8B CB 48 83 C4 20 5B E9 ?? ?? ?? ??";
// C_BaseModelEntity::SetModel in the client module.  The current wrapper
// interns the requested path through the client string service and then calls
// the adjacent model-state transition.  Runtime-created CEconWearable objects
// do not receive the network creation path which normally performs this step,
// so model_and_finish uses this uniquely verified wrapper after definition and
// owner binding have succeeded.
inline constexpr char kBaseModelEntitySetModelPattern[] =
    "40 53 48 83 EC ?? 48 8B D9 4C 8B C2 48 8B 0D ?? ?? ?? ?? 48 8D 54 24 ?? 48 8B 01 FF 50 ?? 48 8B 54 24 ?? 48 8B CB E8 ?? ?? ?? ?? 48 83 C4 ?? 5B C3";
// C_BaseEntity::OnSubclassIDChanged.  Knife item definitions alone do not
// replace the entity's weapon VData: this callback resolves m_nSubclassID into
// the adjacent VData pointer before the first native deploy/HUD build. Runtime
// additionally verifies that the embedded VData displacement equals the
// Schema-resolved m_nSubclassID offset + 8 before invoking it.
inline constexpr char kBaseEntitySubclassChangedPattern[] =
    "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 83 BB ?? ?? ?? ?? 00 74 ?? 48 8B 03 BA 01 00 00 00 48 8B CB 48 83 C4 20 5B 48 FF A0 40 06 00 00 48 83 C4 20 5B C3";
// CUtlStringToken::Make returns its four-byte value through a hidden result
// pointer on x64 (RCX=result, RDX=text), despite the small logical return
// type. Calling it as uint64_t(const char*) writes through the class-name
// pointer and faults before a knife can be pre-armed. Keep the installed ABI
// prologue in the compatibility manifest and cross-check the export against
// this exact entry before any native subclass transition.
inline constexpr char kUtlStringTokenMakeSretPattern[] =
    "40 53 48 83 EC 20 C7 01 00 00 00 00 48 8B C2 48 8B D9 48 85 D2";
// Optional map-remap evidence synchronized from MulNX_CS2 upstream
// f2466e18e26a8fc3daf4ac7945bc45220ba95993.  Relative protobuf parser
// branches are intentionally masked; the following field-10 branch keeps the
// shared CSVCMsg_GameSessionConfiguration string exit unique on build 2000918.
// MapRemap independently fails closed when this evidence is absent, so it is
// reference-only here and does not disable firearm/knife/glove functionality.
inline constexpr char kMapRemapGameSessionParserPattern[] =
    "48 8B D8 48 85 C0 0F 85 ?? ?? ?? ?? E9 ?? ?? ?? ?? 40 80 FF 52 0F 85 ?? ?? ?? ?? 83 4E";
// Reference-only high-level model-provider binding entry for build 2000918.
// m_pGameSceneNode is the scene outer: its current vtable slot 10 owns the
// direct call and passes that object in R8, its embedded CSkeletonInstance at
// outer+0x140 in RCX, and outer+0x10 in R9. Runtime model initialization remains
// fail-closed unless this caller relationship, the scene owner, provider
// vtable/model source, and empty Wearable model state all validate. This
// reference signature does not gate compatibility.
// kBindModelPattern itself is the lower six-argument render-instance factory
// and must never be called as a two-argument CModelService setter.
inline constexpr char kSkeletonInstanceBindModelPattern[] =
    "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 50 48 8B F9 49 8B E9 49 8B C8 49 8B D8 48 8B F2 E8 ?? ?? ?? ?? 4C 8B 50 10";
// Compatibility alias retained only for offline reference diagnostics.
inline constexpr const char* kModelStateSetupModelPattern =
    kSkeletonInstanceBindModelPattern;

struct SignatureSpec
{
    const char* name = nullptr;
    const wchar_t* module_relative_path = nullptr;
    const char* ida_pattern = nullptr;
    bool required = true;
};

inline constexpr std::array<SignatureSpec, 16> kSignatureSpecs {{
    {"schema_global", L"game\\bin\\win64\\schemasystem.dll", kSchemaGlobalPattern, true},
    {"entity_system_root", L"game\\csgo\\bin\\win64\\client.dll", kEntitySystemRootPattern, true},
    {"get_entity_from_index", L"game\\csgo\\bin\\win64\\client.dll", kGetEntityFromIndexPattern, true},
    {"attribute_helper", L"game\\csgo\\bin\\win64\\client.dll", kAttributeHelperPattern, true},
    {"resource_system_query", L"game\\bin\\win64\\resourcesystem.dll", kResourceSystemQueryPattern, true},
    {"bind_model", L"game\\csgo\\bin\\win64\\client.dll", kBindModelPattern, true},
    {"skeleton_instance_bind_model_reference", L"game\\csgo\\bin\\win64\\client.dll", kSkeletonInstanceBindModelPattern, false},
    {"wearable_entity_factory_callsite", L"game\\csgo\\bin\\win64\\client.dll", kWearableEntityFactoryCallsitePattern, true},
    {"wearable_entity_create_by_name", L"game\\csgo\\bin\\win64\\client.dll", kWearableEntityCreateByNamePattern, true},
    {"wearable_entity_formal_create", L"game\\csgo\\bin\\win64\\client.dll", kWearableEntityFormalCreatePattern, true},
    {"wearable_entity_class_lookup", L"game\\csgo\\bin\\win64\\client.dll", kWearableEntityClassLookupPattern, true},
    {"wearable_definition_refresh_thunk", L"game\\csgo\\bin\\win64\\client.dll", kWearableDefinitionRefreshThunkPattern, true},
    {"base_model_entity_set_model", L"game\\csgo\\bin\\win64\\client.dll", kBaseModelEntitySetModelPattern, true},
    {"base_entity_subclass_changed", L"game\\csgo\\bin\\win64\\client.dll", kBaseEntitySubclassChangedPattern, true},
    {"utl_string_token_make_sret", L"game\\bin\\win64\\tier0.dll", kUtlStringTokenMakeSretPattern, true},
    {"map_remap_game_session_parser", L"game\\bin\\win64\\engine2.dll", kMapRemapGameSessionParserPattern, false},
}};

struct SignatureResult
{
    std::string name;
    std::filesystem::path module_path;
    bool required = true;
    bool module_read = false;
    std::size_t match_count = 0;
    std::uint32_t pe_timestamp = 0;
    std::uintmax_t file_size = 0;
    std::wstring issue;
};

struct InstalledReport
{
    std::filesystem::path install_root;
    std::string client_version;
    std::string server_version;
    std::string patch_version;
    std::string source_revision;
    std::string version_date;
    std::string version_time;
    std::vector<SignatureResult> signatures;

    bool RequiredSignaturesUnique() const noexcept;
};

std::size_t CountPatternMatches(std::span<const std::uint8_t> bytes, std::string_view ida_pattern);
std::filesystem::path FindCs2InstallRoot(const std::filesystem::path& starting_path);
InstalledReport ScanInstalledCs2(const std::filesystem::path& install_root);
std::vector<std::wstring> BuildRuntimeCompatibilityDiagnostics();
bool IsHlae21911InterfaceCombination(
    std::string_view source2_client,
    std::string_view resource_system,
    std::string_view file_system) noexcept;
}
