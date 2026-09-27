#include "Runtime/KnifeSerialization.hpp"
#include "Cs2/Compatibility.hpp"
#include "MinHook.h"
#include <Windows.h>
#include <intrin.h>
#include <algorithm>
#include <atomic>
#include <limits>

namespace nskinzcs2::runtime
{
namespace
{
constexpr std::uint8_t kCached[] {
    0x40,0x53,0x48,0x83,0xEC,0x30,0x48,0x8B,0x44,0x24,0x68,0x48,0x81,0xC1,0x20,0x01,0x01,0x00,
    0x48,0x89,0x44,0x24,0x28,0x48,0x8B,0xDA,0x48,0x8B,0x44,0x24,0x60,0x48,0x89,0x44,0x24,0x20,
    0xE8,0x17,0x00,0x00,0x00,0x48,0x8B,0xC3,0x48,0x83,0xC4,0x30,0x5B,0xC3 };
constexpr std::uint8_t kCaller[] {
    0x48,0x8B,0x0D,0xF3,0x81,0xA8,0x01,0x48,0x8D,0x44,0x24,0x70,0x48,0x89,0x7C,0x24,0x28,
    0x4C,0x8D,0x4C,0x24,0x40,0x4C,0x8D,0x45,0xA0,0x48,0x89,0x44,0x24,0x20,0x48,0x8D,0x55,0x20,
    0xE8,0x6A,0x06,0xA3,0xFF,0xEB,0x1C,0x4C,0x8D,0x4C,0x24,0x70,0x48,0x89,0x7C,0x24,0x20,
    0x4C,0x8D,0x44,0x24,0x40,0x48,0x8D,0x55,0xA0,0x48,0x8D,0x4D,0x20,0xE8,0xEC,0x88,0x9B,0x00 };
constexpr std::uint8_t kLocal[] {
    0x4C,0x89,0x4C,0x24,0x20,0x4C,0x89,0x44,0x24,0x18,0x48,0x89,0x54,0x24,0x10,
    0x53,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x83,0xEC,0x58 };
constexpr std::uint8_t kLookup[] {
    0x48,0x8B,0x0D,0xB9,0xEF,0x03,0x02,0x48,0x8B,0x01,0x4C,0x8B,0x40,0x58,0x8B,0x45,0xE4,
    0x0F,0xBA,0xE0,0x1E,0x73,0x06,0x48,0x8D,0x55,0xE8,0xEB,0x11,0xA9,0xFF,0xFF,0xFF,0x3F,
    0x48,0x8D,0x15,0x2F,0x4E,0x74,0x01,0x48,0x0F,0x47,0x55,0xE8,0x41,0xFF,0xD0,
    0x83,0xF8,0xFF,0x0F,0x84,0x62,0x06,0x00,0x00 };
constexpr std::uint8_t kMissing[] {
    0x4C,0x8D,0x4D,0xE0,0x48,0x8B,0xD3,0x4C,0x8D,0x45,0xE8,0x48,0x8B,0xCE,
    0xE8,0xC5,0x8D,0x01,0x00,0x4C,0x8B,0x6D,0xE0,0x48,0x8B,0x7D,0xE8,0x48,0x8B,0x77,0x08,0x48,0x63,0x07 };
const std::array<KnifeResourceEvidenceWindow, 5> kWindows {{
    {kKnifeCachedSerializationRva, kCached}, {0x92D7DE, kCaller},
    {kKnifeLocalSerializationRva, kLocal}, {0x377230, kLookup}, {0x35E3C8, kMissing} }};

// Published before MH_EnableHook; then immutable. The bridge holds no AppState,
// entity, graph, attribute, mutex or UI objects that can be destroyed on Stop.
std::uintptr_t g_client = 0;
void* g_engine = nullptr;
KnifeCachedSerializationFn g_cached = nullptr;
KnifeLocalSerializationFn g_local = nullptr;
std::atomic<std::uint32_t> g_modified {0};
std::atomic<std::uint64_t> g_local_count {0};

bool HasMissingModifiedSkeleton(const void* vector, std::uint32_t models) noexcept
{
    // Only read resource names/lookup rows. No object writes, allocation,
    // file I/O, graph reset, cache fabrication, or logging on animation jobs.
    __try
    {
        if (!vector || !g_engine) return false;
        const auto engine_table = *static_cast<const std::uintptr_t* const*>(g_engine);
        const auto playing = reinterpret_cast<bool(__fastcall*)(void*)>(
            engine_table[compatibility::kEngineIsPlayingDemoVtableIndex]);
        if (!playing(g_engine)) return false;
        const int count = *static_cast<const int*>(vector);
        if (count < 1 || count > 64) return false;
        const auto refs = *reinterpret_cast<const std::uintptr_t* const*>(
            static_cast<const std::uint8_t*>(vector) + 8);
        if (!refs) return false;
        for (int i = 0; i < count; ++i)
        {
            const auto resource = refs[i];
            if (!resource) continue;
            const auto descriptor = *reinterpret_cast<const std::uintptr_t*>(resource + 8);
            if (!descriptor) continue;
            const auto name = *reinterpret_cast<const char* const*>(descriptor);
            if (!name) continue;
            std::size_t length = 0;
            while (length < 128 && name[length]) ++length;
            if (length == 128 || !NeedsLocalKnifeSerialization(true, models, {name, length}, false)) continue;
            const auto table = *reinterpret_cast<void* const*>(g_client + kKnifeAnimAssetDataPointerRva);
            if (!table) return true;
            const auto methods = *static_cast<const std::uintptr_t* const*>(table);
            const auto find = reinterpret_cast<int(__fastcall*)(void*, const char*)>(methods[0x58 / 8]);
            if (find(table, name) == -1) return true;
        }
        return false;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void* __fastcall HookedSerialization(void* manager, void* output, const void* graph,
    const void* child_graphs, const void* animation_sets, const void* skeletons)
{
    const auto models = g_modified.load(std::memory_order_acquire);
    const bool local = models && reinterpret_cast<std::uintptr_t>(_ReturnAddress()) ==
        g_client + kKnifeSerializationCallerRva && HasMissingModifiedSkeleton(skeletons, models);
    if (local) g_local_count.fetch_add(1, std::memory_order_relaxed);
    // Native construction must complete normally. Do NOT swallow an exception
    // or return an unconstructed context to the engine.
    return DispatchKnifeSerialization(local, g_cached, g_local, manager, output,
        graph, child_graphs, animation_sets, skeletons);
}
}

const std::array<KnifeResourceEvidenceWindow, 5>& KnifeSerializationEvidenceWindows() noexcept { return kWindows; }

bool VerifyKnifeSerializationEvidence(std::uintptr_t client, std::size_t size, KnifeGraphReader& reader) noexcept
{
    if (client < 0x10000 || size > std::numeric_limits<std::uintptr_t>::max() - client ||
        size <= kKnifeAnimAssetDataPointerRva + sizeof(void*)) return false;
    for (const auto& window : kWindows)
    {
        std::array<std::uint8_t, 96> bytes {};
        if (window.rva >= size || window.bytes.size() > size - window.rva || window.bytes.size() > bytes.size() ||
            !reader.Read(client + window.rva, bytes.data(), window.bytes.size()) ||
            !std::equal(window.bytes.begin(), window.bytes.end(), bytes.begin())) return false;
    }
    return true;
}

std::uint32_t KnifeSerializationModelBit(int definition) noexcept
{
    const auto& assets = KnifeModelAssets();
    for (std::size_t i = 0; i < assets.size(); ++i)
        if (assets[i].definition == definition) return 1u << i;
    return 0;
}

bool NeedsLocalKnifeSerialization(bool offline, std::uint32_t modified,
    std::string_view path, bool recorded_asset_available) noexcept
{
    if (!offline || !modified || recorded_asset_available) return false;
    const auto& assets = KnifeModelAssets();
    for (std::size_t i = 0; i < assets.size(); ++i)
        if ((modified & (1u << i)) && assets[i].skeleton == path) return true;
    return false;
}

void* DispatchKnifeSerialization(bool local, KnifeCachedSerializationFn cached, KnifeLocalSerializationFn native,
    void* manager, void* output, const void* graph, const void* children, const void* sets, const void* skeletons)
{
    // Exact argument shift from adjacent branches at client+8FE43A..8FE480.
    return local ? native(output, graph, children, sets, skeletons)
                 : cached(manager, output, graph, children, sets, skeletons);
}

bool ArmKnifeSerializationBridge(std::uintptr_t client, std::size_t size, void* engine,
    int definition, KnifeGraphReader& reader) noexcept
{
    const auto bit = KnifeSerializationModelBit(definition);
    if (!bit || !engine) return false;
    if (!g_cached)
    {
        if (!VerifyKnifeSerializationEvidence(client, size, reader)) return false;
        const auto initialized = MH_Initialize();
        if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED) return false;
        // A model may outlive the UI/runtime owner and be consumed by async
        // animation jobs. Keep this stateless bridge + code mapped until CS2
        // exits, rather than unhooking underneath a still-modified skeleton.
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(&HookedSerialization), &pinned)) return false;
        void* original = nullptr;
        const auto target = reinterpret_cast<void*>(client + kKnifeCachedSerializationRva);
        if (MH_CreateHook(target, reinterpret_cast<void*>(&HookedSerialization), &original) != MH_OK) return false;
        g_client = client;
        g_engine = engine;
        g_local = reinterpret_cast<KnifeLocalSerializationFn>(client + kKnifeLocalSerializationRva);
        g_cached = reinterpret_cast<KnifeCachedSerializationFn>(original);
        if (MH_EnableHook(target) != MH_OK)
        {
            MH_RemoveHook(target);
            g_cached = nullptr;
            return false;
        }
    }
    if (client != g_client || engine != g_engine) return false;
    // Retain bits across Update/seek: old models can still be referenced by a
    // queued job. A complete recorded row always takes the original path.
    g_modified.fetch_or(bit, std::memory_order_release);
    return true;
}

std::uint64_t KnifeLocalSerializationCount() noexcept { return g_local_count.load(std::memory_order_relaxed); }
}
