#include "Core/Config.hpp"
#include "Cs2/Catalog.hpp"
#include "Cs2/Runtime.hpp"
#include "Runtime/GloveAppearance.hpp"
#include "Runtime/KnifeAnimation.hpp"
#include "Runtime/KnifeAppearance.hpp"
#include "Runtime/WeaponAppearance.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace
{
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

bool ContainsDefinitionIndex(
    const std::vector<std::uint16_t>& definition_indices,
    const std::uint16_t definition_index)
{
    return std::find(definition_indices.begin(), definition_indices.end(), definition_index) != definition_indices.end();
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
    profile.paint_kit = 344;
    profile.quality = 12;
    profile.seed = 1337;
    profile.stat_trak = 42;
    profile.wear = 0.1337f;
    profile.custom_name = "movie prop";
    profile.stickers[0].kit_id = 3;
    profile.stickers[0].rotation = 90.0f;
    document.profiles.push_back(profile);
    document.experimental_allow_hud_alias_synthetic_proxy_model_write = true;

    Expect(nskinzcs2::ConfigStore::Save(temp_path, document), "config save should succeed");
    const auto loaded = nskinzcs2::ConfigStore::Load(temp_path);

    Expect(loaded.profiles.size() == 1, "config should contain one profile");
    Expect(loaded.profiles[0].name == "AK test", "profile name should round-trip");
    Expect(loaded.profiles[0].stickers[0].kit_id == 3, "sticker should round-trip");
    Expect(
        loaded.experimental_allow_hud_alias_synthetic_proxy_model_write,
        "experimental HUD proxy write toggle should round-trip");

    fs::remove(temp_path);
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

    Expect(loaded.version == 3, "glove config should round-trip version 3");
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
    Expect(loaded.version == 3, "legacy knife config should migrate to version 3");
    Expect(loaded.profiles.size() == 1, "legacy knife entries should be removed from weapon profiles");
    Expect(loaded.profiles[0].base_item == 7, "non-knife weapon entries should remain after migration");

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
    }
}
)kv";

    const auto catalog = nskinzcs2::CatalogBuilder::BuildCatalogFromGameText(items_game_text, localization_text);
    Expect(catalog.has_value(), "catalog should parse from in-memory game text");
    Expect(catalog->runtime_populated, "text-backed catalog should mark itself populated");
    Expect(catalog->weapon_paint_kits.size() == 2, "text-backed catalog should expose full parsed paint kits");

    const auto printstream_it = std::find_if(
        catalog->weapon_paint_kits.begin(),
        catalog->weapon_paint_kits.end(),
        [](const nskinzcs2::model::NamedId& item)
        {
            return item.id == 711;
        });
    Expect(printstream_it != catalog->weapon_paint_kits.end(), "in-memory parse should preserve weapon paint kits");
    Expect(printstream_it->name == "Printstream", "in-memory parse should preserve localized paint kit names");
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
    Expect(catalog->qualities.size() >= 5, "real installed game files should provide quality entries");
    std::cout
        << "real catalog counts: weapon paints=" << catalog->weapon_paint_kits.size()
        << ", glove paints=" << catalog->glove_paint_kits.size()
        << ", stickers=" << catalog->sticker_kits.size()
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
    using nskinzcs2::runtime::RuntimeRebuildReason;
    using nskinzcs2::runtime::ShouldAllowAttributeListVectorMutationForProfile;
    using nskinzcs2::runtime::ShouldAllowWeaponRefreshHelperDispatch;
    using nskinzcs2::runtime::ShouldConsumeKnifeRefreshHelperEpoch;
    using nskinzcs2::runtime::ShouldInstallRuntimeOwnedAttributeListForProfile;

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
        ShouldAllowWeaponRefreshHelperDispatch(false, KnifeRefreshHelperPolicy::Disabled, false),
        "standard weapon helper dispatch should stay enabled under the knife crash guard");
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
    }

    {
        nskinzcs2::model::CosmeticProfile weapon_profile;
        weapon_profile.base_item = 9;
        Expect(
            ShouldAllowAttributeListVectorMutationForProfile(weapon_profile),
            "standard weapons should keep attribute-list vector mutation enabled");
        Expect(
            ShouldInstallRuntimeOwnedAttributeListForProfile(weapon_profile),
            "standard weapons should keep runtime-owned attribute-list installs enabled");
        Expect(
            !ProbeKnifeRefreshAttachmentPromotionAllowedForTests(weapon_profile),
            "standard weapon refresh helper attachments should still restore immediately after helper dispatch");
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

void TestKnifeAppearanceModule()
{
    using nskinzcs2::runtime::BuildKnifeAppearanceRequest;
    using nskinzcs2::runtime::BuildKnifeProfileApplySignature;
    using nskinzcs2::runtime::EvaluateKnifeApplyGate;
    using nskinzcs2::runtime::EvaluateKnifeRenderVerification;
    using nskinzcs2::runtime::KnifeApplyGateInputs;
    using nskinzcs2::runtime::KnifeApplyGateReason;
    using nskinzcs2::runtime::KnifeFirstPersonOwnerKind;
    using nskinzcs2::runtime::KnifeProfileRequiresModelDefinition;
    using nskinzcs2::runtime::KnifeRenderVerificationInputs;
    using nskinzcs2::runtime::KnifeRenderVerificationReason;
    using nskinzcs2::runtime::ResolveKnifeSlotMode;
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
        !missing_first_person_gate.allow_finish_writes &&
            missing_first_person_gate.resolved_mode == KnifeSlotMode::None &&
            !missing_first_person_gate.allow_animation_remap &&
            missing_first_person_gate.reason == KnifeApplyGateReason::MissingFirstPersonBinding,
        "world-only knife evidence without a safe first-person owner should now fail closed");

    KnifeApplyGateInputs unsafe_finish_only_inputs = finish_inputs;
    unsafe_finish_only_inputs.context.watched_target = true;
    unsafe_finish_only_inputs.context.first_person_source_valid = false;
    unsafe_finish_only_inputs.context.providee_carrier_valid = false;
    unsafe_finish_only_inputs.context.viewmodel_carrier_valid = false;
    unsafe_finish_only_inputs.context.hud_proxy_fallback_allowed = false;
    unsafe_finish_only_inputs.context.direct_first_person_render_valid = false;
    const auto unsafe_finish_only_gate = EvaluateKnifeApplyGate(unsafe_finish_only_inputs);
    Expect(
        !unsafe_finish_only_gate.allow_finish_writes &&
            unsafe_finish_only_gate.resolved_mode == KnifeSlotMode::None &&
            unsafe_finish_only_gate.reason == KnifeApplyGateReason::MissingFirstPersonBinding,
        "finish-only watched knife paths should now fail closed even when watched-target ownership is present but no safe first-person evidence is available");

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
        !blocked_direct_render_gate.allow_finish_writes &&
            blocked_direct_render_gate.resolved_mode == KnifeSlotMode::None &&
            !blocked_direct_render_gate.allow_model_writes &&
            blocked_direct_render_gate.reason == KnifeApplyGateReason::MissingFirstPersonBinding,
        "direct-render-only knife evidence must now fail closed unless HUD fallback is explicitly allowed");

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
    using nskinzcs2::runtime::GloveCarrierKind;
    using nskinzcs2::runtime::EvaluateGloveCompatibility;
    using nskinzcs2::runtime::EvaluateGloveRenderVerification;
    using nskinzcs2::runtime::GloveCompatibilityReason;
    using nskinzcs2::runtime::GloveProfileRequiresModelDefinition;
    using nskinzcs2::runtime::GloveRenderVerificationReason;
    using nskinzcs2::runtime::GloveSlotMode;
    using nskinzcs2::runtime::ResolveCanonicalGloveCarrierKind;
    using nskinzcs2::runtime::ResolveGloveSlotMode;

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
        profile.quality = 3;
        profile.seed = 7;
        profile.wear = 0.15f;

        const auto request = BuildGloveAppearanceRequest(profile);
        Expect(request.xuid == profile.xuid, "glove request should preserve XUID");
        Expect(request.mode == GloveSlotMode::ModelAndFinish, "glove request should preserve mode");
        Expect(request.model_definition == 5030, "glove request should preserve model definition");
        Expect(request.paint_kit == 10018, "glove request should preserve paint");
        Expect(GloveProfileRequiresModelDefinition(request.mode), "model-and-finish mode requires a model definition");
    }

    Expect(
        ResolveCanonicalGloveCarrierKind(GloveSlotMode::FinishOnly, true, false) == GloveCarrierKind::None,
        "finish-only should not accept wearable-only resolution");
    Expect(
        ResolveCanonicalGloveCarrierKind(GloveSlotMode::FinishOnly, true, true) == GloveCarrierKind::PawnEconGloves,
        "finish-only should always resolve to pawn_econ_gloves when available");
    Expect(
        ResolveCanonicalGloveCarrierKind(GloveSlotMode::ModelAndFinish, true, true) == GloveCarrierKind::Wearable,
        "model-and-finish should resolve to the real wearable carrier when available");
    Expect(
        ResolveCanonicalGloveCarrierKind(GloveSlotMode::ModelAndFinish, false, true) ==
            GloveCarrierKind::PawnEconGlovesRenderFallback,
        "model-and-finish should fall back to the pawn glove slot when no wearable carrier exists");

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
        inputs.first_person_arms_model_state_resolved = true;
        inputs.first_person_arms_model_handle_resolved = true;
        inputs.first_person_arms_skeleton_identity_resolved = true;
        inputs.target_view_skeleton_identity_resolved = true;
        inputs.target_view_matches_first_person_skeleton = true;
        inputs.target_model_known = true;
        const auto compatibility = EvaluateGloveCompatibility(inputs);
        Expect(
            compatibility.compatible,
            "runtime-created model-and-finish should allow render binding to be verified after apply");
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
            "pawn glove render fallback should accept a fully resolved render binding");
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
            !compatibility.compatible &&
                compatibility.reason == GloveCompatibilityReason::MissingRenderBinding,
            "pawn glove render fallback should still require a live render binding before apply");
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
            "pawn glove render fallback should allow render binding to appear after apply");
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
            "pawn glove render fallback still needs a known target model before apply");
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

void TestStandardWeaponRefreshModel()
{
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

int RunAllTests()
{
    try
    {
        RunNamedTest("TestConfigRoundTrip", TestConfigRoundTrip);
        RunNamedTest("TestProfileMatching", TestProfileMatching);
        RunNamedTest("TestGloveConfigAndMatching", TestGloveConfigAndMatching);
        RunNamedTest("TestKnifeProfileConfigMigration", TestKnifeProfileConfigMigration);
        RunNamedTest("TestCatalogFallback", TestCatalogFallback);
        RunNamedTest("TestCatalogFromGameFiles", TestCatalogFromGameFiles);
        RunNamedTest("TestCatalogFromGameText", TestCatalogFromGameText);
        RunNamedTest("TestCatalogFromRealGameFilesIfProvided", TestCatalogFromRealGameFilesIfProvided);
        RunNamedTest("TestDuplicateDetection", TestDuplicateDetection);
        RunNamedTest("TestVersionCandidateFiltering", TestVersionCandidateFiltering);
        RunNamedTest("TestRuntimeRebuildDecision", TestRuntimeRebuildDecision);
        RunNamedTest("TestKnifeRefreshCrashGuardPolicy", TestKnifeRefreshCrashGuardPolicy);
        RunNamedTest("TestKnifeAnimationRemapTable", TestKnifeAnimationRemapTable);
        RunNamedTest("TestPreferredKnifeTargetXuid", TestPreferredKnifeTargetXuid);
        RunNamedTest("TestWatchedKnifeTargetResolutionFallback", TestWatchedKnifeTargetResolutionFallback);
        RunNamedTest("TestRuntimeGloveBindingChoice", TestRuntimeGloveBindingChoice);
    RunNamedTest("TestKnifeAppearanceModule", TestKnifeAppearanceModule);
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
        RunNamedTest("TestWeaponAppearanceModule", TestWeaponAppearanceModule);
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

int main()
{
    __try
    {
        return RunAllTests();
    }
    __except (HandleStructuredTestException(GetExceptionCode()))
    {
        return EXIT_FAILURE;
    }
}
