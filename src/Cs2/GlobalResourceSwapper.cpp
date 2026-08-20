#include "Cs2/GlobalResourceSwapper.hpp"

#include "Core/Log.hpp"
#include "MinHook.h"

#include <Windows.h>
#include <intrin.h>
#include <Psapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace
{
using GenericSlotFn = std::uint64_t(__fastcall*)(void* this_ptr, std::uint64_t arg2, std::uint64_t arg3, std::uint64_t arg4);
using QueryFn = GenericSlotFn;

constexpr std::size_t kInvalidSlot = static_cast<std::size_t>(-1);
constexpr std::size_t kSlotBegin = 8;
constexpr std::size_t kSlotEndInclusive = 48;
constexpr std::size_t kMachineByteCount = 32;
// Historical diagnostic hint only. Live resource-query provenance is the
// compatibility gate because this RVA changes between CS2 builds.
constexpr std::uint64_t kLegacyEngine2PrimaryCallerOffset = 0x2B8Cull;
constexpr std::uint32_t kEngine2PrimaryDispatchSlotOffset = 0x40u;
constexpr std::uint32_t kEngine2WrapperDispatchSlotOffset = 0x2B8u;
constexpr std::size_t kEngine2ProbeStubStackSize = 0x108;
constexpr std::size_t kEngine2ProbeStubSize = 333;
constexpr std::size_t kEngine2ProbeHelperAddressOffset = 168;
constexpr std::size_t kEngine2ProbeOriginalAddressOffset = 323;
constexpr std::size_t kEngine2CallerWindowBackRange = 0x180;
constexpr std::size_t kEngine2CallerWindowForwardRange = 0x80;
constexpr std::size_t kEngine2CallerWindowContextBytes = 32;
constexpr std::size_t kEngine2CallerWindowMaxEntries = 24;
constexpr std::size_t kEngine2CallerRcxBackscanBytes = 0x30;
constexpr std::uint32_t kEngine2UpstreamProbeMaxEntryCaptures = 64;
constexpr std::size_t kEngine2PrimaryReturnBucketCapacity = 32;
constexpr std::size_t kEngine2PrimaryCallerCandidateCapacity = 16;
constexpr std::size_t kEngine2PrimaryAncestryFrameCapacity = 6;
constexpr std::size_t kAnimationSlot120CallerCandidateCapacity = 8;
constexpr std::uint64_t kEngine2UpstreamColdSlot68Threshold = 192;
constexpr std::uintptr_t kMinUserModePointer = 0x10000ull;
constexpr std::uintptr_t kMaxUserModePointer = 0x00007FFFFFFFFFFFull;
constexpr std::size_t kFileSystemFirstSlotOffset = 0x20u;
constexpr std::size_t kFileSystemLastSlotOffset = 0x180u;
constexpr std::size_t kFileSystemFirstSlotIndex = kFileSystemFirstSlotOffset / sizeof(void*);
constexpr std::size_t kFileSystemLastSlotIndex = kFileSystemLastSlotOffset / sizeof(void*);
constexpr std::size_t kFileSystemKnownHotSlotOffset = 0x78u;
constexpr std::size_t kFileSystemKnownHotSlotIndex = kFileSystemKnownHotSlotOffset / sizeof(void*);
constexpr std::uint32_t kFileSystemProbeMaxCallsPerSlot = 32;
constexpr std::uint64_t kFileSystemProbeIdleTimeoutMs = 250;
constexpr std::uint64_t kFileSystemProbeActiveTimeoutMs = 1000;
constexpr std::size_t kFileSystemAmbushPathCapacity = 512;
constexpr std::size_t kFileSystemAmbushFrameCapacity = 32;
constexpr std::uint32_t kFileSystemCarrierProbeMaxAttempts = 8;
constexpr std::size_t kFileSystemCarrierProbeMaxSamples = 8;
constexpr std::size_t kFileSystemCarrierProbeLabelCapacity = 64;
constexpr std::size_t kFileSystemCarrierProbeSampleCapacity = 160;
constexpr std::array<std::size_t, 13> kCarrierProbeOffsets {{
    0x00,
    0x08,
    0x10,
    0x18,
    0x20,
    0x28,
    0x30,
    0x38,
    0x40,
    0x48,
    0x50,
    0x58,
    0x60,
}};
constexpr char kGlobalResourceSwapperBuildStamp[] = __DATE__ " " __TIME__;

#pragma intrinsic(_ReturnAddress)

struct ModuleSpan
{
    std::uint8_t* base = nullptr;
    std::size_t size = 0;
};

struct OwnerSummary
{
    bool available = false;
    std::wstring label;
    std::wstring dominant_module;
    std::size_t dominant_count = 0;
    std::size_t slot_count = 0;
};

struct CallerInfo
{
    const void* return_address = nullptr;
    std::wstring return_owner;
    void* caller_base = nullptr;
    std::wstring caller_owner;
    std::uint64_t caller_offset = 0;
    const void* before_return = nullptr;
    std::uint32_t stack_depth = 0;
};

struct Engine2EntryCapture
{
    std::uint64_t rax = 0;
    std::uint64_t rcx = 0;
    std::uint64_t rdx = 0;
    std::uint64_t r8 = 0;
    std::uint64_t r9 = 0;
    std::uint64_t r10 = 0;
    std::uint64_t r11 = 0;
    std::uint64_t original_rsp = 0;
    std::uint64_t rbx = 0;
    std::uint64_t rbp = 0;
    std::uint64_t rsi = 0;
    std::uint64_t rdi = 0;
    std::uint64_t r12 = 0;
    std::uint64_t r13 = 0;
    std::uint64_t r14 = 0;
    std::uint64_t r15 = 0;
};

struct Engine2WrapperDispatchSnapshot
{
    const void* rcx_object = nullptr;
    void* emulated_rax_after_mov = nullptr;
    void* slot_entry_address = nullptr;
    void* slot_target = nullptr;
    void* followed_target = nullptr;
    void* base_target = nullptr;
    bool direct_executable = false;
    bool followed_executable = false;
    bool base_executable = false;
};

struct SiblingProbeSample
{
    const void* this_ptr = nullptr;
    const void* arg2 = nullptr;
    const void* arg3 = nullptr;
    const void* arg4 = nullptr;
};

struct SiblingHookDescriptor
{
    std::uint32_t slot_offset = 0;
    const wchar_t* owner_module = nullptr;
    const char* log_tag = nullptr;
    std::uint32_t active_bit = 0;
};

struct SiblingHookRuntime
{
    void* pending_target = nullptr;
    void* target = nullptr;
    GenericSlotFn original = nullptr;
    bool active = false;
    std::uint32_t sample_count = 0;
    std::array<SiblingProbeSample, 3> samples {};
};

struct Engine2PrimaryReturnBucket
{
    const void* return_address = nullptr;
    void* caller_base = nullptr;
    std::uint64_t caller_offset = 0;
    std::uint32_t stack_depth = 0;
    std::uint32_t hits = 0;
};

struct Engine2PrimaryCallerCandidate
{
    void* caller_base = nullptr;
    const void* sample_return_address = nullptr;
    std::uint64_t caller_offset = 0;
    std::uint32_t sample_depth = 0;
    std::uint32_t hits = 0;
    bool queued = false;
    bool attempted = false;
    bool success = false;
};

struct FileSystemAmbushCapture
{
    DWORD thread_id = 0;
    std::size_t slot = kInvalidSlot;
    bool wide = false;
    char ansi_path[kFileSystemAmbushPathCapacity] {};
    wchar_t wide_path[kFileSystemAmbushPathCapacity] {};
    std::uint32_t frame_count = 0;
    std::array<const void*, kFileSystemAmbushFrameCapacity> frames {};
};

struct FileSystemCarrierProbeSample
{
    char label[kFileSystemCarrierProbeLabelCapacity] {};
    const void* pointer = nullptr;
    bool wide = false;
    bool resource_like = false;
    char ansi[kFileSystemCarrierProbeSampleCapacity] {};
    wchar_t wide_text[kFileSystemCarrierProbeSampleCapacity] {};
};

struct FileSystemCarrierProbeCapture
{
    DWORD thread_id = 0;
    std::size_t slot = kInvalidSlot;
    std::uint32_t attempt = 0;
    std::uint64_t entered = 0;
    const void* return_address = nullptr;
    std::uint64_t arg2 = 0;
    std::uint64_t arg3 = 0;
    std::uint64_t arg4 = 0;
    bool exhausted_without_sample = false;
    std::uint32_t sample_count = 0;
    std::array<FileSystemCarrierProbeSample, kFileSystemCarrierProbeMaxSamples> samples {};
};

template <typename T>
bool SafeReadValue(const void* address, T& out) noexcept;

bool IsLikelyUserModePointer(const void* address) noexcept;
bool TryAddPointerOffset(
    const void* base,
    const std::uintptr_t offset,
    const void*& out) noexcept;
bool IsExecutableAddress(const void* address) noexcept;
bool IsReadableAddress(const void* address) noexcept;
bool IsWritableAddress(const void* address) noexcept;
void* FollowCodePointer(void* address) noexcept;
void* ResolveFunctionBase(void* address) noexcept;
bool TryCaptureEngine2UpstreamWrapperDispatchSnapshot(
    const void* object,
    Engine2WrapperDispatchSnapshot& out) noexcept;
bool TryCaptureEngine2UpstreamWrapperDispatchSnapshot(
    const Engine2EntryCapture* capture,
    Engine2WrapperDispatchSnapshot& out) noexcept;
bool TryCaptureEngine2PrimaryDispatchSnapshot(
    const Engine2EntryCapture* capture,
    Engine2WrapperDispatchSnapshot& out) noexcept;
bool TryCaptureAsciiSample(const char* address, char* out, const std::size_t out_size) noexcept;
bool TryCaptureWideSample(const wchar_t* address, wchar_t* out, const std::size_t out_size) noexcept;
bool TryBuildUnwindContextFromEntryCapture(
    const Engine2EntryCapture* capture,
    const void* control_pc,
    CONTEXT& out_context) noexcept;
bool TryStepUnwindContext(CONTEXT& context) noexcept;
bool TryValidateFileSystemSlotTarget(void* iface, const std::size_t slot, void*& out_target) noexcept;
bool TryActivateNextFileSystemProbeLocked(std::wstring& reason) noexcept;
void DeactivateFileSystemProbeLocked() noexcept;
bool TryCaptureFileSystemAmbushFromValue(std::uint64_t value) noexcept;
void TryCaptureFileSystemCarrierProbe(
    void* this_ptr,
    std::uint64_t arg2,
    std::uint64_t arg3,
    std::uint64_t arg4,
    const void* return_address) noexcept;
void TryAdvanceFileSystemProbe() noexcept;
void TryFlushFileSystemAmbushCapture() noexcept;
void TryFlushFileSystemCarrierProbe() noexcept;
std::size_t CollectEngine2PrimaryCallerAncestry(
    const Engine2EntryCapture* capture,
    const void* control_pc,
    std::array<CallerInfo, kEngine2PrimaryAncestryFrameCapacity>& out_infos) noexcept;
bool TryBuildCallerInfoFromCandidate(
    const Engine2PrimaryCallerCandidate& candidate,
    CallerInfo& out_info) noexcept;
bool TryQueueEngine2UpstreamCallerProbe(
    const CallerInfo& info,
    const void* live_object,
    void* live_vtable) noexcept;
void TryLogQueryCarrierPathSampleOnce(const char* label, const char* path) noexcept;
void TryActivatePendingSiblingHooks() noexcept;
bool TryQueueNextEngine2UpstreamCallerCandidate() noexcept;
void TryMarkColdEngine2UpstreamCallerProbeForRemoval() noexcept;
void TryCaptureSiblingProbeSample(
    std::size_t descriptor_index,
    void* this_ptr,
    std::uint64_t arg2,
    std::uint64_t arg3,
    std::uint64_t arg4) noexcept;
void TryTrackAnimationSlot120CallerSource(const CallerInfo& info) noexcept;
bool TryQueueNextAnimationSlot120CallerCandidate() noexcept;
void TryActivateDeferredAnimationSlot120CallerProbe() noexcept;
void TryDeactivateAnimationSlot120CallerProbe() noexcept;
void TryLogAnimationSlot120CallerProbeWindow(const Engine2EntryCapture* capture) noexcept;
void UpdateThreadLocalEngine2PrimaryCapture(const Engine2EntryCapture* capture) noexcept;
void TryCorrelateAnimationSlot120WithPrimaryCapture(const CallerInfo& info) noexcept;
void TryLogAnimationCarrierProbeWindow(
    void* this_ptr,
    std::uint64_t arg2,
    std::uint64_t arg3,
    std::uint64_t arg4) noexcept;
std::uint64_t __fastcall hkFileSystemSlotProbe(
    void* this_ptr,
    std::uint64_t arg2,
    std::uint64_t arg3,
    std::uint64_t arg4) noexcept;
std::uint64_t __fastcall hkResourceSystemSlot68(
    void* this_ptr,
    std::uint64_t arg2,
    std::uint64_t arg3,
    std::uint64_t arg4) noexcept;
std::uint64_t __fastcall hkSiblingProbe248(
    void* this_ptr,
    std::uint64_t arg2,
    std::uint64_t arg3,
    std::uint64_t arg4) noexcept;
std::uint64_t __fastcall hkSiblingProbe0A0(
    void* this_ptr,
    std::uint64_t arg2,
    std::uint64_t arg3,
    std::uint64_t arg4) noexcept;
std::uint64_t __fastcall hkSiblingProbe120(
    void* this_ptr,
    std::uint64_t arg2,
    std::uint64_t arg3,
    std::uint64_t arg4) noexcept;
bool IsNoisyEngine2UpstreamOwner(const std::wstring& owner) noexcept;

struct VmtPatch
{
    void** slot = nullptr;
    void* original = nullptr;
    bool installed = false;

    bool Install(void* iface, const std::size_t index, void* detour) noexcept
    {
        if (installed || iface == nullptr || detour == nullptr)
        {
            return false;
        }

        void** vtable = nullptr;
        if (!SafeReadValue(iface, vtable) || vtable == nullptr)
        {
            return false;
        }

        slot = vtable + index;
        DWORD old_protect = 0;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old_protect))
        {
            slot = nullptr;
            return false;
        }

        original = *slot;
        *slot = detour;
        FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));

        DWORD ignored = 0;
        VirtualProtect(slot, sizeof(void*), old_protect, &ignored);
        installed = true;
        return true;
    }

    void Remove() noexcept
    {
        if (!installed || slot == nullptr)
        {
            return;
        }

        DWORD old_protect = 0;
        if (VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old_protect))
        {
            *slot = original;
            FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));

            DWORD ignored = 0;
            VirtualProtect(slot, sizeof(void*), old_protect, &ignored);
        }

        slot = nullptr;
        original = nullptr;
        installed = false;
    }
};

enum class Route
{
    None,
    FileSystemAmbush,
    ResourceSystemSlot68,
    ResourceSystemVtableQuery,
    ResourceSystemQueryBody,
};

enum class FileSystemCaptureState : std::uint32_t
{
    Inactive = 0,
    Armed = 1,
    Captured = 2,
    Miss = 3,
};

enum class Engine2UpstreamAnalysisState
{
    None,
    Queued,
    Begin,
    SkippedNonModelOwner,
    AwaitingModelOwner,
    ModuleMismatch,
    TextMissing,
    NoDirectCallers,
    VcallObjectMissing,
    VcallTargetMissing,
    VcallTargetInvalid,
    VcallTargetReady,
    ModelOwnerReady,
    XrefReady,
    Exception,
};

const wchar_t* Engine2UpstreamAnalysisStateText(const Engine2UpstreamAnalysisState state) noexcept
{
    switch (state)
    {
    case Engine2UpstreamAnalysisState::Queued:
        return L"QUEUED";
    case Engine2UpstreamAnalysisState::Begin:
        return L"BEGIN";
    case Engine2UpstreamAnalysisState::SkippedNonModelOwner:
        return L"SKIPPED_NON_MODEL_OWNER";
    case Engine2UpstreamAnalysisState::AwaitingModelOwner:
        return L"AWAITING_MODEL_OWNER";
    case Engine2UpstreamAnalysisState::ModuleMismatch:
        return L"MODULE_MISMATCH";
    case Engine2UpstreamAnalysisState::TextMissing:
        return L"TEXT_MISSING";
    case Engine2UpstreamAnalysisState::NoDirectCallers:
        return L"NO_DIRECT_E8";
    case Engine2UpstreamAnalysisState::VcallObjectMissing:
        return L"VCALL_OBJECT_MISSING";
    case Engine2UpstreamAnalysisState::VcallTargetMissing:
        return L"VCALL_TARGET_MISSING";
    case Engine2UpstreamAnalysisState::VcallTargetInvalid:
        return L"VCALL_TARGET_INVALID";
    case Engine2UpstreamAnalysisState::VcallTargetReady:
        return L"VCALL_TARGET_READY";
    case Engine2UpstreamAnalysisState::ModelOwnerReady:
        return L"MODEL_OWNER_READY";
    case Engine2UpstreamAnalysisState::XrefReady:
        return L"XREF_READY";
    case Engine2UpstreamAnalysisState::Exception:
        return L"EXCEPTION";
    case Engine2UpstreamAnalysisState::None:
    default:
        return L"NONE";
    }
}

const wchar_t* FileSystemCaptureStateText(const FileSystemCaptureState state) noexcept
{
    switch (state)
    {
    case FileSystemCaptureState::Armed:
        return L"ARMED";
    case FileSystemCaptureState::Captured:
        return L"CAPTURED";
    case FileSystemCaptureState::Miss:
        return L"MISS";
    case FileSystemCaptureState::Inactive:
    default:
        return L"INACTIVE";
    }
}

struct State
{
    void* file_system_interface = nullptr;
    void* signature_match = nullptr;
    void* target = nullptr;
    QueryFn original = nullptr;
    void* engine2_probe_target = nullptr;
    void* engine2_probe_original = nullptr;
    void* engine2_probe_stub = nullptr;
    void* engine2_upstream_probe_target = nullptr;
    void* engine2_upstream_probe_original = nullptr;
    void* engine2_upstream_probe_stub = nullptr;
    void* animation_slot120_probe_target = nullptr;
    void* animation_slot120_probe_original = nullptr;
    void* animation_slot120_probe_stub = nullptr;
    const void* engine2_upstream_live_object = nullptr;
    std::array<const void*, 8> engine2_upstream_live_registers {};
    void* engine2_upstream_live_vtable = nullptr;
    void* engine2_upstream_live_entry_address = nullptr;
    void* engine2_upstream_live_target = nullptr;
    void* engine2_upstream_live_target_base = nullptr;
    const void* engine2_upstream_last_rejected_object = nullptr;
    void* engine2_upstream_last_rejected_vtable = nullptr;
    void* engine2_upstream_last_rejected_target = nullptr;
    void* engine2_upstream_last_rejected_target_base = nullptr;
    void* engine2_upstream_last_rejected_caller = nullptr;
    const void* engine2_upstream_probe_pending_return = nullptr;
    void* engine2_upstream_probe_pending_target = nullptr;
    std::uint64_t engine2_upstream_probe_pending_offset = 0;
    const void* animation_slot120_probe_pending_return = nullptr;
    void* animation_slot120_probe_pending_target = nullptr;
    std::uint64_t animation_slot120_probe_pending_offset = 0;
    std::size_t slot = kInvalidSlot;
    std::size_t file_system_next_slot = kInvalidSlot;
    Route route = Route::None;
    VmtPatch vmt_patch {};
    bool minhook_active = false;
    bool engine2_probe_active = false;
    bool engine2_upstream_probe_active = false;
    bool engine2_upstream_probe_pending = false;
    bool animation_slot120_probe_active = false;
    bool animation_slot120_probe_pending = false;
    bool engine2_upstream_probe_analysis_done = false;
    bool file_system_scan_complete = false;
    bool file_system_flushed = false;
    bool file_system_carrier_probe_flushed = false;
    Engine2UpstreamAnalysisState engine2_upstream_analysis_state = Engine2UpstreamAnalysisState::None;
    std::uint32_t engine2_upstream_analysis_attempts = 0;
    std::uint64_t engine2_upstream_analysis_total_direct_calls = 0;
    std::uint32_t engine2_upstream_analysis_summary_count = 0;
    std::uint32_t engine2_upstream_analysis_exception_code = 0;
    std::uint64_t engine2_upstream_probe_armed_slot68_count = 0;
    std::uint32_t engine2_upstream_probe_window_attempts = 0;
    bool engine2_upstream_probe_window_resource_found = false;
    bool engine2_upstream_probe_window_dispatch_snapshot_complete = false;
    bool engine2_upstream_probe_window_exhausted_logged = false;
    bool engine2_upstream_keep_armed_logged = false;
    bool engine2_upstream_wait_exhausted_logged = false;
    std::uint32_t animation_slot120_probe_window_attempts = 0;
    bool animation_slot120_probe_window_resource_found = false;
    bool animation_slot120_probe_window_exhausted_logged = false;
    std::uint64_t file_system_probe_armed_tick = 0;
    bool installed = false;
    std::atomic<std::uint64_t> entered_count {0};
    std::atomic<std::uint64_t> slot68_entered_count {0};
    std::atomic<std::uint64_t> success_count {0};
    std::atomic<std::uint64_t> file_system_entered_count {0};
    std::atomic<std::uint64_t> file_system_capture_count {0};
    std::atomic<std::uint32_t> file_system_capture_state {static_cast<std::uint32_t>(FileSystemCaptureState::Inactive)};
    std::atomic<std::uint32_t> file_system_probe_call_count {0};
    std::atomic<bool> file_system_pending_flush {false};
    std::atomic<std::uint32_t> file_system_carrier_probe_attempts {0};
    std::atomic<bool> file_system_carrier_probe_done {false};
    std::atomic<bool> file_system_carrier_probe_pending_flush {false};
    std::atomic<std::uint32_t> sibling_active_mask {0};
    std::atomic<std::uint64_t> engine2_probe_entered_count {0};
    std::atomic<std::uint64_t> engine2_upstream_probe_entered_count {0};
    std::atomic<std::uint64_t> animation_slot120_probe_entered_count {0};
    std::atomic<bool> engine2_upstream_probe_remove_requested {false};
    std::atomic<bool> engine2_upstream_precall_snapshot_captured {false};
    std::atomic<std::uint32_t> engine2_upstream_precall_capture_state {0};
    std::atomic<bool> animation_slot120_probe_remove_requested {false};
    Engine2EntryCapture engine2_upstream_precall_capture {};
    const void* engine2_upstream_precall_return_address = nullptr;
    std::array<Engine2PrimaryReturnBucket, kEngine2PrimaryReturnBucketCapacity> engine2_primary_return_buckets {};
    std::array<Engine2PrimaryCallerCandidate, kEngine2PrimaryCallerCandidateCapacity> engine2_primary_caller_candidates {};
    std::array<Engine2PrimaryCallerCandidate, kAnimationSlot120CallerCandidateCapacity> animation_slot120_caller_candidates {};
    FileSystemAmbushCapture file_system_capture {};
    FileSystemCarrierProbeCapture file_system_carrier_probe_capture {};
    std::size_t engine2_primary_return_bucket_count = 0;
    std::size_t engine2_primary_caller_candidate_count = 0;
    std::size_t engine2_upstream_source_candidate_index = kInvalidSlot;
    std::size_t animation_slot120_caller_candidate_count = 0;
    std::size_t animation_slot120_source_candidate_index = kInvalidSlot;
    std::array<SiblingHookRuntime, 3> sibling_hooks {};
};

State g_state;
std::mutex g_state_mutex;
thread_local bool g_reentry_guard = false;

struct ThreadLocalEngine2PrimaryCapture
{
    Engine2EntryCapture capture {};
    const void* return_address = nullptr;
    std::uint64_t sequence = 0;
    std::uint64_t last_animation_slot120_sequence = 0;
    bool valid = false;
};

thread_local ThreadLocalEngine2PrimaryCapture g_thread_primary_capture {};

constexpr std::array<const wchar_t*, 8> kEngine2NonvolatileRegisterLabels {{
    L"rbx",
    L"rbp",
    L"rsi",
    L"rdi",
    L"r12",
    L"r13",
    L"r14",
    L"r15",
}};

constexpr const wchar_t* kRadarModules[] = {
    L"filesystem_stdio.dll",
    L"resourcesystem.dll",
    L"AfxHookSource2Rs.dll",
    L"AfxHookSourceRs.dll",
    L"AfxHookSource2.nskinz-test.dll",
    L"AfxHookSource2.dll",
    L"AfxHookSource.dll",
    L"client.dll",
};

constexpr std::uint32_t kResourceSystemQuerySlotOffset = 0x68u;
constexpr std::size_t kResourceSystemQuerySlotIndex = kResourceSystemQuerySlotOffset / sizeof(void*);
constexpr std::uint32_t kSiblingActiveScene = 0x1u;
constexpr std::uint32_t kSiblingActiveMaterial = 0x2u;
constexpr std::uint32_t kSiblingActiveAnimation = 0x4u;
constexpr std::array<SiblingHookDescriptor, 3> kSiblingHookDescriptors {{
    {0x248u, L"scenesystem.dll", "SIBLING_PROBE_SLOT_248", kSiblingActiveScene},
    {0x0A0u, L"materialsystem2.dll", "SIBLING_PROBE_SLOT_0A0", kSiblingActiveMaterial},
    {0x120u, L"animationsystem.dll", "SIBLING_PROBE_SLOT_120", kSiblingActiveAnimation},
}};

constexpr const char* kButterflyWeaponPath = "weapon_knife_butterfly.vmdl";
constexpr const char* kKarambitWeaponPath = "weapon_knife_karambit.vmdl";
constexpr const char* kButterflyWeaponPathC = "weapon_knife_butterfly.vmdl_c";
constexpr const char* kKarambitWeaponPathC = "weapon_knife_karambit.vmdl_c";
constexpr const char* kButterflyWeaponToken = "weapon_knife_butterfly";
constexpr const char* kKarambitWeaponToken = "weapon_knife_karambit";
constexpr const char* kButterflyWorldPath = "models/weapons/w_knife_butterfly.vmdl";
constexpr const char* kKarambitWorldPath = "models/weapons/w_knife_karam.vmdl";
constexpr const char* kButterflyWorldPathC = "models/weapons/w_knife_butterfly.vmdl_c";
constexpr const char* kKarambitWorldPathC = "models/weapons/w_knife_karam.vmdl_c";
constexpr const char* kButterflyViewPath = "models/weapons/v_knife_butterfly.vmdl";
constexpr const char* kKarambitViewPath = "models/weapons/v_knife_karam.vmdl";
constexpr const char* kButterflyViewPathC = "models/weapons/v_knife_butterfly.vmdl_c";
constexpr const char* kKarambitViewPathC = "models/weapons/v_knife_karam.vmdl_c";
constexpr std::size_t kQueryCarrierOffset = 0x60;
constexpr std::size_t kQueryArg4DirectPathOffset = 0x10;
constexpr std::size_t kQueryCarrierPathOffset = 0x18;

template <typename T>
bool SafeReadValue(const void* address, T& out) noexcept
{
    if (address == nullptr)
    {
        return false;
    }

    __try
    {
        out = *reinterpret_cast<const T*>(address);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool IsLikelyUserModePointer(const void* address) noexcept
{
    const auto value = reinterpret_cast<std::uintptr_t>(address);
    return value >= kMinUserModePointer && value <= kMaxUserModePointer;
}

bool TryAddPointerOffset(
    const void* base,
    const std::uintptr_t offset,
    const void*& out) noexcept
{
    out = nullptr;
    if (!IsLikelyUserModePointer(base))
    {
        return false;
    }

    const auto value = reinterpret_cast<std::uintptr_t>(base);
    if (value > kMaxUserModePointer - offset)
    {
        return false;
    }

    out = reinterpret_cast<const void*>(value + offset);
    return IsLikelyUserModePointer(out);
}

bool TryCaptureEngine2UpstreamWrapperDispatchSnapshot(
    const void* object,
    Engine2WrapperDispatchSnapshot& out) noexcept
{
    out = {};
    if (object == nullptr)
    {
        return false;
    }

    __try
    {
        out.rcx_object = object;
        if (!IsReadableAddress(out.rcx_object))
        {
            return false;
        }

        if (!SafeReadValue(out.rcx_object, out.emulated_rax_after_mov) ||
            !IsReadableAddress(out.emulated_rax_after_mov))
        {
            return false;
        }

        const void* slot_entry_address_const = nullptr;
        if (!TryAddPointerOffset(
                out.emulated_rax_after_mov,
                static_cast<std::uintptr_t>(kEngine2WrapperDispatchSlotOffset),
                slot_entry_address_const) ||
            !IsReadableAddress(slot_entry_address_const))
        {
            return false;
        }

        out.slot_entry_address = const_cast<void*>(slot_entry_address_const);
        if (!SafeReadValue(out.slot_entry_address, out.slot_target) ||
            !IsLikelyUserModePointer(out.slot_target))
        {
            return false;
        }

        out.direct_executable = IsExecutableAddress(out.slot_target);
        if (!out.direct_executable)
        {
            return true;
        }

        out.followed_target = FollowCodePointer(out.slot_target);
        if (out.followed_target == nullptr)
        {
            out.followed_target = out.slot_target;
        }

        out.followed_executable = IsExecutableAddress(out.followed_target);
        if (!out.followed_executable)
        {
            return true;
        }

        out.base_target = ResolveFunctionBase(out.followed_target);
        if (out.base_target == nullptr)
        {
            out.base_target = out.followed_target;
        }

        out.base_executable = IsExecutableAddress(out.base_target);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool TryCaptureEngine2UpstreamWrapperDispatchSnapshot(
    const Engine2EntryCapture* capture,
    Engine2WrapperDispatchSnapshot& out) noexcept
{
    if (capture == nullptr)
    {
        out = {};
        return false;
    }

    return TryCaptureEngine2UpstreamWrapperDispatchSnapshot(
        reinterpret_cast<const void*>(capture->rcx),
        out);
}

bool TryCaptureEngine2PrimaryDispatchSnapshot(
    const Engine2EntryCapture* capture,
    Engine2WrapperDispatchSnapshot& out) noexcept
{
    out = {};
    if (capture == nullptr)
    {
        return false;
    }

    __try
    {
        out.rcx_object = reinterpret_cast<const void*>(capture->rcx);
        out.emulated_rax_after_mov = reinterpret_cast<void*>(capture->rax);
        if (!IsReadableAddress(out.rcx_object) || !IsReadableAddress(out.emulated_rax_after_mov))
        {
            return false;
        }

        const void* slot_entry_address_const = nullptr;
        if (!TryAddPointerOffset(
                out.emulated_rax_after_mov,
                static_cast<std::uintptr_t>(kEngine2PrimaryDispatchSlotOffset),
                slot_entry_address_const) ||
            !IsReadableAddress(slot_entry_address_const))
        {
            return false;
        }

        out.slot_entry_address = const_cast<void*>(slot_entry_address_const);
        if (!SafeReadValue(out.slot_entry_address, out.slot_target) ||
            !IsLikelyUserModePointer(out.slot_target))
        {
            return false;
        }

        out.direct_executable = IsExecutableAddress(out.slot_target);
        if (!out.direct_executable)
        {
            return true;
        }

        out.followed_target = FollowCodePointer(out.slot_target);
        if (out.followed_target == nullptr)
        {
            out.followed_target = out.slot_target;
        }

        out.followed_executable = IsExecutableAddress(out.followed_target);
        if (!out.followed_executable)
        {
            return true;
        }

        out.base_target = ResolveFunctionBase(out.followed_target);
        if (out.base_target == nullptr)
        {
            out.base_target = out.followed_target;
        }

        out.base_executable = IsExecutableAddress(out.base_target);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

std::optional<ModuleSpan> GetModuleSpan(HMODULE module) noexcept
{
    if (module == nullptr)
    {
        return std::nullopt;
    }

    MODULEINFO info {};
    if (!GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info)))
    {
        return std::nullopt;
    }

    return ModuleSpan {
        static_cast<std::uint8_t*>(info.lpBaseOfDll),
        static_cast<std::size_t>(info.SizeOfImage),
    };
}

std::optional<ModuleSpan> GetTextSectionSpan(HMODULE module) noexcept
{
    const auto module_span = GetModuleSpan(module);
    if (!module_span.has_value())
    {
        return std::nullopt;
    }

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module_span->base);
    if (dos == nullptr || dos->e_magic != IMAGE_DOS_SIGNATURE)
    {
        return module_span;
    }

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        module_span->base + static_cast<std::size_t>(dos->e_lfanew));
    if (nt == nullptr || nt->Signature != IMAGE_NT_SIGNATURE)
    {
        return module_span;
    }

    const auto* section = IMAGE_FIRST_SECTION(nt);
    for (WORD index = 0; index < nt->FileHeader.NumberOfSections; ++index, ++section)
    {
        char name[9] {};
        std::memcpy(name, section->Name, 8);
        if (std::strcmp(name, ".text") == 0)
        {
            return ModuleSpan {
                module_span->base + section->VirtualAddress,
                static_cast<std::size_t>(
                    section->Misc.VirtualSize > section->SizeOfRawData
                        ? section->Misc.VirtualSize
                        : section->SizeOfRawData),
            };
        }
    }

    return module_span;
}

bool IsExecutableAddress(const void* address) noexcept
{
    if (!IsLikelyUserModePointer(address))
    {
        return false;
    }

    MEMORY_BASIC_INFORMATION memory_info {};
    if (VirtualQuery(address, &memory_info, sizeof(memory_info)) != sizeof(memory_info))
    {
        return false;
    }

    if (memory_info.State != MEM_COMMIT || (memory_info.Protect & PAGE_GUARD) != 0)
    {
        return false;
    }

    switch (memory_info.Protect & 0xFFu)
    {
    case PAGE_EXECUTE:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

bool IsReadableAddress(const void* address) noexcept
{
    if (!IsLikelyUserModePointer(address))
    {
        return false;
    }

    MEMORY_BASIC_INFORMATION memory_info {};
    if (VirtualQuery(address, &memory_info, sizeof(memory_info)) != sizeof(memory_info))
    {
        return false;
    }

    if (memory_info.State != MEM_COMMIT || (memory_info.Protect & PAGE_GUARD) != 0)
    {
        return false;
    }

    switch (memory_info.Protect & 0xFFu)
    {
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

bool IsWritableAddress(const void* address) noexcept
{
    if (!IsLikelyUserModePointer(address))
    {
        return false;
    }

    MEMORY_BASIC_INFORMATION memory_info {};
    if (VirtualQuery(address, &memory_info, sizeof(memory_info)) != sizeof(memory_info))
    {
        return false;
    }

    if (memory_info.State != MEM_COMMIT || (memory_info.Protect & PAGE_GUARD) != 0)
    {
        return false;
    }

    switch (memory_info.Protect & 0xFFu)
    {
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

bool AddressInSpan(const ModuleSpan& span, const void* address) noexcept
{
    if (span.base == nullptr || address == nullptr)
    {
        return false;
    }

    const auto value = reinterpret_cast<std::uintptr_t>(address);
    const auto begin = reinterpret_cast<std::uintptr_t>(span.base);
    const auto end = begin + span.size;
    return value >= begin && value < end;
}

HMODULE GetModuleFromAddress(const void* address) noexcept
{
    if (!IsLikelyUserModePointer(address))
    {
        return nullptr;
    }

    HMODULE module = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(address),
            &module))
    {
        return nullptr;
    }

    return module;
}

std::wstring FileNameOnly(const std::wstring& path)
{
    const auto separator = path.find_last_of(L"\\/");
    return separator == std::wstring::npos ? path : path.substr(separator + 1);
}

std::wstring GetModuleFileNameOnly(HMODULE module)
{
    if (module == nullptr)
    {
        return {};
    }

    std::wstring buffer(MAX_PATH, L'\0');
    const auto length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
    buffer.resize(length);
    return FileNameOnly(buffer);
}

std::wstring GetModuleFilePath(HMODULE module)
{
    if (module == nullptr)
    {
        return {};
    }

    std::wstring buffer(MAX_PATH, L'\0');
    const auto length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
    buffer.resize(length);
    return buffer;
}

std::wstring ToLowerCopy(std::wstring value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](wchar_t ch)
        {
            return static_cast<wchar_t>(std::towlower(ch));
         });
    return value;
}

bool ModuleNameEquals(const std::wstring& module_name, const wchar_t* expected) noexcept
{
    return expected != nullptr && !module_name.empty() && _wcsicmp(module_name.c_str(), expected) == 0;
}

bool IsNoisyEngine2UpstreamOwner(const std::wstring& owner) noexcept
{
    return ModuleNameEquals(owner, L"tier0.dll") ||
        ModuleNameEquals(owner, L"localize.dll") ||
        ModuleNameEquals(owner, L"dbghelp.dll") ||
        ModuleNameEquals(owner, L"nskinzcs2.dll");
}

std::wstring FormatPointer(const void* address)
{
    wchar_t buffer[32] {};
    swprintf_s(buffer, L"0x%p", address);
    return buffer;
}

std::wstring FormatMachineBytes(const void* address, const std::size_t count)
{
    if (address == nullptr || count == 0)
    {
        return L"<null>";
    }

    std::wstring text;
    text.reserve(count * 3);

    const auto* bytes = static_cast<const std::uint8_t*>(address);
    for (std::size_t index = 0; index < count; ++index)
    {
        std::uint8_t value = 0;
        if (!SafeReadValue(bytes + index, value))
        {
            text += L"??";
        }
        else
        {
            wchar_t token[4] {};
            swprintf_s(token, L"%02X", static_cast<unsigned int>(value));
            text += token;
        }

        if (index + 1 < count)
        {
            text += L' ';
        }
    }

    return text;
}

void FormatMachineBytesAnsi(const void* address, const std::size_t count, char* out, const std::size_t out_size) noexcept
{
    if (out == nullptr || out_size == 0)
    {
        return;
    }

    out[0] = '\0';
    if (address == nullptr || count == 0)
    {
        sprintf_s(out, out_size, "<null>");
        return;
    }

    std::size_t cursor = 0;
    const auto* bytes = static_cast<const std::uint8_t*>(address);
    for (std::size_t index = 0; index < count && cursor + 4 < out_size; ++index)
    {
        std::uint8_t value = 0;
        const int written = SafeReadValue(bytes + index, value)
            ? sprintf_s(out + cursor, out_size - cursor, "%02X", static_cast<unsigned int>(value))
            : sprintf_s(out + cursor, out_size - cursor, "??");
        if (written <= 0)
        {
            out[cursor] = '\0';
            return;
        }

        cursor += static_cast<std::size_t>(written);
        if (index + 1 < count && cursor + 2 < out_size)
        {
            out[cursor++] = ' ';
            out[cursor] = '\0';
        }
    }
}

void WideToAnsiLossy(const std::wstring& text, char* out, const std::size_t out_size) noexcept
{
    if (out == nullptr || out_size == 0)
    {
        return;
    }

    out[0] = '\0';
    if (text.empty())
    {
        sprintf_s(out, out_size, "<unknown>");
        return;
    }

    const int converted = WideCharToMultiByte(
        CP_ACP,
        0,
        text.c_str(),
        -1,
        out,
        static_cast<int>(out_size),
        nullptr,
        nullptr);
    if (converted <= 0)
    {
        sprintf_s(out, out_size, "<ansi-conversion-failed>");
    }
}

void LogTargetBytes(const wchar_t* label, const void* address)
{
    std::wstring message = label != nullptr ? label : L"GlobalResourceSwapper target";
    message += L": address=";
    message += FormatPointer(address);
    message += L", bytes=";
    message += FormatMachineBytes(address, kMachineByteCount);
    nskinzcs2::Logger::Info(message);
}

void LogMinHookStep(const wchar_t* step, const MH_STATUS status, const void* target)
{
    std::wstring message = step != nullptr ? step : L"MinHook";
    message += L": status=";
    message += std::to_wstring(static_cast<int>(status));
    message += L", target=";
    message += FormatPointer(target);
    nskinzcs2::Logger::Info(message);
}

void __fastcall Engine2PrimaryCallerProbeCallback(const Engine2EntryCapture* capture) noexcept;
void __fastcall Engine2UpstreamCallerProbeCallback(const Engine2EntryCapture* capture) noexcept;
void __fastcall AnimationSlot120CallerProbeCallback(const Engine2EntryCapture* capture) noexcept;
bool TryLogEngine2UpstreamWrapperDispatchSnapshotOnce(
    const Engine2EntryCapture* capture,
    const CallerInfo* info) noexcept;
void TryLogEngine2PrimaryDispatchSnapshotOnce(
    const Engine2EntryCapture* capture,
    const CallerInfo* info) noexcept;
void TryTrackEngine2PrimaryCallerSource(const CallerInfo& info) noexcept;
void TryCaptureEngine2UpstreamLiveObject(
    const CallerInfo& info,
    const Engine2EntryCapture* capture) noexcept;

bool PatchStubAddress(void* stub, const std::size_t offset, const void* address) noexcept
{
    if (stub == nullptr || address == nullptr || offset + sizeof(void*) > kEngine2ProbeStubSize)
    {
        return false;
    }

    __try
    {
        std::memcpy(static_cast<std::uint8_t*>(stub) + offset, &address, sizeof(address));
        FlushInstructionCache(GetCurrentProcess(), static_cast<std::uint8_t*>(stub) + offset, sizeof(address));
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

void* CreateEngine2PrimaryCallerProbeStub() noexcept
{
    static constexpr std::uint8_t kStubTemplate[kEngine2ProbeStubSize] = {
        0x48, 0x81, 0xEC, 0x08, 0x01, 0x00, 0x00,
        0x48, 0x89, 0x44, 0x24, 0x20,
        0x48, 0x89, 0x4C, 0x24, 0x28,
        0x48, 0x89, 0x54, 0x24, 0x30,
        0x4C, 0x89, 0x44, 0x24, 0x38,
        0x4C, 0x89, 0x4C, 0x24, 0x40,
        0x4C, 0x89, 0x54, 0x24, 0x48,
        0x4C, 0x89, 0x5C, 0x24, 0x50,
        0x48, 0x8D, 0x84, 0x24, 0x08, 0x01, 0x00, 0x00,
        0x48, 0x89, 0x44, 0x24, 0x58,
        0x48, 0x89, 0x5C, 0x24, 0x60,
        0x48, 0x89, 0x6C, 0x24, 0x68,
        0x48, 0x89, 0x74, 0x24, 0x70,
        0x48, 0x89, 0x7C, 0x24, 0x78,
        0x4C, 0x89, 0xA4, 0x24, 0x80, 0x00, 0x00, 0x00,
        0x4C, 0x89, 0xAC, 0x24, 0x88, 0x00, 0x00, 0x00,
        0x4C, 0x89, 0xB4, 0x24, 0x90, 0x00, 0x00, 0x00,
        0x4C, 0x89, 0xBC, 0x24, 0x98, 0x00, 0x00, 0x00,
        0xF3, 0x0F, 0x7F, 0x84, 0x24, 0xA0, 0x00, 0x00, 0x00,
        0xF3, 0x0F, 0x7F, 0x8C, 0x24, 0xB0, 0x00, 0x00, 0x00,
        0xF3, 0x0F, 0x7F, 0x94, 0x24, 0xC0, 0x00, 0x00, 0x00,
        0xF3, 0x0F, 0x7F, 0x9C, 0x24, 0xD0, 0x00, 0x00, 0x00,
        0xF3, 0x0F, 0x7F, 0xA4, 0x24, 0xE0, 0x00, 0x00, 0x00,
        0xF3, 0x0F, 0x7F, 0xAC, 0x24, 0xF0, 0x00, 0x00, 0x00,
        0x48, 0x8D, 0x4C, 0x24, 0x20,
        0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xFF, 0xD0,
        0xF3, 0x0F, 0x6F, 0x84, 0x24, 0xA0, 0x00, 0x00, 0x00,
        0xF3, 0x0F, 0x6F, 0x8C, 0x24, 0xB0, 0x00, 0x00, 0x00,
        0xF3, 0x0F, 0x6F, 0x94, 0x24, 0xC0, 0x00, 0x00, 0x00,
        0xF3, 0x0F, 0x6F, 0x9C, 0x24, 0xD0, 0x00, 0x00, 0x00,
        0xF3, 0x0F, 0x6F, 0xA4, 0x24, 0xE0, 0x00, 0x00, 0x00,
        0xF3, 0x0F, 0x6F, 0xAC, 0x24, 0xF0, 0x00, 0x00, 0x00,
        0x4C, 0x8B, 0xBC, 0x24, 0x98, 0x00, 0x00, 0x00,
        0x4C, 0x8B, 0xB4, 0x24, 0x90, 0x00, 0x00, 0x00,
        0x4C, 0x8B, 0xAC, 0x24, 0x88, 0x00, 0x00, 0x00,
        0x4C, 0x8B, 0xA4, 0x24, 0x80, 0x00, 0x00, 0x00,
        0x48, 0x8B, 0x7C, 0x24, 0x78,
        0x48, 0x8B, 0x74, 0x24, 0x70,
        0x48, 0x8B, 0x6C, 0x24, 0x68,
        0x48, 0x8B, 0x5C, 0x24, 0x60,
        0x4C, 0x8B, 0x5C, 0x24, 0x50,
        0x4C, 0x8B, 0x54, 0x24, 0x48,
        0x4C, 0x8B, 0x4C, 0x24, 0x40,
        0x4C, 0x8B, 0x44, 0x24, 0x38,
        0x48, 0x8B, 0x54, 0x24, 0x30,
        0x48, 0x8B, 0x4C, 0x24, 0x28,
        0x48, 0x81, 0xC4, 0x08, 0x01, 0x00, 0x00,
        0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xFF, 0xE0,
    };

    static_assert(sizeof(kStubTemplate) == kEngine2ProbeStubSize);
    static_assert(sizeof(Engine2EntryCapture) == 0x80);

    void* stub = VirtualAlloc(nullptr, kEngine2ProbeStubSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (stub == nullptr)
    {
        return nullptr;
    }

    std::memcpy(stub, kStubTemplate, sizeof(kStubTemplate));
    if (!PatchStubAddress(stub, kEngine2ProbeHelperAddressOffset, reinterpret_cast<const void*>(&Engine2PrimaryCallerProbeCallback)))
    {
        VirtualFree(stub, 0, MEM_RELEASE);
        return nullptr;
    }

    FlushInstructionCache(GetCurrentProcess(), stub, kEngine2ProbeStubSize);
    return stub;
}

void* CreateEngine2UpstreamCallerProbeStub() noexcept
{
    void* stub = CreateEngine2PrimaryCallerProbeStub();
    if (stub == nullptr)
    {
        return nullptr;
    }

    if (!PatchStubAddress(stub, kEngine2ProbeHelperAddressOffset, reinterpret_cast<const void*>(&Engine2UpstreamCallerProbeCallback)))
    {
        VirtualFree(stub, 0, MEM_RELEASE);
        return nullptr;
    }

    FlushInstructionCache(GetCurrentProcess(), stub, kEngine2ProbeStubSize);
    return stub;
}

void* CreateAnimationSlot120CallerProbeStub() noexcept
{
    void* stub = CreateEngine2PrimaryCallerProbeStub();
    if (stub == nullptr)
    {
        return nullptr;
    }

    if (!PatchStubAddress(stub, kEngine2ProbeHelperAddressOffset, reinterpret_cast<const void*>(&AnimationSlot120CallerProbeCallback)))
    {
        VirtualFree(stub, 0, MEM_RELEASE);
        return nullptr;
    }

    FlushInstructionCache(GetCurrentProcess(), stub, kEngine2ProbeStubSize);
    return stub;
}

void* FollowCodePointer(void* address) noexcept
{
    if (!IsExecutableAddress(address))
    {
        return nullptr;
    }

    auto* cursor = static_cast<std::uint8_t*>(address);
    for (int depth = 0; depth < 4 && cursor != nullptr; ++depth)
    {
        __try
        {
            if (cursor[0] == 0xE9)
            {
                const auto rel = *reinterpret_cast<const std::int32_t*>(cursor + 1);
                cursor = cursor + 5 + rel;
                continue;
            }
            if (cursor[0] == 0xEB)
            {
                const auto rel = *reinterpret_cast<const std::int8_t*>(cursor + 1);
                cursor = cursor + 2 + rel;
                continue;
            }
            if (cursor[0] == 0xFF && cursor[1] == 0x25)
            {
                void* target = nullptr;
                const auto rel = *reinterpret_cast<const std::int32_t*>(cursor + 2);
                if (!SafeReadValue(cursor + 6 + rel, target))
                {
                    return nullptr;
                }
                if (!IsExecutableAddress(target))
                {
                    return nullptr;
                }
                cursor = static_cast<std::uint8_t*>(target);
                continue;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }

        break;
    }

    return cursor;
}

void* ResolveFunctionBase(void* address) noexcept
{
    auto* followed = FollowCodePointer(address);
    auto* probe = followed != nullptr ? followed : static_cast<std::uint8_t*>(address);
    if (probe == nullptr || !IsExecutableAddress(probe))
    {
        return nullptr;
    }

    DWORD64 image_base = 0;
    const auto runtime_function = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(probe), &image_base, nullptr);
    if (runtime_function != nullptr && image_base != 0 && runtime_function->BeginAddress != 0)
    {
        return reinterpret_cast<void*>(image_base + static_cast<DWORD64>(runtime_function->BeginAddress));
    }

    return probe;
}

bool ResolveCallerInfo(const void* return_address, CallerInfo& out) noexcept
{
    out = {};
    if (return_address == nullptr)
    {
        return false;
    }

    out.return_address = return_address;
    out.return_owner = GetModuleFileNameOnly(GetModuleFromAddress(return_address));
    out.caller_base = ResolveFunctionBase(const_cast<void*>(return_address));
    out.caller_owner = GetModuleFileNameOnly(
        GetModuleFromAddress(out.caller_base != nullptr ? out.caller_base : return_address));

    const auto return_value = reinterpret_cast<std::uintptr_t>(return_address);
    const auto caller_value = reinterpret_cast<std::uintptr_t>(out.caller_base);
    out.caller_offset = caller_value != 0 && return_value >= caller_value
        ? return_value - caller_value
        : 0;
    out.before_return = reinterpret_cast<const void*>(
        return_value > kMachineByteCount ? return_value - kMachineByteCount : return_value);
    return true;
}

bool TryBuildUnwindContextFromEntryCapture(
    const Engine2EntryCapture* capture,
    const void* control_pc,
    CONTEXT& out_context) noexcept
{
    std::memset(&out_context, 0, sizeof(out_context));
    if (capture == nullptr ||
        !IsLikelyUserModePointer(control_pc) ||
        capture->original_rsp < kMinUserModePointer)
    {
        return false;
    }

    out_context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    out_context.Rip = reinterpret_cast<DWORD64>(control_pc);
    out_context.Rsp = static_cast<DWORD64>(capture->original_rsp);
    out_context.Rax = static_cast<DWORD64>(capture->rax);
    out_context.Rcx = static_cast<DWORD64>(capture->rcx);
    out_context.Rdx = static_cast<DWORD64>(capture->rdx);
    out_context.Rbx = static_cast<DWORD64>(capture->rbx);
    out_context.Rbp = static_cast<DWORD64>(capture->rbp);
    out_context.Rsi = static_cast<DWORD64>(capture->rsi);
    out_context.Rdi = static_cast<DWORD64>(capture->rdi);
    out_context.R8 = static_cast<DWORD64>(capture->r8);
    out_context.R9 = static_cast<DWORD64>(capture->r9);
    out_context.R10 = static_cast<DWORD64>(capture->r10);
    out_context.R11 = static_cast<DWORD64>(capture->r11);
    out_context.R12 = static_cast<DWORD64>(capture->r12);
    out_context.R13 = static_cast<DWORD64>(capture->r13);
    out_context.R14 = static_cast<DWORD64>(capture->r14);
    out_context.R15 = static_cast<DWORD64>(capture->r15);
    return true;
}

bool TryStepUnwindContext(CONTEXT& context) noexcept
{
    if (context.Rip < kMinUserModePointer || context.Rsp < kMinUserModePointer)
    {
        return false;
    }

    const auto previous_rip = context.Rip;
    const auto previous_rsp = context.Rsp;

    __try
    {
        DWORD64 image_base = 0;
        const auto runtime_function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
        if (runtime_function != nullptr)
        {
            PVOID handler_data = nullptr;
            DWORD64 establisher_frame = 0;
            KNONVOLATILE_CONTEXT_POINTERS context_pointers {};
            RtlVirtualUnwind(
                UNW_FLAG_NHANDLER,
                image_base,
                context.Rip,
                runtime_function,
                &context,
                &handler_data,
                &establisher_frame,
                &context_pointers);
        }
        else
        {
            DWORD64 return_address = 0;
            if (!SafeReadValue(reinterpret_cast<const void*>(context.Rsp), return_address) ||
                return_address < kMinUserModePointer)
            {
                return false;
            }

            context.Rip = return_address;
            context.Rsp += sizeof(DWORD64);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }

    if (context.Rip < kMinUserModePointer ||
        context.Rsp < kMinUserModePointer ||
        (context.Rip == previous_rip && context.Rsp == previous_rsp))
    {
        return false;
    }

    return true;
}

std::size_t CollectEngine2PrimaryCallerAncestry(
    const Engine2EntryCapture* capture,
    const void* control_pc,
    std::array<CallerInfo, kEngine2PrimaryAncestryFrameCapacity>& out_infos) noexcept
{
    out_infos.fill({});

    CONTEXT context {};
    if (!TryBuildUnwindContextFromEntryCapture(capture, control_pc, context))
    {
        return 0;
    }

    std::size_t count = 0;
    for (std::size_t depth = 0; depth < out_infos.size(); ++depth)
    {
        if (!TryStepUnwindContext(context))
        {
            break;
        }

        CallerInfo info {};
        if (!ResolveCallerInfo(reinterpret_cast<const void*>(context.Rip), info))
        {
            break;
        }

        info.stack_depth = static_cast<std::uint32_t>(depth);
        out_infos[count++] = std::move(info);
    }

    return count;
}

bool TryBuildCallerInfoFromCandidate(
    const Engine2PrimaryCallerCandidate& candidate,
    CallerInfo& out_info) noexcept
{
    out_info = {};
    if (candidate.caller_base == nullptr || candidate.sample_return_address == nullptr)
    {
        return false;
    }

    out_info.return_address = candidate.sample_return_address;
    out_info.return_owner = GetModuleFileNameOnly(GetModuleFromAddress(candidate.sample_return_address));
    out_info.caller_base = candidate.caller_base;
    out_info.caller_owner = GetModuleFileNameOnly(GetModuleFromAddress(candidate.caller_base));
    out_info.caller_offset = candidate.caller_offset;
    out_info.stack_depth = candidate.sample_depth;

    const auto return_value = reinterpret_cast<std::uintptr_t>(candidate.sample_return_address);
    out_info.before_return = reinterpret_cast<const void*>(
        return_value > kMachineByteCount ? return_value - kMachineByteCount : return_value);
    return true;
}

std::size_t CountPatternBytes(const char* pattern) noexcept
{
    if (pattern == nullptr)
    {
        return 0;
    }

    std::size_t count = 0;
    while (*pattern != '\0')
    {
        while (*pattern == ' ')
        {
            ++pattern;
        }
        if (*pattern == '\0')
        {
            break;
        }
        ++count;
        while (*pattern != '\0' && *pattern != ' ')
        {
            ++pattern;
        }
    }

    return count;
}

bool MatchPatternAt(const std::uint8_t* address, const char* pattern) noexcept
{
    if (address == nullptr || pattern == nullptr)
    {
        return false;
    }

    std::size_t byte_index = 0;
    while (*pattern != '\0')
    {
        while (*pattern == ' ')
        {
            ++pattern;
        }
        if (*pattern == '\0')
        {
            break;
        }
        if (*pattern == '?')
        {
            ++byte_index;
            ++pattern;
            if (*pattern == '?')
            {
                ++pattern;
            }
            continue;
        }

        char token[3] = {'\0', '\0', '\0'};
        token[0] = *pattern++;
        if (*pattern == '\0')
        {
            return false;
        }
        token[1] = *pattern++;

        if (address[byte_index] != static_cast<std::uint8_t>(std::strtoul(token, nullptr, 16)))
        {
            return false;
        }
        ++byte_index;
    }

    return true;
}

std::size_t FindPatternMatches(const ModuleSpan& span, const char* pattern, void** matches, const std::size_t max_matches) noexcept
{
    const auto pattern_size = CountPatternBytes(pattern);
    if (span.base == nullptr || pattern == nullptr || pattern_size == 0 || span.size < pattern_size)
    {
        return 0;
    }

    std::size_t count = 0;
    for (std::size_t offset = 0; offset + pattern_size <= span.size; ++offset)
    {
        const auto* address = span.base + offset;
        if (!MatchPatternAt(address, pattern))
        {
            continue;
        }

        if (count < max_matches)
        {
            matches[count] = const_cast<std::uint8_t*>(address);
        }
        ++count;
        if (count >= max_matches)
        {
            break;
        }
    }

    return count;
}

bool IsTrustedResourceModuleName(const std::wstring& module_name) noexcept
{
    const auto lowered = ToLowerCopy(module_name);
    return lowered == L"resourcesystem.dll" || lowered.find(L"afxhooksource") != std::wstring::npos;
}

bool IsTrustedFileSystemModuleName(const std::wstring& module_name) noexcept
{
    const auto lowered = ToLowerCopy(module_name);
    return lowered == L"filesystem_stdio.dll" ||
        lowered == L"tier0.dll" ||
        lowered.find(L"filesystem") != std::wstring::npos;
}

bool IsPreferredEngine2ModelOwnerName(const std::wstring& module_name) noexcept
{
    if (module_name.empty())
    {
        return false;
    }

    const auto lowered = ToLowerCopy(module_name);
    return lowered == L"client.dll" ||
        lowered == L"animationsystem.dll" ||
        lowered == L"resourcesystem.dll" ||
        lowered.find(L"anim") != std::wstring::npos ||
        lowered.find(L"scene") != std::wstring::npos ||
        lowered.find(L"model") != std::wstring::npos;
}

bool IsRejectedEngine2ModelOwnerName(const std::wstring& module_name) noexcept
{
    if (module_name.empty())
    {
        return false;
    }

    const auto lowered = ToLowerCopy(module_name);
    return lowered == L"localize.dll" || lowered == L"tier0.dll";
}

struct Engine2UpstreamOwnerDecision
{
    bool accepted = false;
    bool rejected = false;
    const wchar_t* source = L"none";
    std::wstring owner;
};

Engine2UpstreamOwnerDecision SelectEngine2UpstreamOwner(
    const std::wstring& return_owner,
    const std::wstring& caller_owner,
    const std::wstring& emulated_rax_owner,
    const std::wstring& slot_target_owner,
    const std::wstring& followed_target_owner,
    const std::wstring& base_target_owner) noexcept
{
    Engine2UpstreamOwnerDecision decision {};

    const struct Candidate
    {
        const wchar_t* source;
        const std::wstring* owner;
    } candidates[] = {
        {L"base_owner", &base_target_owner},
        {L"followed_owner", &followed_target_owner},
        {L"call_target_owner", &slot_target_owner},
        {L"emulated_rax_owner", &emulated_rax_owner},
        {L"caller_owner", &caller_owner},
        {L"ret_owner", &return_owner},
    };

    for (const auto& candidate : candidates)
    {
        if (candidate.owner != nullptr && IsPreferredEngine2ModelOwnerName(*candidate.owner))
        {
            decision.accepted = true;
            decision.source = candidate.source;
            decision.owner = *candidate.owner;
            return decision;
        }
    }

    for (const auto& candidate : candidates)
    {
        if (candidate.owner != nullptr && IsRejectedEngine2ModelOwnerName(*candidate.owner))
        {
            decision.rejected = true;
            decision.source = candidate.source;
            decision.owner = *candidate.owner;
            return decision;
        }
    }

    return decision;
}

OwnerSummary CollectOwnerSummary(const wchar_t* label, void* iface, const std::size_t slot_begin, const std::size_t slot_end_exclusive)
{
    OwnerSummary summary {};
    summary.label = label != nullptr ? label : L"interface";
    if (iface == nullptr || slot_end_exclusive <= slot_begin)
    {
        return summary;
    }

    void** vtable = nullptr;
    if (!SafeReadValue(iface, vtable) || vtable == nullptr)
    {
        return summary;
    }

    summary.available = true;
    summary.slot_count = slot_end_exclusive - slot_begin;
    std::vector<std::pair<std::wstring, std::size_t>> buckets;
    buckets.reserve(summary.slot_count);

    for (std::size_t slot = slot_begin; slot < slot_end_exclusive; ++slot)
    {
        void* entry = nullptr;
        if (!SafeReadValue(vtable + slot, entry) || entry == nullptr)
        {
            continue;
        }

        const auto followed = FollowCodePointer(entry);
        const auto name = GetModuleFileNameOnly(GetModuleFromAddress(followed != nullptr ? followed : entry));
        auto bucket = std::find_if(
            buckets.begin(),
            buckets.end(),
            [&name](const auto& value)
            {
                return _wcsicmp(value.first.c_str(), name.c_str()) == 0;
            });
        if (bucket == buckets.end())
        {
            buckets.emplace_back(name, 1);
            bucket = buckets.end() - 1;
        }
        else
        {
            ++bucket->second;
        }

        if (bucket->second > summary.dominant_count)
        {
            summary.dominant_module = bucket->first;
            summary.dominant_count = bucket->second;
        }
    }

    std::wstring message = L"GlobalResourceSwapper owner radar ";
    message += summary.label;
    message += L": dominant=";
    message += summary.dominant_module.empty() ? L"<unknown>" : summary.dominant_module;
    message += L"(";
    message += std::to_wstring(summary.dominant_count);
    message += L"/";
    message += std::to_wstring(summary.slot_count);
    message += L")";
    nskinzcs2::Logger::Info(message);
    return summary;
}

void LogOwnerRadar()
{
    for (const auto* module_name : kRadarModules)
    {
        const auto module = GetModuleHandleW(module_name);
        if (module == nullptr)
        {
            continue;
        }

        std::wstring message = L"GlobalResourceSwapper owner radar module: ";
        message += module_name;
        message += L", base=";
        message += FormatPointer(module);

        const auto span = GetModuleSpan(module);
        if (span.has_value())
        {
            message += L", size=0x";
            wchar_t size_text[32] {};
            swprintf_s(size_text, L"%zX", span->size);
            message += size_text;
        }

        nskinzcs2::Logger::Info(message);
    }
}

const wchar_t* RouteText(const Route route) noexcept
{
    switch (route)
    {
    case Route::FileSystemAmbush:
        return L"filesystem_ambush";
    case Route::ResourceSystemSlot68:
        return L"resourcesystem_slot_0x68";
    case Route::ResourceSystemVtableQuery:
        return L"resource_system_vtable_query";
    case Route::ResourceSystemQueryBody:
        return L"resourcesystem_query_body";
    case Route::None:
    default:
        return L"none";
    }
}

bool TryResolveQuerySignature(void*& out_match, std::wstring& issue)
{
    out_match = nullptr;
    issue.clear();

    const auto module = GetModuleHandleW(L"resourcesystem.dll");
    const auto text = GetTextSectionSpan(module);
    if (module == nullptr || !text.has_value())
    {
        issue = L"resourcesystem.dll unavailable";
        return false;
    }

    void* matches[4] {};
    const auto count = FindPatternMatches(
        *text,
        nskinzcs2::kGlobalResourceSwapperSignatureHints[0].ida_pattern,
        matches,
        std::size(matches));
    if (count == 0)
    {
        issue = L"query pattern not found";
        return false;
    }
    if (count != 1)
    {
        issue = L"query pattern not unique";
        return false;
    }
    if (!IsExecutableAddress(matches[0]) || !AddressInSpan(*text, matches[0]))
    {
        issue = L"query pattern resolved outside executable text";
        return false;
    }

    out_match = matches[0];
    return true;
}

bool TryValidateSlotTarget(void* iface, const std::size_t slot, void*& out_target) noexcept
{
    out_target = nullptr;
    if (iface == nullptr)
    {
        return false;
    }

    void** vtable = nullptr;
    if (!SafeReadValue(iface, vtable) || vtable == nullptr)
    {
        return false;
    }

    void* candidate = nullptr;
    if (!SafeReadValue(vtable + slot, candidate) || candidate == nullptr)
    {
        return false;
    }

    auto* followed = FollowCodePointer(candidate);
    const auto direct_name = GetModuleFileNameOnly(GetModuleFromAddress(candidate));
    const auto followed_name = GetModuleFileNameOnly(GetModuleFromAddress(followed != nullptr ? followed : candidate));
    if (!IsTrustedResourceModuleName(direct_name) && !IsTrustedResourceModuleName(followed_name))
    {
        return false;
    }

    out_target = candidate;
    return true;
}

bool TryValidateFileSystemSlotTarget(void* iface, const std::size_t slot, void*& out_target) noexcept
{
    out_target = nullptr;
    if (iface == nullptr)
    {
        return false;
    }

    void** vtable = nullptr;
    if (!SafeReadValue(iface, vtable) || vtable == nullptr)
    {
        return false;
    }

    void* candidate = nullptr;
    if (!SafeReadValue(vtable + slot, candidate) || candidate == nullptr)
    {
        return false;
    }

    auto* followed = FollowCodePointer(candidate);
    const auto direct_name = GetModuleFileNameOnly(GetModuleFromAddress(candidate));
    const auto followed_name = GetModuleFileNameOnly(GetModuleFromAddress(followed != nullptr ? followed : candidate));
    if (!IsTrustedFileSystemModuleName(direct_name) && !IsTrustedFileSystemModuleName(followed_name))
    {
        return false;
    }

    if (!IsExecutableAddress(candidate) && !IsExecutableAddress(followed != nullptr ? followed : candidate))
    {
        return false;
    }

    out_target = candidate;
    return true;
}

void DeactivateFileSystemProbeLocked() noexcept
{
    if (g_state.minhook_active && g_state.target != nullptr)
    {
        MH_DisableHook(g_state.target);
        MH_RemoveHook(g_state.target);
    }

    g_state.signature_match = nullptr;
    g_state.target = nullptr;
    g_state.original = nullptr;
    g_state.slot = kInvalidSlot;
    g_state.minhook_active = false;
    g_state.file_system_probe_call_count.store(0, std::memory_order_relaxed);
    g_state.file_system_probe_armed_tick = 0;
}

bool TryActivateNextFileSystemProbeLocked(std::wstring& reason) noexcept
{
    reason.clear();
    if (g_state.file_system_interface == nullptr)
    {
        reason = L"file_system_interface_missing";
        return false;
    }

    const auto start_slot = g_state.file_system_next_slot == kInvalidSlot
        ? kFileSystemFirstSlotIndex
        : g_state.file_system_next_slot;
    for (std::size_t slot = start_slot; slot <= kFileSystemLastSlotIndex; ++slot)
    {
        void* target = nullptr;
        if (!TryValidateFileSystemSlotTarget(g_state.file_system_interface, slot, target))
        {
            continue;
        }

        void* original_raw = nullptr;
        const auto create_status = MH_CreateHook(
            target,
            reinterpret_cast<void*>(&hkFileSystemSlotProbe),
            &original_raw);
        LogMinHookStep(L"MH_CreateHook[filesystem_probe_slot]", create_status, target);
        if (create_status != MH_OK)
        {
            continue;
        }

        const auto enable_status = MH_EnableHook(target);
        LogMinHookStep(L"MH_EnableHook[filesystem_probe_slot]", enable_status, target);
        if (enable_status != MH_OK && enable_status != MH_ERROR_ENABLED)
        {
            MH_RemoveHook(target);
            continue;
        }

        g_state.signature_match = target;
        g_state.target = target;
        g_state.original = reinterpret_cast<QueryFn>(original_raw);
        g_state.slot = slot * sizeof(void*);
        g_state.file_system_next_slot = slot + 1;
        g_state.minhook_active = true;
        g_state.file_system_probe_call_count.store(0, std::memory_order_relaxed);
        g_state.file_system_probe_armed_tick = GetTickCount64();
        g_state.file_system_capture_state.store(
            static_cast<std::uint32_t>(FileSystemCaptureState::Armed),
            std::memory_order_relaxed);

        std::wstring line = L"GlobalResourceSwapper filesystem probe armed: slot=0x";
        wchar_t slot_text[32] {};
        swprintf_s(slot_text, L"%zX", g_state.slot);
        line += slot_text;
        line += L", target=";
        line += FormatPointer(target);
        line += L", original=";
        line += FormatPointer(original_raw);
        line += L".";
        nskinzcs2::Logger::Info(line);
        return true;
    }

    reason = L"no_vfilesystem_path_slot";
    g_state.file_system_scan_complete = true;
    g_state.file_system_capture_state.store(
        static_cast<std::uint32_t>(FileSystemCaptureState::Miss),
        std::memory_order_relaxed);
    return false;
}

void TryAdvanceFileSystemProbe() noexcept
{
    if (g_state.route != Route::FileSystemAmbush ||
        g_state.file_system_capture_count.load(std::memory_order_acquire) != 0)
    {
        return;
    }

    std::scoped_lock lock(g_state_mutex);
    if (!g_state.installed ||
        g_state.route != Route::FileSystemAmbush ||
        g_state.file_system_scan_complete ||
        !g_state.minhook_active)
    {
        return;
    }

    const auto calls = g_state.file_system_probe_call_count.load(std::memory_order_relaxed);
    const auto now = GetTickCount64();
    const auto timeout_ms = calls == 0 ? kFileSystemProbeIdleTimeoutMs : kFileSystemProbeActiveTimeoutMs;
    const bool timed_out =
        g_state.file_system_probe_armed_tick != 0 &&
        now >= g_state.file_system_probe_armed_tick &&
        (now - g_state.file_system_probe_armed_tick) >= timeout_ms;
    if (calls < kFileSystemProbeMaxCallsPerSlot && !timed_out)
    {
        return;
    }

    const auto previous_slot = g_state.slot;
    const auto previous_calls = calls;
    DeactivateFileSystemProbeLocked();

    std::wstring reason;
    if (TryActivateNextFileSystemProbeLocked(reason))
    {
        std::wstring line = L"GlobalResourceSwapper filesystem probe rotated after window expiry: previous_slot=0x";
        wchar_t slot_text[32] {};
        swprintf_s(slot_text, L"%zX", previous_slot);
        line += slot_text;
        line += L", previous_calls=";
        line += std::to_wstring(previous_calls);
        line += L".";
        nskinzcs2::Logger::Info(line);
        return;
    }

    std::wstring line = L"GlobalResourceSwapper filesystem probe exhausted VFileSystem candidate window: reason=";
    line += reason;
    line += L".";
    nskinzcs2::Logger::Warn(line);
}

bool TryResolveQuerySlot(void* resource_system_interface, void* query_match, std::size_t& out_slot, void*& out_target, std::wstring& issue)
{
    out_slot = kInvalidSlot;
    out_target = nullptr;
    issue.clear();

    const auto query_base = ResolveFunctionBase(query_match);
    if (query_base == nullptr)
    {
        issue = L"query function base unavailable";
        return false;
    }

    std::size_t matched_slot = kInvalidSlot;
    for (std::size_t slot = kSlotBegin; slot <= kSlotEndInclusive; ++slot)
    {
        void* target = nullptr;
        if (!TryValidateSlotTarget(resource_system_interface, slot, target))
        {
            continue;
        }

        if (ResolveFunctionBase(target) != query_base)
        {
            continue;
        }

        if (matched_slot != kInvalidSlot)
        {
            issue = L"query slot ambiguous";
            return false;
        }

        matched_slot = slot;
        out_target = target;
    }

    if (matched_slot == kInvalidSlot)
    {
        issue = L"query slot not found";
        return false;
    }

    out_slot = matched_slot;
    return true;
}

const char* TryResolveRedirectPath(const char* path) noexcept
{
    if (path == nullptr || reinterpret_cast<std::uintptr_t>(path) < 0x10000ull)
    {
        return nullptr;
    }

    __try
    {
        if (std::strstr(path, kButterflyWeaponPathC) != nullptr)
        {
            return kKarambitWeaponPathC;
        }
        if (std::strstr(path, kButterflyWeaponPath) != nullptr)
        {
            return kKarambitWeaponPath;
        }
        if (std::strstr(path, kButterflyWorldPathC) != nullptr)
        {
            return kKarambitWorldPathC;
        }
        if (std::strstr(path, kButterflyWorldPath) != nullptr)
        {
            return kKarambitWorldPath;
        }
        if (std::strstr(path, kButterflyViewPathC) != nullptr)
        {
            return kKarambitViewPathC;
        }
        if (std::strstr(path, kButterflyViewPath) != nullptr)
        {
            return kKarambitViewPath;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }

    return nullptr;
}

bool TryFindCarrierRedirectMatch(
    const char* text,
    const char*& out_needle,
    const char*& out_replacement,
    const char*& out_match) noexcept
{
    out_needle = nullptr;
    out_replacement = nullptr;
    out_match = nullptr;
    if (text == nullptr)
    {
        return false;
    }

    __try
    {
        if ((out_match = std::strstr(text, kButterflyWeaponPathC)) != nullptr)
        {
            out_needle = kButterflyWeaponPathC;
            out_replacement = kKarambitWeaponPathC;
            return true;
        }
        if ((out_match = std::strstr(text, kButterflyWeaponPath)) != nullptr)
        {
            out_needle = kButterflyWeaponPath;
            out_replacement = kKarambitWeaponPath;
            return true;
        }
        if ((out_match = std::strstr(text, kButterflyWorldPathC)) != nullptr)
        {
            out_needle = kButterflyWorldPathC;
            out_replacement = kKarambitWorldPathC;
            return true;
        }
        if ((out_match = std::strstr(text, kButterflyWorldPath)) != nullptr)
        {
            out_needle = kButterflyWorldPath;
            out_replacement = kKarambitWorldPath;
            return true;
        }
        if ((out_match = std::strstr(text, kButterflyViewPathC)) != nullptr)
        {
            out_needle = kButterflyViewPathC;
            out_replacement = kKarambitViewPathC;
            return true;
        }
        if ((out_match = std::strstr(text, kButterflyViewPath)) != nullptr)
        {
            out_needle = kButterflyViewPath;
            out_replacement = kKarambitViewPath;
            return true;
        }
        if ((out_match = std::strstr(text, kButterflyWeaponToken)) != nullptr)
        {
            out_needle = kButterflyWeaponToken;
            out_replacement = kKarambitWeaponToken;
            return true;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        out_needle = nullptr;
        out_replacement = nullptr;
        out_match = nullptr;
        return false;
    }

    return false;
}

void TryLogCarrierRedirectOnce(
    const char* label,
    const char* before,
    const char* after) noexcept
{
    static std::atomic<bool> s_logged {false};
    if (label == nullptr || before == nullptr || after == nullptr)
    {
        return;
    }

    if (!s_logged.exchange(true, std::memory_order_relaxed))
    {
        char line[768] {};
        sprintf_s(
            line,
            "[GlobalResourceSwapper] Query arg4 carrier redirected: field=%s before=%s after=%s",
            label,
            before,
            after);
        nskinzcs2::Logger::InfoAnsi(line);
    }
}

bool TryRewriteRedirectSubstringInPlace(
    char* text,
    const char* label) noexcept
{
    if (text == nullptr || reinterpret_cast<std::uintptr_t>(text) < 0x10000ull ||
        !IsReadableAddress(text) || !IsWritableAddress(text))
    {
        return false;
    }

    char sample[384] {};
    if (!TryCaptureAsciiSample(text, sample, std::size(sample)))
    {
        return false;
    }

    const char* needle = nullptr;
    const char* replacement = nullptr;
    const char* match = nullptr;
    if (!TryFindCarrierRedirectMatch(sample, needle, replacement, match) ||
        needle == nullptr || replacement == nullptr || match == nullptr)
    {
        return false;
    }

    const auto match_offset = static_cast<std::size_t>(match - sample);
    const auto replacement_length = std::strlen(replacement);
    const auto existing_length = std::strlen(match);
    if (replacement_length > existing_length)
    {
        return false;
    }

    char* live_match = text + match_offset;
    if (!IsWritableAddress(live_match) || !IsWritableAddress(live_match + replacement_length))
    {
        return false;
    }

    __try
    {
        std::memcpy(live_match, replacement, replacement_length + 1);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }

    TryLogCarrierRedirectOnce(label, sample, text);
    return true;
}

bool TryRedirectQueryArg4Carrier(const std::uint64_t arg4) noexcept
{
    const auto* root = reinterpret_cast<const std::uint8_t*>(arg4);
    if (root == nullptr || reinterpret_cast<std::uintptr_t>(root) < 0x10000ull || !IsReadableAddress(root))
    {
        return false;
    }

    bool redirected = false;
    const void* direct_path_address = nullptr;
    if (TryAddPointerOffset(root, static_cast<std::uintptr_t>(kQueryArg4DirectPathOffset), direct_path_address))
    {
        const auto* direct_path = reinterpret_cast<const char*>(direct_path_address);
        TryLogQueryCarrierPathSampleOnce("arg4{+0x10}", direct_path);
        redirected = TryRewriteRedirectSubstringInPlace(
            reinterpret_cast<char*>(const_cast<void*>(direct_path_address)),
            "arg4{+0x10}") || redirected;
    }

    const char* pointed_direct_path = nullptr;
    if (SafeReadValue(root + kQueryArg4DirectPathOffset, pointed_direct_path) &&
        pointed_direct_path != nullptr &&
        reinterpret_cast<std::uintptr_t>(pointed_direct_path) >= 0x10000ull)
    {
        TryLogQueryCarrierPathSampleOnce("arg4[*+0x10]", pointed_direct_path);
        redirected = TryRewriteRedirectSubstringInPlace(
            const_cast<char*>(pointed_direct_path),
            "arg4[*+0x10]") || redirected;
    }

    const void* carrier = nullptr;
    if (!SafeReadValue(root + kQueryCarrierOffset, carrier) || carrier == nullptr || !IsReadableAddress(carrier))
    {
        return redirected;
    }

    redirected = TryRewriteRedirectSubstringInPlace(
        reinterpret_cast<char*>(const_cast<void*>(carrier)),
        "arg4[*+0x60]") || redirected;

    const void* inline_path_address = nullptr;
    if (TryAddPointerOffset(carrier, static_cast<std::uintptr_t>(kQueryCarrierPathOffset), inline_path_address))
    {
        TryLogQueryCarrierPathSampleOnce(
            "arg4[*+0x60]{+0x18}",
            reinterpret_cast<const char*>(inline_path_address));
        redirected = TryRewriteRedirectSubstringInPlace(
            reinterpret_cast<char*>(const_cast<void*>(inline_path_address)),
            "arg4[*+0x60]{+0x18}") || redirected;
    }

    const char* pointed_path = nullptr;
    if (SafeReadValue(reinterpret_cast<const std::uint8_t*>(carrier) + kQueryCarrierPathOffset, pointed_path) &&
        pointed_path != nullptr && reinterpret_cast<std::uintptr_t>(pointed_path) >= 0x10000ull)
    {
        TryLogQueryCarrierPathSampleOnce("arg4[*+0x60][*+0x18]", pointed_path);
        redirected = TryRewriteRedirectSubstringInPlace(
            const_cast<char*>(pointed_path),
            "arg4[*+0x60][*+0x18]") || redirected;
    }

    return redirected;
}

bool TryCaptureAsciiSample(const char* address, char* out, const std::size_t out_size) noexcept
{
    if (out == nullptr || out_size < 8 || address == nullptr)
    {
        return false;
    }

    out[0] = '\0';
    if (reinterpret_cast<std::uintptr_t>(address) < 0x10000ull || !IsReadableAddress(address))
    {
        return false;
    }

    bool saw_printable = false;
    bool saw_alpha_numeric = false;
    std::size_t length = 0;

    for (; length + 1 < out_size; ++length)
    {
        char value = '\0';
        if (!SafeReadValue(address + length, value))
        {
            out[0] = '\0';
            return false;
        }

        if (value == '\0')
        {
            break;
        }

        const auto byte = static_cast<unsigned char>(value);
        if (byte < 0x20u || byte > 0x7Eu)
        {
            out[0] = '\0';
            return false;
        }

        out[length] = value;
        saw_printable = true;
        if ((byte >= '0' && byte <= '9') || (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z'))
        {
            saw_alpha_numeric = true;
        }
    }

    if (!saw_printable || !saw_alpha_numeric || length < 4 || length + 1 >= out_size)
    {
        out[0] = '\0';
        return false;
    }

    out[length] = '\0';
    return true;
}

bool TryCaptureWideSample(const wchar_t* address, wchar_t* out, const std::size_t out_size) noexcept
{
    if (out == nullptr || out_size < 8 || address == nullptr)
    {
        return false;
    }

    out[0] = L'\0';
    if (reinterpret_cast<std::uintptr_t>(address) < kMinUserModePointer || !IsReadableAddress(address))
    {
        return false;
    }

    bool saw_printable = false;
    bool saw_alpha_numeric = false;
    std::size_t length = 0;
    for (; length + 1 < out_size; ++length)
    {
        wchar_t value = L'\0';
        if (!SafeReadValue(address + length, value))
        {
            out[0] = L'\0';
            return false;
        }

        if (value == L'\0')
        {
            break;
        }

        if (value < 0x20 || value > 0x7E)
        {
            out[0] = L'\0';
            return false;
        }

        out[length] = value;
        saw_printable = true;
        if ((value >= L'0' && value <= L'9') || (value >= L'A' && value <= L'Z') || (value >= L'a' && value <= L'z'))
        {
            saw_alpha_numeric = true;
        }
    }

    if (!saw_printable || !saw_alpha_numeric || length < 4 || length + 1 >= out_size)
    {
        out[0] = L'\0';
        return false;
    }

    out[length] = L'\0';
    return true;
}

char ToLowerAsciiChar(const char value) noexcept
{
    return value >= 'A' && value <= 'Z'
        ? static_cast<char>(value - 'A' + 'a')
        : value;
}

wchar_t ToLowerWideChar(const wchar_t value) noexcept
{
    return value >= L'A' && value <= L'Z'
        ? static_cast<wchar_t>(value - L'A' + L'a')
        : value;
}

bool ContainsCaseInsensitive(const char* text, const char* token) noexcept
{
    if (text == nullptr || token == nullptr || token[0] == '\0')
    {
        return false;
    }

    for (std::size_t index = 0; text[index] != '\0'; ++index)
    {
        std::size_t token_index = 0;
        while (token[token_index] != '\0' &&
            text[index + token_index] != '\0' &&
            ToLowerAsciiChar(text[index + token_index]) == ToLowerAsciiChar(token[token_index]))
        {
            ++token_index;
        }

        if (token[token_index] == '\0')
        {
            return true;
        }
    }

    return false;
}

bool ContainsCaseInsensitive(const wchar_t* text, const wchar_t* token) noexcept
{
    if (text == nullptr || token == nullptr || token[0] == L'\0')
    {
        return false;
    }

    for (std::size_t index = 0; text[index] != L'\0'; ++index)
    {
        std::size_t token_index = 0;
        while (token[token_index] != L'\0' &&
            text[index + token_index] != L'\0' &&
            ToLowerWideChar(text[index + token_index]) == ToLowerWideChar(token[token_index]))
        {
            ++token_index;
        }

        if (token[token_index] == L'\0')
        {
            return true;
        }
    }

    return false;
}

bool LooksLikeKnifeModelPathSample(const char* sample) noexcept
{
    return sample != nullptr &&
        (std::strchr(sample, '/') != nullptr || std::strchr(sample, '\\') != nullptr) &&
        ContainsCaseInsensitive(sample, "knife") &&
        (ContainsCaseInsensitive(sample, ".vmdl") || ContainsCaseInsensitive(sample, ".vmdl_c"));
}

bool LooksLikeKnifeModelPathSample(const wchar_t* sample) noexcept
{
    return sample != nullptr &&
        (std::wcschr(sample, L'/') != nullptr || std::wcschr(sample, L'\\') != nullptr) &&
        ContainsCaseInsensitive(sample, L"knife") &&
        (ContainsCaseInsensitive(sample, L".vmdl") || ContainsCaseInsensitive(sample, L".vmdl_c"));
}

bool IsNoisyFileSystemSummaryOwner(const std::wstring& owner) noexcept
{
    if (owner.empty())
    {
        return true;
    }

    const auto lowered = ToLowerCopy(owner);
    return lowered == L"nskinzcs2.dll" ||
        lowered == L"filesystem_stdio.dll" ||
        lowered == L"tier0.dll" ||
        lowered == L"localize.dll" ||
        lowered == L"kernel32.dll" ||
        lowered == L"kernelbase.dll" ||
        lowered == L"ntdll.dll";
}

std::uint32_t CaptureCurrentUnwindFrames(std::array<const void*, kFileSystemAmbushFrameCapacity>& out_frames) noexcept
{
    for (auto& frame : out_frames)
    {
        frame = nullptr;
    }

    CONTEXT context {};
    RtlCaptureContext(&context);

    const HMODULE self_module = GetModuleFromAddress(reinterpret_cast<const void*>(&CaptureCurrentUnwindFrames));
    std::uint32_t count = 0;
    for (std::uint32_t attempt = 0; attempt < 96 && count < out_frames.size(); ++attempt)
    {
        if (!TryStepUnwindContext(context))
        {
            break;
        }

        const auto* frame = reinterpret_cast<const void*>(context.Rip);
        if (!IsLikelyUserModePointer(frame))
        {
            break;
        }

        if (GetModuleFromAddress(frame) == self_module)
        {
            continue;
        }

        if (count > 0 && out_frames[count - 1] == frame)
        {
            continue;
        }

        out_frames[count++] = frame;
    }

    return count;
}

bool TryCaptureFileSystemAmbushFromAnsiSample(const char* sample) noexcept
{
    std::uint64_t expected = 0;
    if (!g_state.file_system_capture_count.compare_exchange_strong(expected, 1, std::memory_order_acq_rel))
    {
        return false;
    }

    auto& capture = g_state.file_system_capture;
    capture = {};
    capture.thread_id = GetCurrentThreadId();
    capture.slot = g_state.slot;
    capture.wide = false;
    strncpy_s(capture.ansi_path, sample, _TRUNCATE);
    capture.frame_count = CaptureCurrentUnwindFrames(capture.frames);
    g_state.file_system_pending_flush.store(true, std::memory_order_release);
    return true;
}

bool TryCaptureFileSystemAmbushFromWideSample(const wchar_t* sample) noexcept
{
    std::uint64_t expected = 0;
    if (!g_state.file_system_capture_count.compare_exchange_strong(expected, 1, std::memory_order_acq_rel))
    {
        return false;
    }

    auto& capture = g_state.file_system_capture;
    capture = {};
    capture.thread_id = GetCurrentThreadId();
    capture.slot = g_state.slot;
    capture.wide = true;
    wcsncpy_s(capture.wide_path, sample, _TRUNCATE);
    capture.frame_count = CaptureCurrentUnwindFrames(capture.frames);
    g_state.file_system_pending_flush.store(true, std::memory_order_release);
    return true;
}

bool TryCaptureFileSystemAmbushFromValue(const std::uint64_t value) noexcept
{
    if (value < kMinUserModePointer)
    {
        return false;
    }

    const auto* candidate = reinterpret_cast<const void*>(value);
    if (!IsReadableAddress(candidate))
    {
        return false;
    }

    char ansi_sample[kFileSystemAmbushPathCapacity] {};
    if (TryCaptureAsciiSample(reinterpret_cast<const char*>(candidate), ansi_sample, std::size(ansi_sample)) &&
        LooksLikeKnifeModelPathSample(ansi_sample))
    {
        return TryCaptureFileSystemAmbushFromAnsiSample(ansi_sample);
    }

    wchar_t wide_sample[kFileSystemAmbushPathCapacity] {};
    if (TryCaptureWideSample(reinterpret_cast<const wchar_t*>(candidate), wide_sample, std::size(wide_sample)) &&
        LooksLikeKnifeModelPathSample(wide_sample))
    {
        return TryCaptureFileSystemAmbushFromWideSample(wide_sample);
    }

    return false;
}

void TryFlushFileSystemAmbushCapture() noexcept
{
    if (!g_state.file_system_pending_flush.load(std::memory_order_acquire) || g_state.file_system_flushed)
    {
        return;
    }

    FileSystemAmbushCapture capture {};
    {
        std::scoped_lock lock(g_state_mutex);
        if (!g_state.file_system_pending_flush.load(std::memory_order_acquire) || g_state.file_system_flushed)
        {
            return;
        }

        capture = g_state.file_system_capture;
        g_state.file_system_pending_flush.store(false, std::memory_order_release);
        g_state.file_system_flushed = true;
        g_state.file_system_capture_state.store(
            static_cast<std::uint32_t>(FileSystemCaptureState::Captured),
            std::memory_order_release);
    }

    if (capture.wide)
    {
        std::wstring line = L"FS_AMBUSH_CAPTURE: slot=0x";
        wchar_t slot_text[32] {};
        swprintf_s(slot_text, L"%zX", capture.slot);
        line += slot_text;
        line += L" | tid=";
        line += std::to_wstring(static_cast<std::uint64_t>(capture.thread_id));
        line += L" | path_kind=wide | path=";
        line += capture.wide_path;
        nskinzcs2::Logger::Info(line);
    }
    else
    {
        char line[1024] {};
        sprintf_s(
            line,
            "FS_AMBUSH_CAPTURE: slot=0x%zX | tid=%lu | path_kind=ansi | path=%s",
            capture.slot,
            static_cast<unsigned long>(capture.thread_id),
            capture.ansi_path);
        nskinzcs2::Logger::InfoAnsi(line);
    }

    bool summary_logged = false;
    for (std::uint32_t index = 0; index < capture.frame_count; ++index)
    {
        const auto* frame = capture.frames[index];
        const HMODULE module = GetModuleFromAddress(frame);
        const auto owner = GetModuleFileNameOnly(module);
        const auto frame_value = reinterpret_cast<std::uintptr_t>(frame);
        const auto module_value = reinterpret_cast<std::uintptr_t>(module);
        const auto module_offset = module_value != 0 && frame_value >= module_value
            ? static_cast<unsigned long long>(frame_value - module_value)
            : 0ull;

        std::wstring line = L"FS_AMBUSH_STACK[";
        line += std::to_wstring(index);
        line += L"]: address=";
        line += FormatPointer(frame);
        line += L" | owner=";
        line += owner.empty() ? std::wstring(L"<unknown>") : owner;
        line += L" | module_offset=0x";
        wchar_t offset_text[32] {};
        swprintf_s(offset_text, L"%llX", module_offset);
        line += offset_text;
        nskinzcs2::Logger::Info(line);

        if (!summary_logged && !IsNoisyFileSystemSummaryOwner(owner))
        {
            std::wstring summary = L"FS_AMBUSH_SUMMARY: candidate_frame=";
            summary += std::to_wstring(index);
            summary += L" | address=";
            summary += FormatPointer(frame);
            summary += L" | owner=";
            summary += owner;
            summary += L" | module_offset=0x";
            summary += offset_text;
            nskinzcs2::Logger::Info(summary);
            summary_logged = true;
        }
    }

    if (!summary_logged)
    {
        nskinzcs2::Logger::Warn(L"FS_AMBUSH_SUMMARY: no non-noisy wrapper candidate found above filesystem.");
    }
}

bool LooksLikeResourceSample(const char* sample) noexcept
{
    if (sample == nullptr)
    {
        return false;
    }

    return std::strstr(sample, ".vmdl") != nullptr ||
        std::strstr(sample, ".vmdl_c") != nullptr ||
        std::strstr(sample, "models/") != nullptr ||
        std::strstr(sample, "weapons/") != nullptr ||
        std::strstr(sample, "knife") != nullptr ||
        std::strstr(sample, ".vmat") != nullptr ||
        std::strstr(sample, ".vpcf") != nullptr;
}

bool LooksLikeResourceSample(const wchar_t* sample) noexcept
{
    if (sample == nullptr)
    {
        return false;
    }

    return ContainsCaseInsensitive(sample, L".vmdl") ||
        ContainsCaseInsensitive(sample, L".vmdl_c") ||
        ContainsCaseInsensitive(sample, L"models/") ||
        ContainsCaseInsensitive(sample, L"weapons/") ||
        ContainsCaseInsensitive(sample, L"knife") ||
        ContainsCaseInsensitive(sample, L".vmat") ||
        ContainsCaseInsensitive(sample, L".vpcf");
}

bool TryAppendFileSystemCarrierProbeSample(
    FileSystemCarrierProbeCapture& capture,
    const char* label,
    const void* pointer) noexcept
{
    if (label == nullptr || pointer == nullptr || capture.sample_count >= capture.samples.size())
    {
        return false;
    }

    if (!IsReadableAddress(pointer))
    {
        return false;
    }

    char ansi_sample[kFileSystemCarrierProbeSampleCapacity] {};
    if (TryCaptureAsciiSample(reinterpret_cast<const char*>(pointer), ansi_sample, std::size(ansi_sample)))
    {
        auto& sample = capture.samples[capture.sample_count++];
        strncpy_s(sample.label, label, _TRUNCATE);
        sample.pointer = pointer;
        sample.wide = false;
        sample.resource_like = LooksLikeResourceSample(ansi_sample);
        strncpy_s(sample.ansi, ansi_sample, _TRUNCATE);
        return true;
    }

    wchar_t wide_sample[kFileSystemCarrierProbeSampleCapacity] {};
    if (TryCaptureWideSample(reinterpret_cast<const wchar_t*>(pointer), wide_sample, std::size(wide_sample)))
    {
        auto& sample = capture.samples[capture.sample_count++];
        strncpy_s(sample.label, label, _TRUNCATE);
        sample.pointer = pointer;
        sample.wide = true;
        sample.resource_like = LooksLikeResourceSample(wide_sample);
        wcsncpy_s(sample.wide_text, wide_sample, _TRUNCATE);
        return true;
    }

    return false;
}

void TryCollectFileSystemCarrierProbeObjectSamples(
    FileSystemCarrierProbeCapture& capture,
    const char* label,
    const void* base) noexcept
{
    const auto initial_sample_count = capture.sample_count;
    constexpr std::uint32_t kMaxSamplesPerCarrier = 3;

    if (label == nullptr || base == nullptr || !IsReadableAddress(base) ||
        capture.sample_count >= capture.samples.size())
    {
        return;
    }

    TryAppendFileSystemCarrierProbeSample(capture, label, base);
    if (capture.sample_count >= capture.samples.size() ||
        (capture.sample_count - initial_sample_count) >= kMaxSamplesPerCarrier)
    {
        return;
    }

    for (const auto offset : kCarrierProbeOffsets)
    {
        if (capture.sample_count >= capture.samples.size() ||
            (capture.sample_count - initial_sample_count) >= kMaxSamplesPerCarrier)
        {
            break;
        }

        const void* nested = nullptr;
        if (!SafeReadValue(reinterpret_cast<const std::uint8_t*>(base) + offset, nested) ||
            nested == nullptr ||
            nested == base ||
            reinterpret_cast<std::uintptr_t>(nested) < kMinUserModePointer)
        {
            continue;
        }

        char pointer_label[kFileSystemCarrierProbeLabelCapacity] {};
        sprintf_s(pointer_label, "%s[*+0x%02zX]", label, offset);
        TryAppendFileSystemCarrierProbeSample(capture, pointer_label, nested);
    }

    for (const auto offset : kCarrierProbeOffsets)
    {
        if (capture.sample_count >= capture.samples.size() ||
            (capture.sample_count - initial_sample_count) >= kMaxSamplesPerCarrier)
        {
            break;
        }

        char inline_label[kFileSystemCarrierProbeLabelCapacity] {};
        sprintf_s(inline_label, "%s{+0x%02zX}", label, offset);
        TryAppendFileSystemCarrierProbeSample(
            capture,
            inline_label,
            reinterpret_cast<const std::uint8_t*>(base) + offset);
    }
}

void TryCaptureFileSystemCarrierProbe(
    void* this_ptr,
    std::uint64_t arg2,
    std::uint64_t arg3,
    std::uint64_t arg4,
    const void* return_address) noexcept
{
    if (this_ptr == nullptr ||
        g_state.slot != kFileSystemKnownHotSlotOffset ||
        g_state.file_system_carrier_probe_done.load(std::memory_order_relaxed))
    {
        return;
    }

    const auto attempt = g_state.file_system_carrier_probe_attempts.fetch_add(1, std::memory_order_relaxed) + 1;
    if (attempt > kFileSystemCarrierProbeMaxAttempts)
    {
        g_state.file_system_carrier_probe_done.store(true, std::memory_order_relaxed);
        return;
    }

    FileSystemCarrierProbeCapture capture {};
    capture.thread_id = GetCurrentThreadId();
    capture.slot = g_state.slot;
    capture.attempt = attempt;
    capture.entered = g_state.file_system_entered_count.load(std::memory_order_relaxed);
    capture.return_address = return_address;
    capture.arg2 = arg2;
    capture.arg3 = arg3;
    capture.arg4 = arg4;

    const auto* arg2_ptr = reinterpret_cast<const void*>(arg2);
    const auto* arg3_ptr = reinterpret_cast<const void*>(arg3);
    const auto* arg4_ptr = reinterpret_cast<const void*>(arg4);

    if (reinterpret_cast<std::uintptr_t>(arg2_ptr) >= kMinUserModePointer)
    {
        TryCollectFileSystemCarrierProbeObjectSamples(capture, "arg2", arg2_ptr);
    }
    if (reinterpret_cast<std::uintptr_t>(arg3_ptr) >= kMinUserModePointer)
    {
        TryCollectFileSystemCarrierProbeObjectSamples(capture, "arg3", arg3_ptr);
    }
    if (reinterpret_cast<std::uintptr_t>(arg4_ptr) >= kMinUserModePointer)
    {
        TryCollectFileSystemCarrierProbeObjectSamples(capture, "arg4", arg4_ptr);
    }

    if (capture.sample_count == 0 && attempt < kFileSystemCarrierProbeMaxAttempts)
    {
        return;
    }

    capture.exhausted_without_sample = capture.sample_count == 0;
    if (g_state.file_system_carrier_probe_done.exchange(true, std::memory_order_acq_rel))
    {
        return;
    }

    g_state.file_system_carrier_probe_capture = capture;
    g_state.file_system_carrier_probe_pending_flush.store(true, std::memory_order_release);
}

void TryFlushFileSystemCarrierProbe() noexcept
{
    if (!g_state.file_system_carrier_probe_pending_flush.load(std::memory_order_acquire) ||
        g_state.file_system_carrier_probe_flushed)
    {
        return;
    }

    FileSystemCarrierProbeCapture capture {};
    {
        std::scoped_lock lock(g_state_mutex);
        if (!g_state.file_system_carrier_probe_pending_flush.load(std::memory_order_acquire) ||
            g_state.file_system_carrier_probe_flushed)
        {
            return;
        }

        capture = g_state.file_system_carrier_probe_capture;
        g_state.file_system_carrier_probe_pending_flush.store(false, std::memory_order_release);
        g_state.file_system_carrier_probe_flushed = true;
    }

    CallerInfo caller_info {};
    const bool has_caller = ResolveCallerInfo(capture.return_address, caller_info);

    std::wstring header = L"FS_SLOT78_PROBE: attempt=";
    header += std::to_wstring(capture.attempt);
    header += L" slot=0x";
    wchar_t slot_text[32] {};
    swprintf_s(slot_text, L"%zX", capture.slot);
    header += slot_text;
    header += L" tid=";
    header += std::to_wstring(static_cast<std::uint64_t>(capture.thread_id));
    header += L" entered=";
    header += std::to_wstring(capture.entered);
    header += L" arg2=";
    header += FormatPointer(reinterpret_cast<const void*>(capture.arg2));
    header += L" arg3=";
    header += FormatPointer(reinterpret_cast<const void*>(capture.arg3));
    header += L" arg4=";
    header += FormatPointer(reinterpret_cast<const void*>(capture.arg4));
    header += L" ret=";
    header += FormatPointer(capture.return_address);
    header += L" owner=";
    header += has_caller && !caller_info.return_owner.empty() ? caller_info.return_owner : std::wstring(L"<unknown>");
    header += L" offset=0x";
    wchar_t offset_text[32] {};
    swprintf_s(offset_text, L"%llX", has_caller ? static_cast<unsigned long long>(caller_info.caller_offset) : 0ull);
    header += offset_text;
    header += L" samples=";
    header += std::to_wstring(capture.sample_count);
    header += L" exhausted=";
    header += capture.exhausted_without_sample ? L"1" : L"0";
    nskinzcs2::Logger::Info(header);

    for (std::uint32_t index = 0; index < capture.sample_count; ++index)
    {
        const auto& sample = capture.samples[index];
        if (sample.wide)
        {
            std::wstring line = L"FS_SLOT78_SAMPLE[";
            line += std::to_wstring(index);
            line += L"]: label=";
            wchar_t label[kFileSystemCarrierProbeLabelCapacity] {};
            size_t converted = 0;
            mbstowcs_s(&converted, label, std::size(label), sample.label, _TRUNCATE);
            line += label;
            line += L" | ptr=";
            line += FormatPointer(sample.pointer);
            line += L" | kind=wide | resource=";
            line += sample.resource_like ? L"1" : L"0";
            line += L" | sample=";
            line += sample.wide_text;
            nskinzcs2::Logger::Info(line);
            continue;
        }

        char line[512] {};
        sprintf_s(
            line,
            "FS_SLOT78_SAMPLE[%u]: label=%s | ptr=%p | kind=ansi | resource=%d | sample=%s",
            index,
            sample.label,
            sample.pointer,
            sample.resource_like ? 1 : 0,
            sample.ansi);
        nskinzcs2::Logger::InfoAnsi(line);
    }
}

constexpr std::uint32_t kQueryAbiProbeMaxAttempts = 48;
constexpr std::size_t kQueryAbiProbeMaxDepth = 3;
constexpr std::size_t kQueryAbiProbeMaxLogsPerAttempt = 12;
constexpr std::array<std::size_t, 13> kQueryAbiProbeOffsets {{
    0x00,
    0x08,
    0x10,
    0x18,
    0x20,
    0x28,
    0x30,
    0x38,
    0x40,
    0x48,
    0x50,
    0x58,
    0x60,
}};

constexpr std::uint32_t kAnimationCarrierProbeMaxAttempts = 24;
constexpr std::size_t kAnimationCarrierProbeMaxDepth = 3;
constexpr std::size_t kAnimationCarrierProbeMaxLogsPerAttempt = 10;

struct AnimationCarrierProbeContext
{
    std::uint32_t attempt = 0;
    bool logged_any_sample = false;
    bool logged_resource_sample = false;
    std::size_t logged_sample_count = 0;
    std::array<const void*, 48> visited {};
    std::size_t visited_count = 0;
};

bool TryVisitAnimationCarrierProbePointer(
    AnimationCarrierProbeContext& context,
    const void* pointer) noexcept
{
    if (pointer == nullptr || reinterpret_cast<std::uintptr_t>(pointer) < 0x10000ull)
    {
        return false;
    }

    for (std::size_t index = 0; index < context.visited_count; ++index)
    {
        if (context.visited[index] == pointer)
        {
            return false;
        }
    }

    if (context.visited_count >= context.visited.size())
    {
        return false;
    }

    context.visited[context.visited_count++] = pointer;
    return true;
}

void TryLogAnimationCarrierProbeCandidate(
    const char* label,
    const void* candidate,
    AnimationCarrierProbeContext& context) noexcept
{
    if (label == nullptr || candidate == nullptr ||
        context.logged_sample_count >= kAnimationCarrierProbeMaxLogsPerAttempt)
    {
        return;
    }

    char sample[128] {};
    if (!TryCaptureAsciiSample(reinterpret_cast<const char*>(candidate), sample, std::size(sample)))
    {
        return;
    }

    const bool resource_like = LooksLikeResourceSample(sample);
    char log_line[320] {};
    sprintf_s(
        log_line,
        "[GlobalResourceSwapper] Animation Slot120 Probe Sample[%s]: attempt=%u ptr=%p resource=%d sample=%s",
        label,
        context.attempt,
        candidate,
        resource_like ? 1 : 0,
        sample);
    nskinzcs2::Logger::InfoAnsi(log_line);
    context.logged_any_sample = true;
    ++context.logged_sample_count;
    if (resource_like)
    {
        context.logged_resource_sample = true;
    }
}

void TryLogAnimationCarrierProbeInlineField(
    const char* label,
    const void* base,
    const std::size_t offset,
    AnimationCarrierProbeContext& context) noexcept
{
    if (label == nullptr || base == nullptr || context.logged_resource_sample ||
        context.logged_sample_count >= kAnimationCarrierProbeMaxLogsPerAttempt)
    {
        return;
    }

    const auto address = reinterpret_cast<std::uintptr_t>(base);
    if (address < 0x10000ull || !IsReadableAddress(base))
    {
        return;
    }

    char field_label[128] {};
    sprintf_s(field_label, "%s{+0x%02zX}", label, offset);
    TryLogAnimationCarrierProbeCandidate(
        field_label,
        reinterpret_cast<const std::uint8_t*>(base) + offset,
        context);
}

void TryLogAnimationCarrierProbeObject(
    const char* label,
    const void* base,
    AnimationCarrierProbeContext& context,
    const std::size_t depth) noexcept;

void TryLogAnimationCarrierProbePointerField(
    const char* label,
    const void* base,
    const std::size_t offset,
    AnimationCarrierProbeContext& context,
    const std::size_t depth) noexcept
{
    if (label == nullptr || base == nullptr || context.logged_resource_sample ||
        context.logged_sample_count >= kAnimationCarrierProbeMaxLogsPerAttempt)
    {
        return;
    }

    const auto address = reinterpret_cast<std::uintptr_t>(base);
    if (address < 0x10000ull || !IsReadableAddress(base))
    {
        return;
    }

    const void* nested = nullptr;
    if (!SafeReadValue(reinterpret_cast<const std::uint8_t*>(base) + offset, nested) || nested == nullptr)
    {
        return;
    }

    if (nested == base || reinterpret_cast<std::uintptr_t>(nested) < 0x10000ull)
    {
        return;
    }

    char field_label[128] {};
    sprintf_s(field_label, "%s[*+0x%02zX]", label, offset);
    TryLogAnimationCarrierProbeCandidate(field_label, nested, context);

    if (context.logged_resource_sample || depth + 1 >= kAnimationCarrierProbeMaxDepth)
    {
        return;
    }

    TryLogAnimationCarrierProbeObject(field_label, nested, context, depth + 1);
}

void TryLogAnimationCarrierProbeObject(
    const char* label,
    const void* base,
    AnimationCarrierProbeContext& context,
    const std::size_t depth) noexcept
{
    if (label == nullptr || base == nullptr ||
        depth >= kAnimationCarrierProbeMaxDepth ||
        context.logged_resource_sample)
    {
        return;
    }

    if (!IsReadableAddress(base) || !TryVisitAnimationCarrierProbePointer(context, base))
    {
        return;
    }

    TryLogAnimationCarrierProbeCandidate(label, base, context);
    if (context.logged_resource_sample ||
        context.logged_sample_count >= kAnimationCarrierProbeMaxLogsPerAttempt)
    {
        return;
    }

    for (const auto offset : kQueryAbiProbeOffsets)
    {
        if (context.logged_resource_sample ||
            context.logged_sample_count >= kAnimationCarrierProbeMaxLogsPerAttempt)
        {
            break;
        }

        TryLogAnimationCarrierProbeInlineField(label, base, offset, context);
        if (context.logged_resource_sample ||
            context.logged_sample_count >= kAnimationCarrierProbeMaxLogsPerAttempt)
        {
            break;
        }

        TryLogAnimationCarrierProbePointerField(label, base, offset, context, depth);
    }
}

void TryLogAnimationCarrierProbeWindow(
    void* this_ptr,
    const std::uint64_t arg2,
    const std::uint64_t arg3,
    const std::uint64_t arg4) noexcept
{
    static std::atomic<std::uint32_t> s_attempts {0};
    static std::atomic<bool> s_resource_found {false};
    static std::atomic<bool> s_exhausted_logged {false};
    thread_local bool s_inside_probe = false;

    if (s_inside_probe || s_resource_found.load(std::memory_order_relaxed))
    {
        return;
    }

    const auto attempt = s_attempts.fetch_add(1, std::memory_order_relaxed) + 1;
    if (attempt > kAnimationCarrierProbeMaxAttempts)
    {
        if (!s_exhausted_logged.exchange(true, std::memory_order_relaxed))
        {
            char exhausted[224] {};
            sprintf_s(
                exhausted,
                "[GlobalResourceSwapper] Animation Slot120 Probe Window exhausted after %u attempts without a resource-like carrier.",
                kAnimationCarrierProbeMaxAttempts);
            nskinzcs2::Logger::InfoAnsi(exhausted);
        }
        return;
    }

    s_inside_probe = true;
    AnimationCarrierProbeContext context {};
    context.attempt = attempt;
    TryLogAnimationCarrierProbeObject("anim.this", this_ptr, context, 0);
    TryLogAnimationCarrierProbeObject("anim.arg2", reinterpret_cast<const void*>(arg2), context, 0);
    TryLogAnimationCarrierProbeObject("anim.arg3", reinterpret_cast<const void*>(arg3), context, 0);
    TryLogAnimationCarrierProbeObject("anim.arg4", reinterpret_cast<const void*>(arg4), context, 0);

    if (context.logged_resource_sample)
    {
        s_resource_found.store(true, std::memory_order_relaxed);
    }
    else if (!context.logged_any_sample && attempt == kAnimationCarrierProbeMaxAttempts &&
             !s_exhausted_logged.exchange(true, std::memory_order_relaxed))
    {
        char empty[224] {};
        sprintf_s(
            empty,
            "[GlobalResourceSwapper] Animation Slot120 Probe Window saw no readable ASCII carriers in %u attempts.",
            kAnimationCarrierProbeMaxAttempts);
        nskinzcs2::Logger::InfoAnsi(empty);
    }

    s_inside_probe = false;
}

struct QueryAbiProbeContext
{
    std::uint32_t attempt = 0;
    bool logged_any_sample = false;
    bool logged_resource_sample = false;
    std::size_t logged_sample_count = 0;
    std::array<const void*, 48> visited {};
    std::size_t visited_count = 0;
};

bool TryVisitQueryAbiProbePointer(QueryAbiProbeContext& context, const void* pointer) noexcept
{
    if (pointer == nullptr || reinterpret_cast<std::uintptr_t>(pointer) < 0x10000ull)
    {
        return false;
    }

    for (std::size_t index = 0; index < context.visited_count; ++index)
    {
        if (context.visited[index] == pointer)
        {
            return false;
        }
    }

    if (context.visited_count >= context.visited.size())
    {
        return false;
    }

    context.visited[context.visited_count++] = pointer;
    return true;
}

void TryLogQueryAbiProbeCandidate(
    const char* label,
    const void* candidate,
    QueryAbiProbeContext& context) noexcept
{
    if (label == nullptr || candidate == nullptr || context.logged_sample_count >= kQueryAbiProbeMaxLogsPerAttempt)
    {
        return;
    }

    char sample[128] {};
    if (!TryCaptureAsciiSample(reinterpret_cast<const char*>(candidate), sample, std::size(sample)))
    {
        return;
    }

    const bool resource_like = LooksLikeResourceSample(sample);
    char log_line[320] {};
    sprintf_s(
        log_line,
        "[GlobalResourceSwapper] Query ABI Probe Sample[%s]: attempt=%u ptr=%p resource=%d sample=%s",
        label,
        context.attempt,
        candidate,
        resource_like ? 1 : 0,
        sample);
    nskinzcs2::Logger::InfoAnsi(log_line);
    context.logged_any_sample = true;
    ++context.logged_sample_count;
    if (resource_like)
    {
        context.logged_resource_sample = true;
    }
}

void TryLogQueryAbiProbeInlineField(
    const char* label,
    const void* base,
    const std::size_t offset,
    QueryAbiProbeContext& context) noexcept
{
    if (label == nullptr || base == nullptr || context.logged_resource_sample || context.logged_sample_count >= kQueryAbiProbeMaxLogsPerAttempt)
    {
        return;
    }

    const auto address = reinterpret_cast<std::uintptr_t>(base);
    if (address < 0x10000ull || !IsReadableAddress(base))
    {
        return;
    }

    char field_label[128] {};
    sprintf_s(field_label, "%s{+0x%02zX}", label, offset);
    TryLogQueryAbiProbeCandidate(
        field_label,
        reinterpret_cast<const std::uint8_t*>(base) + offset,
        context);
}

void TryLogQueryAbiProbePointerField(
    const char* label,
    const void* base,
    const std::size_t offset,
    QueryAbiProbeContext& context,
    const std::size_t depth) noexcept
{
    if (label == nullptr || base == nullptr || context.logged_resource_sample || context.logged_sample_count >= kQueryAbiProbeMaxLogsPerAttempt)
    {
        return;
    }

    const auto address = reinterpret_cast<std::uintptr_t>(base);
    if (address < 0x10000ull || !IsReadableAddress(base))
    {
        return;
    }

    const void* nested = nullptr;
    if (!SafeReadValue(reinterpret_cast<const std::uint8_t*>(base) + offset, nested) || nested == nullptr)
    {
        return;
    }

    if (nested == base || reinterpret_cast<std::uintptr_t>(nested) < 0x10000ull)
    {
        return;
    }

    char field_label[128] {};
    sprintf_s(field_label, "%s[*+0x%02zX]", label, offset);
    TryLogQueryAbiProbeCandidate(field_label, nested, context);

    if (context.logged_resource_sample || depth + 1 >= kQueryAbiProbeMaxDepth)
    {
        return;
    }

    if (IsReadableAddress(nested))
    {
        for (const auto nested_offset : kQueryAbiProbeOffsets)
        {
            if (context.logged_resource_sample || context.logged_sample_count >= kQueryAbiProbeMaxLogsPerAttempt)
            {
                break;
            }

            TryLogQueryAbiProbeInlineField(field_label, nested, nested_offset, context);
            if (context.logged_resource_sample || context.logged_sample_count >= kQueryAbiProbeMaxLogsPerAttempt)
            {
                break;
            }

            TryLogQueryAbiProbePointerField(field_label, nested, nested_offset, context, depth + 1);
        }
    }
}

void TryLogQueryAbiProbeObject(
    const char* label,
    const void* base,
    QueryAbiProbeContext& context,
    const std::size_t depth) noexcept
{
    if (label == nullptr || base == nullptr || depth >= kQueryAbiProbeMaxDepth || context.logged_resource_sample)
    {
        return;
    }

    if (!IsReadableAddress(base) || !TryVisitQueryAbiProbePointer(context, base))
    {
        return;
    }

    TryLogQueryAbiProbeCandidate(label, base, context);
    if (context.logged_resource_sample || context.logged_sample_count >= kQueryAbiProbeMaxLogsPerAttempt)
    {
        return;
    }

    for (const auto offset : kQueryAbiProbeOffsets)
    {
        if (context.logged_resource_sample || context.logged_sample_count >= kQueryAbiProbeMaxLogsPerAttempt)
        {
            break;
        }

        TryLogQueryAbiProbeInlineField(label, base, offset, context);
        if (context.logged_resource_sample || context.logged_sample_count >= kQueryAbiProbeMaxLogsPerAttempt)
        {
            break;
        }

        TryLogQueryAbiProbePointerField(label, base, offset, context, depth);
    }
}

constexpr std::uint32_t kEngine2CallerProbeMaxAttempts = 48;
constexpr std::array<std::size_t, 8> kEngine2CallerStackPointerOffsets {{
    0x28,
    0x30,
    0x38,
    0x40,
    0x48,
    0x50,
    0x58,
    0x60,
}};

struct Engine2CallerProbeContext
{
    std::uint32_t attempt = 0;
    bool logged_any_sample = false;
    bool logged_resource_sample = false;
    std::size_t logged_sample_count = 0;
    std::array<const void*, 64> visited {};
    std::size_t visited_count = 0;
};

bool TryVisitEngine2CallerProbePointer(Engine2CallerProbeContext& context, const void* pointer) noexcept
{
    if (pointer == nullptr || reinterpret_cast<std::uintptr_t>(pointer) < 0x10000ull)
    {
        return false;
    }

    for (std::size_t index = 0; index < context.visited_count; ++index)
    {
        if (context.visited[index] == pointer)
        {
            return false;
        }
    }

    if (context.visited_count >= context.visited.size())
    {
        return false;
    }

    context.visited[context.visited_count++] = pointer;
    return true;
}

void TryLogEngine2CallerProbeCandidate(
    const char* label,
    const void* candidate,
    Engine2CallerProbeContext& context) noexcept
{
    if (label == nullptr || candidate == nullptr || context.logged_sample_count >= kQueryAbiProbeMaxLogsPerAttempt)
    {
        return;
    }

    char sample[128] {};
    if (!TryCaptureAsciiSample(reinterpret_cast<const char*>(candidate), sample, std::size(sample)))
    {
        return;
    }

    const bool resource_like = LooksLikeResourceSample(sample);
    char log_line[352] {};
    sprintf_s(
        log_line,
        "[GlobalResourceSwapper] Engine2 Caller Probe Sample[%s]: attempt=%u ptr=%p resource=%d sample=%s",
        label,
        context.attempt,
        candidate,
        resource_like ? 1 : 0,
        sample);
    nskinzcs2::Logger::InfoAnsi(log_line);
    context.logged_any_sample = true;
    ++context.logged_sample_count;
    if (resource_like)
    {
        context.logged_resource_sample = true;
    }
}

void TryLogEngine2CallerProbeInlineField(
    const char* label,
    const void* base,
    const std::size_t offset,
    Engine2CallerProbeContext& context) noexcept
{
    if (label == nullptr || base == nullptr || context.logged_resource_sample ||
        context.logged_sample_count >= kQueryAbiProbeMaxLogsPerAttempt)
    {
        return;
    }

    if (!IsReadableAddress(base))
    {
        return;
    }

    char field_label[128] {};
    sprintf_s(field_label, "%s{+0x%02zX}", label, offset);
    TryLogEngine2CallerProbeCandidate(
        field_label,
        reinterpret_cast<const std::uint8_t*>(base) + offset,
        context);
}

void TryLogEngine2CallerProbeObject(
    const char* label,
    const void* base,
    Engine2CallerProbeContext& context,
    const std::size_t depth) noexcept;

void TryLogEngine2CallerProbePointerField(
    const char* label,
    const void* base,
    const std::size_t offset,
    Engine2CallerProbeContext& context,
    const std::size_t depth) noexcept
{
    if (label == nullptr || base == nullptr || context.logged_resource_sample ||
        context.logged_sample_count >= kQueryAbiProbeMaxLogsPerAttempt)
    {
        return;
    }

    if (!IsReadableAddress(base))
    {
        return;
    }

    const void* nested = nullptr;
    if (!SafeReadValue(reinterpret_cast<const std::uint8_t*>(base) + offset, nested) || nested == nullptr)
    {
        return;
    }

    if (nested == base || reinterpret_cast<std::uintptr_t>(nested) < 0x10000ull)
    {
        return;
    }

    char field_label[128] {};
    sprintf_s(field_label, "%s[*+0x%02zX]", label, offset);
    TryLogEngine2CallerProbeCandidate(field_label, nested, context);
    if (context.logged_resource_sample || depth + 1 >= kQueryAbiProbeMaxDepth)
    {
        return;
    }

    TryLogEngine2CallerProbeObject(field_label, nested, context, depth + 1);
}

void TryLogEngine2CallerProbeObject(
    const char* label,
    const void* base,
    Engine2CallerProbeContext& context,
    const std::size_t depth) noexcept
{
    if (label == nullptr || base == nullptr || depth >= kQueryAbiProbeMaxDepth || context.logged_resource_sample)
    {
        return;
    }

    if (!IsReadableAddress(base) || !TryVisitEngine2CallerProbePointer(context, base))
    {
        return;
    }

    TryLogEngine2CallerProbeCandidate(label, base, context);
    if (context.logged_resource_sample || context.logged_sample_count >= kQueryAbiProbeMaxLogsPerAttempt)
    {
        return;
    }

    for (const auto offset : kQueryAbiProbeOffsets)
    {
        if (context.logged_resource_sample || context.logged_sample_count >= kQueryAbiProbeMaxLogsPerAttempt)
        {
            break;
        }

        TryLogEngine2CallerProbeInlineField(label, base, offset, context);
        if (context.logged_resource_sample || context.logged_sample_count >= kQueryAbiProbeMaxLogsPerAttempt)
        {
            break;
        }

        TryLogEngine2CallerProbePointerField(label, base, offset, context, depth);
    }
}

void TryLogEngine2CallerStackPointerSlot(
    const std::uintptr_t original_rsp,
    const std::size_t offset,
    Engine2CallerProbeContext& context) noexcept
{
    if (original_rsp < 0x10000ull)
    {
        return;
    }

    const auto slot_address = reinterpret_cast<const void*>(original_rsp + offset);
    if (!IsReadableAddress(slot_address))
    {
        return;
    }

    const void* pointer_value = nullptr;
    if (!SafeReadValue(slot_address, pointer_value) || pointer_value == nullptr)
    {
        return;
    }

    char label[96] {};
    sprintf_s(label, "stack_ptr[+0x%02zX]", offset);
    TryLogEngine2CallerProbeCandidate(label, pointer_value, context);
    if (context.logged_resource_sample || reinterpret_cast<std::uintptr_t>(pointer_value) < 0x10000ull)
    {
        return;
    }

    TryLogEngine2CallerProbeObject(label, pointer_value, context, 0);
}

void LogCallerInfoSnapshotAnsi(const char* prefix, const CallerInfo& info) noexcept
{
    if (prefix == nullptr)
    {
        return;
    }

    char return_owner_ansi[64] {};
    char caller_owner_ansi[64] {};
    char return_bytes[3 * kMachineByteCount + 8] {};
    char caller_bytes[3 * kMachineByteCount + 8] {};
    char line[640] {};

    WideToAnsiLossy(info.return_owner, return_owner_ansi, std::size(return_owner_ansi));
    WideToAnsiLossy(info.caller_owner, caller_owner_ansi, std::size(caller_owner_ansi));
    FormatMachineBytesAnsi(info.before_return, kMachineByteCount, return_bytes, std::size(return_bytes));
    FormatMachineBytesAnsi(info.caller_base, kMachineByteCount, caller_bytes, std::size(caller_bytes));

    sprintf_s(
        line,
        "%s: ret=%p ret_owner=%s caller_base=%p caller_owner=%s caller_offset=0x%llX",
        prefix,
        info.return_address,
        return_owner_ansi,
        info.caller_base,
        caller_owner_ansi,
        static_cast<unsigned long long>(info.caller_offset));
    nskinzcs2::Logger::InfoAnsi(line);

    sprintf_s(
        line,
        "%s Bytes[ret-32..ret): base=%p bytes=%s",
        prefix,
        info.before_return,
        return_bytes);
    nskinzcs2::Logger::InfoAnsi(line);

    sprintf_s(
        line,
        "%s Bytes[caller_base]: base=%p bytes=%s",
        prefix,
        info.caller_base,
        caller_bytes);
    nskinzcs2::Logger::InfoAnsi(line);
}

void TryLogEngine2PrimaryCallerChainOnce(const CallerInfo& info) noexcept
{
    static std::atomic<bool> s_logged {false};
    if (!s_logged.exchange(true, std::memory_order_relaxed))
    {
        LogCallerInfoSnapshotAnsi("[GlobalResourceSwapper] Engine2 Primary Caller Chain", info);
    }
}

void TryLogEngine2UpstreamCallerChainOnce(const CallerInfo& info) noexcept
{
    static std::atomic<bool> s_logged {false};
    if (!s_logged.exchange(true, std::memory_order_relaxed))
    {
        LogCallerInfoSnapshotAnsi("[GlobalResourceSwapper] Engine2 Upstream Caller Chain", info);
    }
}

void TryLogEngine2UpstreamProbeQueuedOnce() noexcept
{
    static std::atomic<bool> s_logged {false};
    if (!s_logged.exchange(true, std::memory_order_relaxed))
    {
        nskinzcs2::Logger::Info(
            L"GlobalResourceSwapper engine2 upstream caller chain is queued for passive identity analysis; live wrapper dispatch snapshots are now derived from the existing primary probe to avoid early-load CTD.");
    }
}

int RankAnimationSlot120CallerOwner(const std::wstring& owner) noexcept
{
    if (owner.empty())
    {
        return 99;
    }

    const auto lowered = ToLowerCopy(owner);
    if (lowered == L"animationsystem.dll")
    {
        return 0;
    }
    if (lowered.find(L"anim") != std::wstring::npos)
    {
        return 1;
    }
    if (lowered == L"client.dll" ||
        lowered == L"resourcesystem.dll" ||
        lowered.find(L"scene") != std::wstring::npos ||
        lowered.find(L"model") != std::wstring::npos)
    {
        return 2;
    }
    if (lowered == L"engine2.dll")
    {
        return 3;
    }
    if (lowered == L"cs2.exe")
    {
        return 4;
    }
    return 5;
}

void TryTrackAnimationSlot120CallerSource(const CallerInfo& info) noexcept
{
    static std::atomic<bool> s_capacity_logged {false};
    static std::atomic<bool> s_primary_reuse_logged {false};

    if (info.return_address == nullptr ||
        info.caller_base == nullptr ||
        IsNoisyEngine2UpstreamOwner(info.return_owner) ||
        IsNoisyEngine2UpstreamOwner(info.caller_owner))
    {
        return;
    }

    void* primary_target = nullptr;
    {
        std::scoped_lock lock(g_state_mutex);
        primary_target = g_state.engine2_probe_target;
    }

    if (primary_target != nullptr && info.caller_base == primary_target)
    {
        if (!s_primary_reuse_logged.exchange(true, std::memory_order_relaxed))
        {
            nskinzcs2::Logger::Info(
                L"GlobalResourceSwapper animation slot120 caller converged to the live engine2 primary probe target; reuse the primary capture and continue collecting caller ancestry instead of arming a duplicate hook.");
        }

        TryCorrelateAnimationSlot120WithPrimaryCapture(info);
        return;
    }

    bool new_candidate = false;
    std::size_t candidate_index = kInvalidSlot;
    std::uint32_t hits = 0;
    {
        std::scoped_lock lock(g_state_mutex);
        for (std::size_t index = 0; index < g_state.animation_slot120_caller_candidate_count; ++index)
        {
            if (g_state.animation_slot120_caller_candidates[index].caller_base == info.caller_base)
            {
                candidate_index = index;
                break;
            }
        }

        if (candidate_index == kInvalidSlot)
        {
            if (g_state.animation_slot120_caller_candidate_count >= g_state.animation_slot120_caller_candidates.size())
            {
                if (!s_capacity_logged.exchange(true, std::memory_order_relaxed))
                {
                    nskinzcs2::Logger::Warn(
                        L"GlobalResourceSwapper animation slot120 caller candidate capacity is full; newer distinct wrappers will no longer be tracked.");
                }
                return;
            }

            candidate_index = g_state.animation_slot120_caller_candidate_count++;
            auto& candidate = g_state.animation_slot120_caller_candidates[candidate_index];
            candidate = {};
            candidate.caller_base = info.caller_base;
            candidate.sample_return_address = info.return_address;
            candidate.caller_offset = info.caller_offset;
            candidate.sample_depth = info.stack_depth;
            candidate.hits = 1;
            hits = candidate.hits;
            new_candidate = true;
        }
        else
        {
            auto& candidate = g_state.animation_slot120_caller_candidates[candidate_index];
            ++candidate.hits;
            hits = candidate.hits;
        }
    }

    if (!new_candidate)
    {
        return;
    }

    char prefix[96] {};
    sprintf_s(
        prefix,
        "[GlobalResourceSwapper] Animation Slot120 Caller Candidate[%zu]",
        candidate_index + 1);
    LogCallerInfoSnapshotAnsi(prefix, info);

    char summary[256] {};
    sprintf_s(
        summary,
        "[GlobalResourceSwapper] Animation Slot120 Caller Candidate[%zu]: owner_rank=%d hits=%u",
        candidate_index + 1,
        RankAnimationSlot120CallerOwner(info.caller_owner),
        hits);
    nskinzcs2::Logger::InfoAnsi(summary);
}

bool TryQueueNextAnimationSlot120CallerCandidate() noexcept
{
    std::array<CallerInfo, kAnimationSlot120CallerCandidateCapacity> ordered_infos {};
    std::array<std::size_t, kAnimationSlot120CallerCandidateCapacity> ordered_indices {};
    std::size_t ordered_count = 0;
    void* primary_target = nullptr;

    {
        std::scoped_lock lock(g_state_mutex);
        if (!g_state.installed ||
            g_state.animation_slot120_probe_active ||
            g_state.animation_slot120_probe_pending)
        {
            return false;
        }

        primary_target = g_state.engine2_probe_target;

        for (std::size_t index = 0; index < g_state.animation_slot120_caller_candidate_count; ++index)
        {
            auto& candidate = g_state.animation_slot120_caller_candidates[index];
            CallerInfo info {};
            if (candidate.queued || candidate.attempted || candidate.success ||
                !TryBuildCallerInfoFromCandidate(candidate, info) ||
                IsNoisyEngine2UpstreamOwner(info.return_owner) ||
                IsNoisyEngine2UpstreamOwner(info.caller_owner))
            {
                continue;
            }

            if (primary_target != nullptr && candidate.caller_base == primary_target)
            {
                candidate.attempted = true;
                candidate.queued = false;
                continue;
            }

            ordered_infos[ordered_count] = std::move(info);
            ordered_indices[ordered_count] = index;
            ++ordered_count;
        }
    }

    if (ordered_count == 0)
    {
        return false;
    }

    std::size_t best_pos = kInvalidSlot;
    int best_rank = 99;
    std::uint32_t best_hits = 0;
    for (std::size_t pos = 0; pos < ordered_count; ++pos)
    {
        const auto& info = ordered_infos[pos];
        const int rank = RankAnimationSlot120CallerOwner(info.caller_owner);
        const auto hits = [&]() -> std::uint32_t
        {
            std::scoped_lock lock(g_state_mutex);
            return ordered_indices[pos] < g_state.animation_slot120_caller_candidate_count
                ? g_state.animation_slot120_caller_candidates[ordered_indices[pos]].hits
                : 0u;
        }();

        if (best_pos == kInvalidSlot ||
            rank < best_rank ||
            (rank == best_rank && hits > best_hits))
        {
            best_pos = pos;
            best_rank = rank;
            best_hits = hits;
        }
    }

    if (best_pos == kInvalidSlot)
    {
        return false;
    }

    const auto& best_info = ordered_infos[best_pos];
    const auto best_index = ordered_indices[best_pos];
    {
        std::scoped_lock lock(g_state_mutex);
        if (!g_state.installed ||
            g_state.animation_slot120_probe_active ||
            g_state.animation_slot120_probe_pending ||
            best_index >= g_state.animation_slot120_caller_candidate_count)
        {
            return false;
        }

        auto& candidate = g_state.animation_slot120_caller_candidates[best_index];
        if (candidate.queued || candidate.attempted || candidate.success)
        {
            return false;
        }

        g_state.animation_slot120_probe_pending_return = best_info.return_address;
        g_state.animation_slot120_probe_pending_target = best_info.caller_base;
        g_state.animation_slot120_probe_pending_offset = best_info.caller_offset;
        g_state.animation_slot120_probe_pending = true;
        g_state.animation_slot120_source_candidate_index = best_index;
        candidate.queued = true;
    }

    char line[320] {};
    sprintf_s(
        line,
        "[GlobalResourceSwapper] Animation Slot120 caller probe queued: ret=%p target=%p owner_rank=%d candidate=%zu hits=%u",
        best_info.return_address,
        best_info.caller_base,
        best_rank,
        best_index + 1,
        best_hits);
    nskinzcs2::Logger::InfoAnsi(line);
    return true;
}

void TryLogAnimationSlot120CallerProbeWindow(const Engine2EntryCapture* capture) noexcept
{
    if (capture == nullptr || g_state.animation_slot120_probe_window_resource_found)
    {
        return;
    }

    const auto attempt = ++g_state.animation_slot120_probe_window_attempts;
    if (attempt > kEngine2CallerProbeMaxAttempts)
    {
        if (!g_state.animation_slot120_probe_window_exhausted_logged)
        {
            g_state.animation_slot120_probe_window_exhausted_logged = true;
            char exhausted_log[224] {};
            sprintf_s(
                exhausted_log,
                "[GlobalResourceSwapper] Animation Slot120 Caller Probe Window exhausted after %u attempts without a resource-like carrier.",
                kEngine2CallerProbeMaxAttempts);
            nskinzcs2::Logger::InfoAnsi(exhausted_log);
        }
        g_state.animation_slot120_probe_remove_requested.store(true, std::memory_order_relaxed);
        return;
    }

    const auto original_rsp = static_cast<std::uintptr_t>(capture->original_rsp);
    const void* return_address = nullptr;
    if (original_rsp >= kMinUserModePointer)
    {
        SafeReadValue(reinterpret_cast<const void*>(original_rsp), return_address);
    }

    CallerInfo info {};
    const bool have_info = ResolveCallerInfo(return_address, info);
    if (have_info && attempt == 1)
    {
        LogCallerInfoSnapshotAnsi("[GlobalResourceSwapper] Animation Slot120 Caller Probe", info);
    }

    char return_owner_ansi[64] {};
    char caller_owner_ansi[64] {};
    char header[512] {};
    WideToAnsiLossy(
        have_info ? info.return_owner : GetModuleFileNameOnly(GetModuleFromAddress(return_address)),
        return_owner_ansi,
        std::size(return_owner_ansi));
    WideToAnsiLossy(have_info ? info.caller_owner : std::wstring {}, caller_owner_ansi, std::size(caller_owner_ansi));
    sprintf_s(
        header,
        "[GlobalResourceSwapper] Animation Slot120 Caller Probe: attempt=%u entered=%llu target=%p ret=%p ret_owner=%s caller_base=%p caller_owner=%s caller_offset=0x%llX rax=%p rcx=%p rdx=%p r8=%p r9=%p",
        attempt,
        static_cast<unsigned long long>(g_state.animation_slot120_probe_entered_count.load(std::memory_order_relaxed)),
        g_state.animation_slot120_probe_target,
        return_address,
        return_owner_ansi,
        have_info ? info.caller_base : nullptr,
        caller_owner_ansi,
        static_cast<unsigned long long>(have_info ? info.caller_offset : 0ull),
        reinterpret_cast<const void*>(capture->rax),
        reinterpret_cast<const void*>(capture->rcx),
        reinterpret_cast<const void*>(capture->rdx),
        reinterpret_cast<const void*>(capture->r8),
        reinterpret_cast<const void*>(capture->r9));
    nskinzcs2::Logger::InfoAnsi(header);

    Engine2CallerProbeContext context {};
    context.attempt = attempt;

    const auto* rax_ptr = reinterpret_cast<const void*>(capture->rax);
    if (rax_ptr != nullptr && reinterpret_cast<std::uintptr_t>(rax_ptr) >= kMinUserModePointer)
    {
        TryLogEngine2CallerProbeObject("anim_caller.rax", rax_ptr, context, 0);
    }

    const auto* rcx_ptr = reinterpret_cast<const void*>(capture->rcx);
    if (!context.logged_resource_sample && rcx_ptr != nullptr && reinterpret_cast<std::uintptr_t>(rcx_ptr) >= kMinUserModePointer)
    {
        TryLogEngine2CallerProbeObject("anim_caller.rcx", rcx_ptr, context, 0);
    }

    const auto* rdx_ptr = reinterpret_cast<const void*>(capture->rdx);
    if (!context.logged_resource_sample && rdx_ptr != nullptr && reinterpret_cast<std::uintptr_t>(rdx_ptr) >= kMinUserModePointer)
    {
        TryLogEngine2CallerProbeObject("anim_caller.rdx", rdx_ptr, context, 0);
    }

    const auto* r8_ptr = reinterpret_cast<const void*>(capture->r8);
    if (!context.logged_resource_sample && r8_ptr != nullptr && reinterpret_cast<std::uintptr_t>(r8_ptr) >= kMinUserModePointer)
    {
        TryLogEngine2CallerProbeObject("anim_caller.r8", r8_ptr, context, 0);
    }

    const auto* r9_ptr = reinterpret_cast<const void*>(capture->r9);
    if (!context.logged_resource_sample && r9_ptr != nullptr && reinterpret_cast<std::uintptr_t>(r9_ptr) >= kMinUserModePointer)
    {
        TryLogEngine2CallerProbeObject("anim_caller.r9", r9_ptr, context, 0);
    }

    if (!context.logged_resource_sample)
    {
        for (const auto offset : kEngine2CallerStackPointerOffsets)
        {
            if (context.logged_resource_sample || context.logged_sample_count >= kQueryAbiProbeMaxLogsPerAttempt)
            {
                break;
            }

            TryLogEngine2CallerStackPointerSlot(original_rsp, offset, context);
        }
    }

    if (context.logged_resource_sample)
    {
        g_state.animation_slot120_probe_window_resource_found = true;
        g_state.animation_slot120_probe_remove_requested.store(true, std::memory_order_relaxed);
        char found_log[192] {};
        sprintf_s(
            found_log,
            "[GlobalResourceSwapper] Animation Slot120 Caller Probe Result: resource-like carrier found on attempt=%u.",
            attempt);
        nskinzcs2::Logger::InfoAnsi(found_log);
    }
    else if (!context.logged_any_sample)
    {
        char none_log[224] {};
        sprintf_s(
            none_log,
            "[GlobalResourceSwapper] Animation Slot120 Caller Probe Result: attempt=%u found no readable ASCII carriers in rax/rcx/rdx/r8/r9/stack-pointer window.",
            attempt);
        nskinzcs2::Logger::InfoAnsi(none_log);
    }
    else
    {
        char no_resource_log[224] {};
        sprintf_s(
            no_resource_log,
            "[GlobalResourceSwapper] Animation Slot120 Caller Probe Result: attempt=%u found ASCII carriers, but none matched resource-like patterns.",
            attempt);
        nskinzcs2::Logger::InfoAnsi(no_resource_log);
    }

    if (attempt == kEngine2CallerProbeMaxAttempts && !context.logged_resource_sample)
    {
        if (!g_state.animation_slot120_probe_window_exhausted_logged)
        {
            g_state.animation_slot120_probe_window_exhausted_logged = true;
            char exhausted_log[224] {};
            sprintf_s(
                exhausted_log,
                "[GlobalResourceSwapper] Animation Slot120 Caller Probe Window exhausted after %u attempts without a resource-like carrier.",
                kEngine2CallerProbeMaxAttempts);
            nskinzcs2::Logger::InfoAnsi(exhausted_log);
        }
        g_state.animation_slot120_probe_remove_requested.store(true, std::memory_order_relaxed);
    }
}

void UpdateThreadLocalEngine2PrimaryCapture(const Engine2EntryCapture* capture) noexcept
{
    if (capture == nullptr)
    {
        g_thread_primary_capture.valid = false;
        g_thread_primary_capture.return_address = nullptr;
        return;
    }

    g_thread_primary_capture.capture = *capture;
    g_thread_primary_capture.return_address = nullptr;
    const auto original_rsp = static_cast<std::uintptr_t>(capture->original_rsp);
    if (original_rsp >= kMinUserModePointer)
    {
        SafeReadValue(
            reinterpret_cast<const void*>(original_rsp),
            g_thread_primary_capture.return_address);
    }

    ++g_thread_primary_capture.sequence;
    g_thread_primary_capture.valid = true;
}

void TryLogAnimationSlot120CorrelatedWrapperSnapshot(
    const CallerInfo& primary_info) noexcept
{
    static std::atomic<std::uint32_t> s_log_count {0};
    static std::atomic<bool> s_capture_failed_logged {false};
    if (s_log_count.load(std::memory_order_relaxed) >= 6)
    {
        return;
    }

    Engine2WrapperDispatchSnapshot snapshot {};
    if (!TryCaptureEngine2UpstreamWrapperDispatchSnapshot(
            &g_thread_primary_capture.capture,
            snapshot))
    {
        if (!s_capture_failed_logged.exchange(true, std::memory_order_relaxed))
        {
            nskinzcs2::Logger::Warn(
                L"GlobalResourceSwapper animation slot120 correlated wrapper snapshot failed: unable to resolve [rcx]->vtable[0x2B8] from the live primary capture.");
        }
        return;
    }

    const auto log_index = s_log_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (log_index > 6)
    {
        return;
    }

    const auto incoming_rax =
        reinterpret_cast<const void*>(g_thread_primary_capture.capture.rax);
    const auto incoming_rax_owner =
        GetModuleFileNameOnly(GetModuleFromAddress(incoming_rax));
    const auto emulated_rax_owner =
        GetModuleFileNameOnly(GetModuleFromAddress(snapshot.emulated_rax_after_mov));
    const auto slot_target_owner =
        GetModuleFileNameOnly(GetModuleFromAddress(snapshot.slot_target));
    const auto followed_target_owner =
        GetModuleFileNameOnly(GetModuleFromAddress(snapshot.followed_target));
    const auto base_target_owner =
        GetModuleFileNameOnly(GetModuleFromAddress(snapshot.base_target));
    const auto owner_decision = SelectEngine2UpstreamOwner(
        primary_info.return_owner,
        primary_info.caller_owner,
        emulated_rax_owner,
        slot_target_owner,
        followed_target_owner,
        base_target_owner);

    wchar_t slot_text[32] {};
    wchar_t accepted_text[8] {};
    wchar_t rejected_text[8] {};
    swprintf_s(slot_text, L"0x%X", kEngine2WrapperDispatchSlotOffset);
    swprintf_s(accepted_text, L"%u", owner_decision.accepted ? 1u : 0u);
    swprintf_s(rejected_text, L"%u", owner_decision.rejected ? 1u : 0u);

    nskinzcs2::Logger::Info(
        std::wstring(L"GlobalResourceSwapper animation slot120 correlated wrapper snapshot[") +
        std::to_wstring(log_index) +
        L"]: ret=" +
        FormatPointer(primary_info.return_address) +
        L", caller_base=" +
        FormatPointer(primary_info.caller_base) +
        L", caller_owner=" +
        primary_info.caller_owner +
        L", incoming_rax=" +
        FormatPointer(incoming_rax) +
        L", incoming_rax_owner=" +
        incoming_rax_owner +
        L", live_rcx=" +
        FormatPointer(snapshot.rcx_object) +
        L", live_rax_after_mov=[rcx]=" +
        FormatPointer(snapshot.emulated_rax_after_mov) +
        L", emulated_rax_owner=" +
        emulated_rax_owner +
        L", slot=" +
        slot_text +
        L", live_slot_entry=[rax+0x2B8]=" +
        FormatPointer(snapshot.slot_entry_address) +
        L", live_call_target=" +
        FormatPointer(snapshot.slot_target) +
        L", call_target_owner=" +
        slot_target_owner +
        L", followed=" +
        FormatPointer(snapshot.followed_target) +
        L", followed_owner=" +
        followed_target_owner +
        L", base=" +
        FormatPointer(snapshot.base_target) +
        L", base_owner=" +
        base_target_owner +
        L", owner_source=" +
        owner_decision.source +
        L", owner=" +
        owner_decision.owner +
        L", accepted=" +
        accepted_text +
        L", rejected=" +
        rejected_text +
        L".");

    if (snapshot.slot_target != nullptr && snapshot.direct_executable)
    {
        wchar_t label[160] {};
        swprintf_s(
            label,
            L"GlobalResourceSwapper animation slot120 correlated slot target[%u]",
            log_index);
        LogTargetBytes(label, snapshot.slot_target);
    }

    const void* preferred_target =
        snapshot.base_target != nullptr
            ? snapshot.base_target
            : snapshot.followed_target != nullptr
                ? snapshot.followed_target
                : snapshot.slot_target;
    if (preferred_target != nullptr &&
        preferred_target != snapshot.slot_target &&
        IsExecutableAddress(preferred_target))
    {
        wchar_t label[176] {};
        swprintf_s(
            label,
            L"GlobalResourceSwapper animation slot120 correlated preferred target[%u]",
            log_index);
        LogTargetBytes(label, preferred_target);
    }
}

void TryCorrelateAnimationSlot120WithPrimaryCapture(const CallerInfo& info) noexcept
{
    if (!g_thread_primary_capture.valid ||
        g_thread_primary_capture.return_address == nullptr ||
        g_thread_primary_capture.last_animation_slot120_sequence ==
            g_thread_primary_capture.sequence)
    {
        return;
    }

    void* primary_target = nullptr;
    {
        std::scoped_lock lock(g_state_mutex);
        primary_target = g_state.engine2_probe_target;
    }

    if (primary_target == nullptr || info.caller_base != primary_target)
    {
        return;
    }

    g_thread_primary_capture.last_animation_slot120_sequence =
        g_thread_primary_capture.sequence;

    CallerInfo primary_info {};
    if (!ResolveCallerInfo(g_thread_primary_capture.return_address, primary_info))
    {
        return;
    }

    TryLogAnimationSlot120CorrelatedWrapperSnapshot(primary_info);

    std::array<CallerInfo, kEngine2PrimaryAncestryFrameCapacity> ancestry_infos {};
    const auto ancestry_count = CollectEngine2PrimaryCallerAncestry(
        &g_thread_primary_capture.capture,
        primary_target,
        ancestry_infos);

    TryTrackEngine2PrimaryCallerSource(primary_info);
    for (std::size_t index = 0; index < ancestry_count; ++index)
    {
        const auto& ancestry_info = ancestry_infos[index];
        if (ancestry_info.return_address == nullptr ||
            ancestry_info.caller_base == nullptr ||
            (ancestry_info.return_address == primary_info.return_address &&
             ancestry_info.caller_base == primary_info.caller_base))
        {
            continue;
        }

        TryTrackEngine2PrimaryCallerSource(ancestry_info);
    }

    bool queued = false;
    for (std::size_t index = 0; index < ancestry_count; ++index)
    {
        const auto& ancestry_info = ancestry_infos[index];
        if (ancestry_info.stack_depth == 0 ||
            ancestry_info.caller_base == nullptr ||
            ancestry_info.return_address == nullptr ||
            IsNoisyEngine2UpstreamOwner(ancestry_info.return_owner) ||
            IsNoisyEngine2UpstreamOwner(ancestry_info.caller_owner))
        {
            continue;
        }

        if (TryQueueEngine2UpstreamCallerProbe(ancestry_info, nullptr, nullptr))
        {
            queued = true;
            break;
        }
    }

    if (!queued &&
        primary_info.caller_base != nullptr &&
        primary_info.return_address != nullptr &&
        !IsNoisyEngine2UpstreamOwner(primary_info.return_owner) &&
        !IsNoisyEngine2UpstreamOwner(primary_info.caller_owner))
    {
        const auto* live_object =
            reinterpret_cast<const void*>(g_thread_primary_capture.capture.rcx);
        void* live_vtable = nullptr;
        if (live_object != nullptr &&
            reinterpret_cast<std::uintptr_t>(live_object) >= kMinUserModePointer)
        {
            SafeReadValue(live_object, live_vtable);
        }

        queued = TryQueueEngine2UpstreamCallerProbe(primary_info, live_object, live_vtable);
    }

    if (queued)
    {
        TryLogEngine2UpstreamProbeQueuedOnce();
    }

    static std::atomic<std::uint32_t> s_log_count {0};
    const auto log_index = s_log_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (log_index <= 6)
    {
        char line[320] {};
        sprintf_s(
            line,
            "[GlobalResourceSwapper] Animation Slot120 correlated with Engine2 primary capture[%u]: primary_ret=%p primary_caller=%p depth=%u ancestry=%zu queued=%u",
            log_index,
            primary_info.return_address,
            primary_info.caller_base,
            primary_info.stack_depth,
            ancestry_count,
            queued ? 1u : 0u);
        nskinzcs2::Logger::InfoAnsi(line);

        LogCallerInfoSnapshotAnsi(
            "[GlobalResourceSwapper] Animation Slot120 Correlated Primary",
            primary_info);
        for (std::size_t index = 0; index < ancestry_count; ++index)
        {
            const auto& ancestry_info = ancestry_infos[index];
            if (ancestry_info.return_address == nullptr || ancestry_info.caller_base == nullptr)
            {
                continue;
            }

            char prefix[112] {};
            sprintf_s(
                prefix,
                "[GlobalResourceSwapper] Animation Slot120 Correlated Ancestry[%zu]",
                index + 1);
            LogCallerInfoSnapshotAnsi(prefix, ancestry_info);
        }
    }
}

void __fastcall AnimationSlot120CallerProbeCallback(const Engine2EntryCapture* capture) noexcept
{
    g_state.animation_slot120_probe_entered_count.fetch_add(1, std::memory_order_relaxed);
    static thread_local bool s_inside_callback = false;
    if (capture == nullptr || s_inside_callback)
    {
        return;
    }

    s_inside_callback = true;
    TryLogAnimationSlot120CallerProbeWindow(capture);
    s_inside_callback = false;
}

void TryTrackEngine2PrimaryCallerSource(const CallerInfo& info) noexcept
{
    struct TrackResult
    {
        bool new_return_bucket = false;
        bool new_candidate = false;
        std::size_t return_bucket_index = kInvalidSlot;
        std::size_t candidate_index = kInvalidSlot;
        std::uint32_t return_hits = 0;
        std::uint32_t candidate_hits = 0;
        bool candidate_attempted = false;
        bool candidate_success = false;
    };

    static std::atomic<bool> s_return_capacity_logged {false};
    static std::atomic<bool> s_candidate_capacity_logged {false};

    if (info.return_address == nullptr || info.caller_base == nullptr)
    {
        return;
    }

    TrackResult result {};
    {
        std::scoped_lock lock(g_state_mutex);

        std::size_t return_bucket_index = kInvalidSlot;
        for (std::size_t index = 0; index < g_state.engine2_primary_return_bucket_count; ++index)
        {
            if (g_state.engine2_primary_return_buckets[index].return_address == info.return_address)
            {
                return_bucket_index = index;
                break;
            }
        }

        if (return_bucket_index == kInvalidSlot)
        {
            if (g_state.engine2_primary_return_bucket_count < g_state.engine2_primary_return_buckets.size())
            {
                return_bucket_index = g_state.engine2_primary_return_bucket_count++;
                auto& bucket = g_state.engine2_primary_return_buckets[return_bucket_index];
                bucket.return_address = info.return_address;
                bucket.caller_base = info.caller_base;
                bucket.caller_offset = info.caller_offset;
                bucket.stack_depth = info.stack_depth;
                bucket.hits = 0;
                result.new_return_bucket = true;
            }
            else
            {
                s_return_capacity_logged.store(true, std::memory_order_relaxed);
            }
        }

        if (return_bucket_index != kInvalidSlot)
        {
            auto& bucket = g_state.engine2_primary_return_buckets[return_bucket_index];
            ++bucket.hits;
            result.return_bucket_index = return_bucket_index;
            result.return_hits = bucket.hits;
        }

        std::size_t candidate_index = kInvalidSlot;
        for (std::size_t index = 0; index < g_state.engine2_primary_caller_candidate_count; ++index)
        {
            if (g_state.engine2_primary_caller_candidates[index].caller_base == info.caller_base)
            {
                candidate_index = index;
                break;
            }
        }

        if (candidate_index == kInvalidSlot)
        {
            if (g_state.engine2_primary_caller_candidate_count < g_state.engine2_primary_caller_candidates.size())
            {
                candidate_index = g_state.engine2_primary_caller_candidate_count++;
                auto& candidate = g_state.engine2_primary_caller_candidates[candidate_index];
                candidate.caller_base = info.caller_base;
                candidate.sample_return_address = info.return_address;
                candidate.caller_offset = info.caller_offset;
                candidate.sample_depth = info.stack_depth;
                candidate.hits = 0;
                candidate.queued = false;
                candidate.attempted = false;
                candidate.success = false;
                result.new_candidate = true;
            }
            else
            {
                s_candidate_capacity_logged.store(true, std::memory_order_relaxed);
            }
        }

        if (candidate_index != kInvalidSlot)
        {
            auto& candidate = g_state.engine2_primary_caller_candidates[candidate_index];
            ++candidate.hits;
            result.candidate_index = candidate_index;
            result.candidate_hits = candidate.hits;
            result.candidate_attempted = candidate.attempted;
            result.candidate_success = candidate.success;
        }
    }

    if (s_return_capacity_logged.exchange(false, std::memory_order_relaxed))
    {
        nskinzcs2::Logger::Warn(
            L"GlobalResourceSwapper engine2 primary return bucket capacity is full; newer return-address callsites will no longer be tracked.");
    }
    if (s_candidate_capacity_logged.exchange(false, std::memory_order_relaxed))
    {
        nskinzcs2::Logger::Warn(
            L"GlobalResourceSwapper engine2 primary caller candidate capacity is full; newer distinct caller wrappers will no longer be tracked.");
    }

    char ret_owner_ansi[64] {};
    char caller_owner_ansi[64] {};
    WideToAnsiLossy(info.return_owner, ret_owner_ansi, std::size(ret_owner_ansi));
    WideToAnsiLossy(info.caller_owner, caller_owner_ansi, std::size(caller_owner_ansi));

    if (result.new_return_bucket)
    {
        char line[512] {};
        sprintf_s(
            line,
            "[GlobalResourceSwapper] Engine2 Primary Return Bucket[%zu]: depth=%u ret=%p ret_owner=%s caller_base=%p caller_owner=%s caller_offset=0x%llX ret_hits=%u caller_hits=%u attempted=%u success=%u",
            result.return_bucket_index + 1,
            info.stack_depth,
            info.return_address,
            ret_owner_ansi,
            info.caller_base,
            caller_owner_ansi,
            static_cast<unsigned long long>(info.caller_offset),
            result.return_hits,
            result.candidate_hits,
            result.candidate_attempted ? 1u : 0u,
            result.candidate_success ? 1u : 0u);
        nskinzcs2::Logger::InfoAnsi(line);
    }

    if (result.new_candidate)
    {
        char line[416] {};
        sprintf_s(
            line,
            "[GlobalResourceSwapper] Engine2 Primary Caller Candidate[%zu]: depth=%u caller_base=%p caller_owner=%s sample_ret=%p ret_owner=%s caller_offset=0x%llX",
            result.candidate_index + 1,
            info.stack_depth,
            info.caller_base,
            caller_owner_ansi,
            info.return_address,
            ret_owner_ansi,
            static_cast<unsigned long long>(info.caller_offset));
        nskinzcs2::Logger::InfoAnsi(line);
    }
}

bool TryReadVirtualCallSlot(const void* address, std::uint32_t& slot) noexcept
{
    if (address == nullptr)
    {
        return false;
    }

    std::uint8_t bytes[9] {};
    for (std::size_t index = 0; index < std::size(bytes); ++index)
    {
        if (!SafeReadValue(static_cast<const std::uint8_t*>(address) + index, bytes[index]))
        {
            return false;
        }
    }

    if (bytes[0] != 0x48 || bytes[1] != 0x8B || bytes[2] != 0x01 || bytes[3] != 0xFF || bytes[4] != 0x90)
    {
        return false;
    }

    std::uint32_t slot_value = 0;
    std::memcpy(&slot_value, bytes + 5, sizeof(slot_value));
    slot = slot_value;
    return true;
}

bool TryReadVirtualMethodTargetRaw(
    const void* object,
    const std::uint32_t slot_offset,
    void*& out_vtable,
    void*& out_entry_address,
    void*& out_target) noexcept
{
    out_vtable = nullptr;
    out_entry_address = nullptr;
    out_target = nullptr;

    if (object == nullptr || reinterpret_cast<std::uintptr_t>(object) < 0x10000ull)
    {
        return false;
    }

    void* vtable = nullptr;
    if (!SafeReadValue(object, vtable) || vtable == nullptr || !IsReadableAddress(vtable))
    {
        return false;
    }

    auto* entry_address = reinterpret_cast<void*>(
        reinterpret_cast<std::uintptr_t>(vtable) + static_cast<std::uintptr_t>(slot_offset));
    void* target = nullptr;
    if (!SafeReadValue(entry_address, target) || target == nullptr)
    {
        return false;
    }

    out_vtable = vtable;
    out_entry_address = entry_address;
    out_target = target;
    return true;
}

bool ResolveVirtualMethodTargetFromObject(
    const void* object,
    const std::uint32_t slot_offset,
    void*& out_vtable,
    void*& out_entry_address,
    void*& out_target,
    void*& out_followed,
    void*& out_base) noexcept
{
    out_vtable = nullptr;
    out_entry_address = nullptr;
    out_target = nullptr;
    out_followed = nullptr;
    out_base = nullptr;

    void* target = nullptr;
    if (!TryReadVirtualMethodTargetRaw(
            object,
            slot_offset,
            out_vtable,
            out_entry_address,
            target))
    {
        return false;
    }

    void* followed = FollowCodePointer(target);
    if (followed == nullptr)
    {
        followed = target;
    }

    void* base = ResolveFunctionBase(target);
    if (base == nullptr)
    {
        base = followed;
    }

    const bool executable =
        IsExecutableAddress(target) ||
        (followed != nullptr && IsExecutableAddress(followed)) ||
        (base != nullptr && IsExecutableAddress(base));
    if (!executable)
    {
        out_target = target;
        out_followed = followed;
        out_base = base;
        return false;
    }

    out_target = target;
    out_followed = followed;
    out_base = base;
    return true;
}

struct Engine2IncomingCallerSummary
{
    const void* callsite = nullptr;
    void* caller_base = nullptr;
    std::uint64_t call_offset = 0;
    std::uint32_t hit_count = 0;
};

struct Engine2VcallObjectCandidate
{
    const wchar_t* label = nullptr;
    std::size_t offset = 0;
    bool dereference_from_live_object = false;
    bool inline_subobject_from_live_object = false;
};

struct Engine2CallerRcxCarrier
{
    const void* loader_address = nullptr;
    const void* source_address = nullptr;
    const void* object = nullptr;
    bool from_memory = false;
};

struct Engine2CallerCallWindowEntry
{
    const void* callsite = nullptr;
    std::uint64_t call_offset = 0;
    std::uint32_t slot = 0;
    std::uint8_t instruction_length = 0;
    const char* kind = nullptr;
    bool focus = false;
    bool same_rcx_source = false;
    Engine2CallerRcxCarrier rcx_carrier {};
    void* raw_target = nullptr;
    void* followed_target = nullptr;
    void* base_target = nullptr;
};

bool TryResolveRipRelativeAddress(
    const void* instruction,
    const std::size_t displacement_offset,
    const std::size_t instruction_length,
    const void*& out) noexcept
{
    out = nullptr;
    if (instruction == nullptr)
    {
        return false;
    }

    std::int32_t displacement = 0;
    if (!SafeReadValue(
            static_cast<const std::uint8_t*>(instruction) + displacement_offset,
            displacement))
    {
        return false;
    }

    out = static_cast<const std::uint8_t*>(instruction) +
        instruction_length +
        static_cast<std::intptr_t>(displacement);
    return true;
}

bool TryResolveEngine2CallsiteRcxCarrier(
    const void* caller_base,
    const void* callsite,
    Engine2CallerRcxCarrier& out) noexcept
{
    out = {};
    if (caller_base == nullptr || callsite == nullptr)
    {
        return false;
    }

    const auto* function_base = static_cast<const std::uint8_t*>(caller_base);
    const auto* site = static_cast<const std::uint8_t*>(callsite);
    if (site <= function_base)
    {
        return false;
    }

    std::size_t max_backscan = static_cast<std::size_t>(site - function_base);
    if (max_backscan > kEngine2CallerRcxBackscanBytes)
    {
        max_backscan = kEngine2CallerRcxBackscanBytes;
    }

    for (std::size_t back = 7; back <= max_backscan; ++back)
    {
        const auto* cursor = site - back;
        std::uint8_t b0 = 0;
        std::uint8_t b1 = 0;
        std::uint8_t b2 = 0;
        if (!SafeReadValue(cursor + 0, b0) ||
            !SafeReadValue(cursor + 1, b1) ||
            !SafeReadValue(cursor + 2, b2))
        {
            continue;
        }

        if (b0 != 0x48 || b2 != 0x0D || (b1 != 0x8B && b1 != 0x8D))
        {
            continue;
        }

        const void* rip_target = nullptr;
        if (!TryResolveRipRelativeAddress(cursor, 3, 7, rip_target))
        {
            continue;
        }

        out.loader_address = cursor;
        out.source_address = rip_target;
        out.from_memory = (b1 == 0x8B);
        if (out.from_memory)
        {
            const void* object = nullptr;
            if (SafeReadValue(rip_target, object))
            {
                out.object = object;
            }
        }
        else
        {
            out.object = rip_target;
        }
        return true;
    }

    return false;
}

bool TryDecodeEngine2CallerCall(
    const void* callsite,
    Engine2CallerCallWindowEntry& out) noexcept
{
    out = {};
    if (callsite == nullptr || !IsExecutableAddress(callsite))
    {
        return false;
    }

    const auto* site = static_cast<const std::uint8_t*>(callsite);
    std::uint8_t b0 = 0;
    std::uint8_t b1 = 0;
    if (!SafeReadValue(site + 0, b0))
    {
        return false;
    }

    out.callsite = callsite;
    if (b0 == 0xE8)
    {
        const void* target = nullptr;
        if (!TryResolveRipRelativeAddress(callsite, 1, 5, target))
        {
            return false;
        }

        out.kind = "direct";
        out.instruction_length = 5;
        out.raw_target = const_cast<void*>(target);
        return true;
    }

    if (!SafeReadValue(site + 1, b1))
    {
        return false;
    }

    if (b0 == 0xFF && b1 == 0x50)
    {
        std::uint8_t slot8 = 0;
        if (!SafeReadValue(site + 2, slot8))
        {
            return false;
        }

        out.kind = "vcall8";
        out.instruction_length = 3;
        out.slot = slot8;
        return true;
    }

    if (b0 == 0xFF && b1 == 0x90)
    {
        std::uint32_t slot32 = 0;
        if (!SafeReadValue(site + 2, slot32))
        {
            return false;
        }

        out.kind = "vcall32";
        out.instruction_length = 6;
        out.slot = slot32;
        return true;
    }

    if (b0 == 0xFF && b1 == 0x15)
    {
        const void* slot_address = nullptr;
        if (!TryResolveRipRelativeAddress(callsite, 2, 6, slot_address))
        {
            return false;
        }

        void* target = nullptr;
        if (!SafeReadValue(slot_address, target))
        {
            target = nullptr;
        }

        out.kind = "rip_indirect";
        out.instruction_length = 6;
        out.raw_target = target;
        return true;
    }

    return false;
}

void ResolveEngine2CallerCallTarget(Engine2CallerCallWindowEntry& entry) noexcept
{
    if (entry.kind == nullptr)
    {
        return;
    }

    if (std::strcmp(entry.kind, "vcall8") == 0 || std::strcmp(entry.kind, "vcall32") == 0)
    {
        if (entry.rcx_carrier.object == nullptr ||
            reinterpret_cast<std::uintptr_t>(entry.rcx_carrier.object) < kMinUserModePointer ||
            entry.slot > 0x1000u)
        {
            return;
        }

        void* vtable = nullptr;
        void* entry_address = nullptr;
        void* target = nullptr;
        if (!TryReadVirtualMethodTargetRaw(
                entry.rcx_carrier.object,
                entry.slot,
                vtable,
                entry_address,
                target))
        {
            return;
        }

        entry.raw_target = target;
    }

    if (entry.raw_target == nullptr)
    {
        return;
    }

    entry.followed_target = FollowCodePointer(entry.raw_target);
    if (entry.followed_target == nullptr)
    {
        entry.followed_target = entry.raw_target;
    }

    entry.base_target = ResolveFunctionBase(entry.raw_target);
    if (entry.base_target == nullptr)
    {
        entry.base_target = entry.followed_target;
    }
}

void* SelectExecutableHookTarget(const Engine2CallerCallWindowEntry& entry) noexcept
{
    if (entry.base_target != nullptr && IsExecutableAddress(entry.base_target))
    {
        return entry.base_target;
    }
    if (entry.followed_target != nullptr && IsExecutableAddress(entry.followed_target))
    {
        return entry.followed_target;
    }
    if (entry.raw_target != nullptr && IsExecutableAddress(entry.raw_target))
    {
        return entry.raw_target;
    }
    return nullptr;
}

bool TryResolveSiblingDescriptorForEntry(
    const Engine2CallerCallWindowEntry& entry,
    std::size_t& out_index) noexcept
{
    out_index = static_cast<std::size_t>(-1);
    const auto raw_owner = GetModuleFileNameOnly(GetModuleFromAddress(entry.raw_target));
    const auto followed_owner = GetModuleFileNameOnly(GetModuleFromAddress(entry.followed_target));
    const auto base_owner = GetModuleFileNameOnly(GetModuleFromAddress(entry.base_target));

    for (std::size_t index = 0; index < kSiblingHookDescriptors.size(); ++index)
    {
        const auto& descriptor = kSiblingHookDescriptors[index];
        if (entry.slot != descriptor.slot_offset)
        {
            continue;
        }

        if (ModuleNameEquals(raw_owner, descriptor.owner_module) ||
            ModuleNameEquals(followed_owner, descriptor.owner_module) ||
            ModuleNameEquals(base_owner, descriptor.owner_module))
        {
            out_index = index;
            return true;
        }
    }

    return false;
}

void TryQueueSiblingHookFromEntry(const Engine2CallerCallWindowEntry& entry) noexcept
{
    std::size_t descriptor_index = static_cast<std::size_t>(-1);
    if (!TryResolveSiblingDescriptorForEntry(entry, descriptor_index))
    {
        return;
    }

    void* target = SelectExecutableHookTarget(entry);
    if (target == nullptr)
    {
        return;
    }

    const auto& descriptor = kSiblingHookDescriptors[descriptor_index];
    bool queued = false;
    {
        std::scoped_lock lock(g_state_mutex);
        auto& runtime = g_state.sibling_hooks[descriptor_index];
        if (!runtime.active && runtime.target == nullptr && runtime.pending_target == nullptr)
        {
            runtime.pending_target = target;
            queued = true;
        }
    }

    if (queued)
    {
        nskinzcs2::Logger::Info(
            std::wstring(L"GlobalResourceSwapper queued sibling monitor: tag=") +
            descriptor.owner_module +
            L"/0x" +
            [] (std::uint32_t slot) {
                wchar_t text[16] {};
                swprintf_s(text, L"%X", slot);
                return std::wstring(text);
            }(descriptor.slot_offset) +
            L", target=" +
            FormatPointer(target) +
            L".");
    }
}

void TryLogEngine2CallerCallWindowOnce(const CallerInfo& info) noexcept
{
    static std::atomic<bool> s_logged {false};
    if (info.caller_base == nullptr ||
        !ModuleNameEquals(info.caller_owner, L"engine2.dll"))
    {
        return;
    }

    if (s_logged.exchange(true, std::memory_order_relaxed))
    {
        return;
    }

    HMODULE module = GetModuleFromAddress(info.caller_base);
    const auto text = GetTextSectionSpan(module);
    if (module == nullptr || !text.has_value())
    {
        return;
    }

    const auto* text_begin = text->base;
    const auto* text_end = text->base + text->size;
    const auto* caller_base = static_cast<const std::uint8_t*>(info.caller_base);
    const auto* focus_callsite = caller_base + info.caller_offset;
    if (focus_callsite < text_begin || focus_callsite >= text_end)
    {
        return;
    }

    Engine2CallerCallWindowEntry focus {};
    const bool have_focus = TryDecodeEngine2CallerCall(focus_callsite, focus);
    if (have_focus)
    {
        focus.focus = true;
        focus.call_offset = info.caller_offset;
        TryResolveEngine2CallsiteRcxCarrier(info.caller_base, focus_callsite, focus.rcx_carrier);
        ResolveEngine2CallerCallTarget(focus);
    }

    char focus_bytes[3 * kEngine2CallerWindowContextBytes + 8] {};
    char focus_target_owner_ansi[64] {};
    char focus_followed_owner_ansi[64] {};
    char focus_base_owner_ansi[64] {};
    char header[768] {};
    const auto focus_context_start_offset =
        info.caller_offset > 16 ? info.caller_offset - 16 : 0ull;
    FormatMachineBytesAnsi(
        caller_base + focus_context_start_offset,
        kEngine2CallerWindowContextBytes,
        focus_bytes,
        std::size(focus_bytes));
    WideToAnsiLossy(
        GetModuleFileNameOnly(GetModuleFromAddress(focus.raw_target)),
        focus_target_owner_ansi,
        std::size(focus_target_owner_ansi));
    WideToAnsiLossy(
        GetModuleFileNameOnly(GetModuleFromAddress(focus.followed_target)),
        focus_followed_owner_ansi,
        std::size(focus_followed_owner_ansi));
    WideToAnsiLossy(
        GetModuleFileNameOnly(GetModuleFromAddress(focus.base_target)),
        focus_base_owner_ansi,
        std::size(focus_base_owner_ansi));

    sprintf_s(
        header,
        "[GlobalResourceSwapper] Engine2 Caller CallWindow Focus: caller_base=%p focus_callsite=%p focus_offset=0x%llX kind=%s slot=0x%X rcx_loader=%p rcx_source=%p rcx_object=%p target=%p target_owner=%s followed=%p followed_owner=%s base=%p base_owner=%s bytes=%s",
        info.caller_base,
        focus_callsite,
        static_cast<unsigned long long>(info.caller_offset),
        have_focus && focus.kind != nullptr ? focus.kind : "undecoded",
        have_focus ? focus.slot : 0u,
        focus.rcx_carrier.loader_address,
        focus.rcx_carrier.source_address,
        focus.rcx_carrier.object,
        focus.raw_target,
        focus_target_owner_ansi,
        focus.followed_target,
        focus_followed_owner_ansi,
        focus.base_target,
        focus_base_owner_ansi,
        focus_bytes);
    nskinzcs2::Logger::InfoAnsi(header);

    const auto start_offset =
        info.caller_offset > kEngine2CallerWindowBackRange
            ? info.caller_offset - kEngine2CallerWindowBackRange
            : 0ull;
    const auto* scan_begin = caller_base + start_offset;
    const auto* scan_end = focus_callsite + kEngine2CallerWindowForwardRange;
    if (scan_end > text_end)
    {
        scan_end = text_end;
    }

    std::size_t logged = 0;
    for (const auto* cursor = scan_begin;
         cursor < scan_end && logged < kEngine2CallerWindowMaxEntries;
         ++cursor)
    {
        Engine2CallerCallWindowEntry entry {};
        if (!TryDecodeEngine2CallerCall(cursor, entry))
        {
            continue;
        }

        entry.call_offset = static_cast<std::uint64_t>(cursor - caller_base);
        entry.focus = (cursor == focus_callsite);
        TryResolveEngine2CallsiteRcxCarrier(info.caller_base, cursor, entry.rcx_carrier);
        entry.same_rcx_source =
            focus.rcx_carrier.source_address != nullptr &&
            focus.rcx_carrier.source_address == entry.rcx_carrier.source_address &&
            focus.rcx_carrier.from_memory == entry.rcx_carrier.from_memory;
        ResolveEngine2CallerCallTarget(entry);

        char target_owner_ansi[64] {};
        char followed_owner_ansi[64] {};
        char base_owner_ansi[64] {};
        char context_bytes[3 * kEngine2CallerWindowContextBytes + 8] {};
        char line[1024] {};
        const auto context_start_offset =
            entry.call_offset > 16 ? entry.call_offset - 16 : 0ull;
        WideToAnsiLossy(
            GetModuleFileNameOnly(GetModuleFromAddress(entry.raw_target)),
            target_owner_ansi,
            std::size(target_owner_ansi));
        WideToAnsiLossy(
            GetModuleFileNameOnly(GetModuleFromAddress(entry.followed_target)),
            followed_owner_ansi,
            std::size(followed_owner_ansi));
        WideToAnsiLossy(
            GetModuleFileNameOnly(GetModuleFromAddress(entry.base_target)),
            base_owner_ansi,
            std::size(base_owner_ansi));
        FormatMachineBytesAnsi(
            caller_base + context_start_offset,
            kEngine2CallerWindowContextBytes,
            context_bytes,
            std::size(context_bytes));

        sprintf_s(
            line,
            "[GlobalResourceSwapper] Engine2 Caller CallWindow[%zu]: focus=%u callsite=%p offset=0x%llX kind=%s slot=0x%X same_rcx=%u rcx_loader=%p rcx_source=%p rcx_object=%p target=%p target_owner=%s followed=%p followed_owner=%s base=%p base_owner=%s bytes=%s",
            logged + 1,
            entry.focus ? 1u : 0u,
            entry.callsite,
            static_cast<unsigned long long>(entry.call_offset),
            entry.kind != nullptr ? entry.kind : "unknown",
            entry.slot,
            entry.same_rcx_source ? 1u : 0u,
            entry.rcx_carrier.loader_address,
            entry.rcx_carrier.source_address,
            entry.rcx_carrier.object,
            entry.raw_target,
            target_owner_ansi,
            entry.followed_target,
            followed_owner_ansi,
            entry.base_target,
            base_owner_ansi,
            context_bytes);
        nskinzcs2::Logger::InfoAnsi(line);
        TryQueueSiblingHookFromEntry(entry);

        ++logged;
        if (entry.instruction_length > 0)
        {
            cursor += entry.instruction_length - 1;
        }
    }

    if (logged == 0)
    {
        nskinzcs2::Logger::Warn(
            L"GlobalResourceSwapper engine2 caller call-window analysis found no decodable sibling/overload callsites near the 0x2B8C focus.");
    }
}

void* GetSiblingDetourForIndex(const std::size_t index) noexcept
{
    switch (index)
    {
    case 0:
        return reinterpret_cast<void*>(&hkSiblingProbe248);
    case 1:
        return reinterpret_cast<void*>(&hkSiblingProbe0A0);
    case 2:
        return reinterpret_cast<void*>(&hkSiblingProbe120);
    default:
        return nullptr;
    }
}

void TryActivatePendingSiblingHooks() noexcept
{
    std::scoped_lock lock(g_state_mutex);

    for (std::size_t index = 0; index < kSiblingHookDescriptors.size(); ++index)
    {
        auto& runtime = g_state.sibling_hooks[index];
        if (runtime.active || runtime.pending_target == nullptr)
        {
            continue;
        }

        void* detour = GetSiblingDetourForIndex(index);
        if (detour == nullptr)
        {
            runtime.pending_target = nullptr;
            continue;
        }

        void* original_raw = nullptr;
        const auto create_status = MH_CreateHook(runtime.pending_target, detour, &original_raw);

        wchar_t create_label[96] {};
        swprintf_s(
            create_label,
            L"MH_CreateHook[%S]",
            kSiblingHookDescriptors[index].log_tag);
        LogMinHookStep(create_label, create_status, runtime.pending_target);
        if (create_status != MH_OK || original_raw == nullptr)
        {
            runtime.pending_target = nullptr;
            continue;
        }

        const auto enable_status = MH_EnableHook(runtime.pending_target);
        wchar_t enable_label[96] {};
        swprintf_s(
            enable_label,
            L"MH_EnableHook[%S]",
            kSiblingHookDescriptors[index].log_tag);
        LogMinHookStep(enable_label, enable_status, runtime.pending_target);
        if (enable_status != MH_OK && enable_status != MH_ERROR_ENABLED)
        {
            MH_RemoveHook(runtime.pending_target);
            runtime.pending_target = nullptr;
            continue;
        }

        runtime.target = runtime.pending_target;
        runtime.original = reinterpret_cast<GenericSlotFn>(original_raw);
        runtime.pending_target = nullptr;
        runtime.active = true;

        nskinzcs2::Logger::Info(
            std::wstring(L"GlobalResourceSwapper sibling monitor armed: tag=") +
            std::wstring(kSiblingHookDescriptors[index].owner_module) +
            L"/0x" +
            [] (std::uint32_t slot) {
                wchar_t text[16] {};
                swprintf_s(text, L"%X", slot);
                return std::wstring(text);
            }(kSiblingHookDescriptors[index].slot_offset) +
            L", target=" +
            FormatPointer(runtime.target) +
            L".");
    }
}

void TryCaptureSiblingProbeSample(
    const std::size_t descriptor_index,
    void* this_ptr,
    const std::uint64_t arg2,
    const std::uint64_t arg3,
    const std::uint64_t arg4) noexcept
{
    if (descriptor_index >= kSiblingHookDescriptors.size())
    {
        return;
    }

    g_state.sibling_active_mask.fetch_or(
        kSiblingHookDescriptors[descriptor_index].active_bit,
        std::memory_order_relaxed);

    const SiblingProbeSample sample {
        this_ptr,
        reinterpret_cast<const void*>(arg2),
        reinterpret_cast<const void*>(arg3),
        reinterpret_cast<const void*>(arg4),
    };

    bool should_log = false;
    std::uint32_t sample_index = 0;
    {
        std::scoped_lock lock(g_state_mutex);
        auto& runtime = g_state.sibling_hooks[descriptor_index];
        for (std::uint32_t index = 0; index < runtime.sample_count; ++index)
        {
            const auto& existing = runtime.samples[index];
            if (existing.this_ptr == sample.this_ptr &&
                existing.arg2 == sample.arg2 &&
                existing.arg3 == sample.arg3 &&
                existing.arg4 == sample.arg4)
            {
                return;
            }
        }

        if (runtime.sample_count >= runtime.samples.size())
        {
            return;
        }

        sample_index = runtime.sample_count;
        runtime.samples[runtime.sample_count++] = sample;
        should_log = true;
    }

    if (should_log)
    {
        char line[320] {};
        sprintf_s(
            line,
            "[%s] sample=%u this=%p arg2=%p arg3=%p arg4=%p",
            kSiblingHookDescriptors[descriptor_index].log_tag,
            sample_index + 1,
            sample.this_ptr,
            sample.arg2,
            sample.arg3,
            sample.arg4);
        nskinzcs2::Logger::InfoAnsi(line);
    }
}

void TryLogEngine2UpstreamOwnerWalk(
    const void* live_object,
    const std::uint32_t virtual_slot) noexcept
{
    static std::atomic<bool> s_logged {false};
    if (s_logged.exchange(true, std::memory_order_relaxed))
    {
        return;
    }

    if (!IsReadableAddress(live_object))
    {
        nskinzcs2::Logger::Warn(
            L"GlobalResourceSwapper engine2 upstream owner walk skipped: live rcx is not readable.");
        return;
    }

    wchar_t slot_text[32] {};
    swprintf_s(slot_text, L"0x%X", virtual_slot);
    nskinzcs2::Logger::Info(
        std::wstring(L"GlobalResourceSwapper engine2 upstream owner walk begin: live_rcx=") +
        FormatPointer(live_object) +
        L", slot=" +
        slot_text +
        L".");

    LogTargetBytes(
        L"GlobalResourceSwapper engine2 upstream owner walk live rcx bytes",
        live_object);

    static constexpr std::array<std::size_t, 17> kBackOffsets {{
        0x0, 0x8, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x40,
        0x48, 0x50, 0x58, 0x60, 0x68, 0x70, 0x78, 0x80,
    }};

    const auto live_value = reinterpret_cast<std::uintptr_t>(live_object);
    std::uint32_t logged = 0;

    for (const auto back_offset : kBackOffsets)
    {
        if (live_value < kMinUserModePointer + back_offset)
        {
            continue;
        }

        const auto* candidate_base = reinterpret_cast<const void*>(live_value - back_offset);
        if (!IsReadableAddress(candidate_base))
        {
            continue;
        }

        void* candidate_vtable = nullptr;
        if (!SafeReadValue(candidate_base, candidate_vtable) ||
            !IsReadableAddress(candidate_vtable))
        {
            continue;
        }

        void* slot_entry = nullptr;
        void* slot_target = nullptr;
        void* slot_followed = nullptr;
        void* slot_base = nullptr;
        void* resolved_vtable = nullptr;
        const bool slot_ready = ResolveVirtualMethodTargetFromObject(
            candidate_base,
            virtual_slot,
            resolved_vtable,
            slot_entry,
            slot_target,
            slot_followed,
            slot_base);

        void* raw_slot_vtable = nullptr;
        void* raw_slot_entry = nullptr;
        void* raw_slot_target = nullptr;
        if (!slot_ready &&
            !TryReadVirtualMethodTargetRaw(
                candidate_base,
                virtual_slot,
                raw_slot_vtable,
                raw_slot_entry,
                raw_slot_target))
        {
            raw_slot_vtable = candidate_vtable;
            raw_slot_entry = nullptr;
            raw_slot_target = nullptr;
        }

        void* slot0_vtable = nullptr;
        void* slot0_entry = nullptr;
        void* slot0_target = nullptr;
        void* slot0_followed = nullptr;
        void* slot0_base = nullptr;
        const bool slot0_ready = ResolveVirtualMethodTargetFromObject(
            candidate_base,
            0,
            slot0_vtable,
            slot0_entry,
            slot0_target,
            slot0_followed,
            slot0_base);

        const auto vtable_owner =
            GetModuleFileNameOnly(GetModuleFromAddress(candidate_vtable));
        const auto slot_owner =
            GetModuleFileNameOnly(GetModuleFromAddress(slot_ready ? slot_target : raw_slot_target));
        const auto slot_followed_owner =
            GetModuleFileNameOnly(GetModuleFromAddress(slot_followed));
        const auto slot_base_owner =
            GetModuleFileNameOnly(GetModuleFromAddress(slot_base));
        const auto slot0_owner =
            GetModuleFileNameOnly(GetModuleFromAddress(slot0_target));
        const auto slot0_followed_owner =
            GetModuleFileNameOnly(GetModuleFromAddress(slot0_followed));
        const auto slot0_base_owner =
            GetModuleFileNameOnly(GetModuleFromAddress(slot0_base));

        const bool slot_exec =
            slot_ready ||
            IsExecutableAddress(raw_slot_target) ||
            IsExecutableAddress(slot_followed) ||
            IsExecutableAddress(slot_base);
        const bool slot0_exec =
            slot0_ready ||
            IsExecutableAddress(slot0_target) ||
            IsExecutableAddress(slot0_followed) ||
            IsExecutableAddress(slot0_base);
        const bool interesting =
            !vtable_owner.empty() ||
            slot_exec ||
            slot0_exec ||
            (slot_ready ? slot_target : raw_slot_target) != nullptr ||
            slot0_target != nullptr;
        if (!interesting)
        {
            continue;
        }

        char vtable_owner_ansi[64] {};
        char slot_owner_ansi[64] {};
        char slot_followed_owner_ansi[64] {};
        char slot_base_owner_ansi[64] {};
        char slot0_owner_ansi[64] {};
        char slot0_followed_owner_ansi[64] {};
        char slot0_base_owner_ansi[64] {};
        char candidate_bytes[3 * kMachineByteCount + 8] {};
        char line[1024] {};

        WideToAnsiLossy(vtable_owner, vtable_owner_ansi, std::size(vtable_owner_ansi));
        WideToAnsiLossy(slot_owner, slot_owner_ansi, std::size(slot_owner_ansi));
        WideToAnsiLossy(slot_followed_owner, slot_followed_owner_ansi, std::size(slot_followed_owner_ansi));
        WideToAnsiLossy(slot_base_owner, slot_base_owner_ansi, std::size(slot_base_owner_ansi));
        WideToAnsiLossy(slot0_owner, slot0_owner_ansi, std::size(slot0_owner_ansi));
        WideToAnsiLossy(slot0_followed_owner, slot0_followed_owner_ansi, std::size(slot0_followed_owner_ansi));
        WideToAnsiLossy(slot0_base_owner, slot0_base_owner_ansi, std::size(slot0_base_owner_ansi));
        FormatMachineBytesAnsi(candidate_base, 24, candidate_bytes, std::size(candidate_bytes));

        sprintf_s(
            line,
            "[GlobalResourceSwapper] Engine2 Owner Walk[%u]: live=%p base=%p back=0x%zX vtable=%p vtable_owner=%s slot_entry=%p slot_target=%p slot_owner=%s slot_followed=%p slot_followed_owner=%s slot_base=%p slot_base_owner=%s slot_exec=%u slot0=%p slot0_owner=%s slot0_followed=%p slot0_followed_owner=%s slot0_base=%p slot0_base_owner=%s slot0_exec=%u bytes=%s",
            logged + 1,
            live_object,
            candidate_base,
            back_offset,
            candidate_vtable,
            vtable_owner_ansi,
            slot_ready ? slot_entry : raw_slot_entry,
            slot_ready ? slot_target : raw_slot_target,
            slot_owner_ansi,
            slot_followed,
            slot_followed_owner_ansi,
            slot_base,
            slot_base_owner_ansi,
            slot_exec ? 1u : 0u,
            slot0_target,
            slot0_owner_ansi,
            slot0_followed,
            slot0_followed_owner_ansi,
            slot0_base,
            slot0_base_owner_ansi,
            slot0_exec ? 1u : 0u,
            candidate_bytes);
        nskinzcs2::Logger::InfoAnsi(line);

        if (slot_exec || slot0_exec)
        {
            wchar_t label[160] {};
            swprintf_s(
                label,
                L"GlobalResourceSwapper engine2 owner walk candidate back=0x%zX",
                back_offset);
            LogTargetBytes(label, candidate_base);
        }

        ++logged;
        if (logged >= 12)
        {
            break;
        }
    }

    if (logged == 0)
    {
        nskinzcs2::Logger::Warn(
            L"GlobalResourceSwapper engine2 upstream owner walk found no module-backed parent candidates near rcx.");
    }
}

bool TryResolveEngine2UpstreamRegisterCarriers(
    const std::array<const void*, 8>& register_candidates,
    const std::uint32_t virtual_slot) noexcept
{
    wchar_t slot_text[32] {};
    swprintf_s(slot_text, L"0x%X", virtual_slot);

    for (std::size_t index = 0; index < register_candidates.size(); ++index)
    {
        const auto* candidate_object = register_candidates[index];
        if (!IsReadableAddress(candidate_object))
        {
            continue;
        }

        void* resolved_vtable = nullptr;
        void* resolved_entry = nullptr;
        void* resolved_target = nullptr;
        void* resolved_followed = nullptr;
        void* resolved_base = nullptr;
        const bool resolved = ResolveVirtualMethodTargetFromObject(
            candidate_object,
            virtual_slot,
            resolved_vtable,
            resolved_entry,
            resolved_target,
            resolved_followed,
            resolved_base);

        void* raw_vtable = nullptr;
        void* raw_entry = nullptr;
        void* raw_target = nullptr;
        if (!resolved &&
            !TryReadVirtualMethodTargetRaw(
                candidate_object,
                virtual_slot,
                raw_vtable,
                raw_entry,
                raw_target))
        {
            raw_vtable = nullptr;
            raw_entry = nullptr;
            raw_target = nullptr;
        }

        void* slot0_vtable = nullptr;
        void* slot0_entry = nullptr;
        void* slot0_target = nullptr;
        void* slot0_followed = nullptr;
        void* slot0_base = nullptr;
        const bool slot0_resolved = ResolveVirtualMethodTargetFromObject(
            candidate_object,
            0,
            slot0_vtable,
            slot0_entry,
            slot0_target,
            slot0_followed,
            slot0_base);

        const auto vtable_owner = GetModuleFileNameOnly(GetModuleFromAddress(
            resolved ? resolved_vtable : raw_vtable));
        const auto target_owner = GetModuleFileNameOnly(GetModuleFromAddress(
            resolved ? resolved_target : raw_target));
        const auto base_owner = GetModuleFileNameOnly(GetModuleFromAddress(
            resolved ? resolved_base : nullptr));
        const auto slot0_owner = GetModuleFileNameOnly(GetModuleFromAddress(slot0_target));

        char vtable_owner_ansi[64] {};
        char target_owner_ansi[64] {};
        char base_owner_ansi[64] {};
        char slot0_owner_ansi[64] {};
        char bytes[3 * kMachineByteCount + 8] {};
        char line[1024] {};

        WideToAnsiLossy(vtable_owner, vtable_owner_ansi, std::size(vtable_owner_ansi));
        WideToAnsiLossy(target_owner, target_owner_ansi, std::size(target_owner_ansi));
        WideToAnsiLossy(base_owner, base_owner_ansi, std::size(base_owner_ansi));
        WideToAnsiLossy(slot0_owner, slot0_owner_ansi, std::size(slot0_owner_ansi));
        FormatMachineBytesAnsi(candidate_object, 24, bytes, std::size(bytes));

        char label_ansi[16] {};
        WideToAnsiLossy(kEngine2NonvolatileRegisterLabels[index], label_ansi, std::size(label_ansi));
        sprintf_s(
            line,
            "[GlobalResourceSwapper] Engine2 Register Carrier[%s]: object=%p vtable=%p entry=%p target=%p target_owner=%s base=%p base_owner=%s slot_exec=%u slot0=%p slot0_owner=%s slot0_exec=%u bytes=%s",
            label_ansi,
            candidate_object,
            resolved ? resolved_vtable : raw_vtable,
            resolved ? resolved_entry : raw_entry,
            resolved ? resolved_target : raw_target,
            target_owner_ansi,
            resolved ? resolved_base : nullptr,
            base_owner_ansi,
            resolved ? 1u : 0u,
            slot0_target,
            slot0_owner_ansi,
            slot0_resolved ? 1u : 0u,
            bytes);
        nskinzcs2::Logger::InfoAnsi(line);

        if (!resolved)
        {
            continue;
        }

        nskinzcs2::Logger::Info(
            std::wstring(L"GlobalResourceSwapper engine2 upstream owner resolved from nonvolatile register: carrier=") +
            kEngine2NonvolatileRegisterLabels[index] +
            L", object=" +
            FormatPointer(candidate_object) +
            L", slot=" +
            slot_text +
            L", target=" +
            FormatPointer(resolved_target) +
            L", base=" +
            FormatPointer(resolved_base) +
            L".");

        {
            std::scoped_lock lock(g_state_mutex);
            g_state.engine2_upstream_live_object = candidate_object;
            g_state.engine2_upstream_live_vtable = resolved_vtable;
            g_state.engine2_upstream_live_entry_address = resolved_entry;
            g_state.engine2_upstream_live_target = resolved_target;
            g_state.engine2_upstream_live_target_base = resolved_base;
            g_state.engine2_upstream_analysis_state = Engine2UpstreamAnalysisState::VcallTargetReady;
        }
        return true;
    }

    return false;
}

std::size_t FindDirectIncomingCallers(
    const ModuleSpan& span,
    const void* target,
    Engine2IncomingCallerSummary* summaries,
    const std::size_t max_summaries,
    std::size_t& total_calls) noexcept
{
    total_calls = 0;
    if (span.base == nullptr || span.size < 5 || target == nullptr || summaries == nullptr || max_summaries == 0)
    {
        return 0;
    }

    const auto target_value = reinterpret_cast<std::uintptr_t>(target);
    std::size_t summary_count = 0;
    for (std::size_t index = 0; index + 5 <= span.size; ++index)
    {
        std::uint8_t opcode = 0;
        if (!SafeReadValue(span.base + index, opcode) || opcode != 0xE8)
        {
            continue;
        }

        std::int32_t rel = 0;
        if (!SafeReadValue(span.base + index + 1, rel))
        {
            continue;
        }

        const auto callsite = reinterpret_cast<const void*>(span.base + index);
        const auto resolved = reinterpret_cast<std::uintptr_t>(span.base + index + 5) + static_cast<std::intptr_t>(rel);
        if (resolved != target_value)
        {
            continue;
        }

        ++total_calls;
        void* caller_base = ResolveFunctionBase(const_cast<void*>(callsite));
        if (caller_base == nullptr)
        {
            caller_base = const_cast<void*>(callsite);
        }

        const auto callsite_value = reinterpret_cast<std::uintptr_t>(callsite);
        const auto caller_value = reinterpret_cast<std::uintptr_t>(caller_base);
        const auto call_offset = caller_value != 0 && callsite_value >= caller_value
            ? static_cast<std::uint64_t>(callsite_value - caller_value)
            : 0ull;

        bool merged = false;
        for (std::size_t summary_index = 0; summary_index < summary_count; ++summary_index)
        {
            if (summaries[summary_index].caller_base == caller_base)
            {
                ++summaries[summary_index].hit_count;
                merged = true;
                break;
            }
        }

        if (merged || summary_count >= max_summaries)
        {
            continue;
        }

        summaries[summary_count].callsite = callsite;
        summaries[summary_count].caller_base = caller_base;
        summaries[summary_count].call_offset = call_offset;
        summaries[summary_count].hit_count = 1;
        ++summary_count;
    }

    return summary_count;
}

void TryAnalyzeQueuedEngine2UpstreamCallerOnce() noexcept
{
    void* pending_target = nullptr;
    const void* live_object = nullptr;
    std::array<const void*, 8> live_registers {};
    bool should_analyze = false;
    std::uint32_t attempt = 0;

    {
        std::scoped_lock lock(g_state_mutex);
        if (g_state.engine2_upstream_precall_snapshot_captured.load(std::memory_order_relaxed))
        {
            g_state.engine2_upstream_probe_pending_return = nullptr;
            g_state.engine2_upstream_probe_pending_target = nullptr;
            g_state.engine2_upstream_probe_pending_offset = 0;
            g_state.engine2_upstream_probe_pending = false;
            return;
        }

        should_analyze =
            g_state.installed &&
            g_state.engine2_upstream_probe_pending &&
            !g_state.engine2_upstream_probe_active &&
            !g_state.engine2_upstream_probe_analysis_done &&
            g_state.engine2_upstream_probe_pending_target != nullptr;
        if (should_analyze)
        {
            pending_target = g_state.engine2_upstream_probe_pending_target;
            live_object = g_state.engine2_upstream_live_object;
            live_registers = g_state.engine2_upstream_live_registers;
            g_state.engine2_upstream_probe_analysis_done = true;
            g_state.engine2_upstream_analysis_state = Engine2UpstreamAnalysisState::Begin;
            g_state.engine2_upstream_analysis_total_direct_calls = 0;
            g_state.engine2_upstream_analysis_summary_count = 0;
            g_state.engine2_upstream_analysis_exception_code = 0;
            attempt = ++g_state.engine2_upstream_analysis_attempts;
        }
    }

    if (!should_analyze || pending_target == nullptr)
    {
        return;
    }

    wchar_t attempt_text[32] {};
    swprintf_s(attempt_text, L"%u", attempt);
    nskinzcs2::Logger::Info(
        std::wstring(L"GlobalResourceSwapper engine2 upstream read-only analysis begin: target=") +
        FormatPointer(pending_target) +
        L", attempt=" +
        attempt_text +
        L".");

    HMODULE module = GetModuleFromAddress(pending_target);
    const auto module_name = GetModuleFileNameOnly(module);
    if (module == nullptr || !ModuleNameEquals(module_name, L"engine2.dll"))
    {
        {
            std::scoped_lock lock(g_state_mutex);
            g_state.engine2_upstream_analysis_state = Engine2UpstreamAnalysisState::ModuleMismatch;
        }

        nskinzcs2::Logger::Warn(
            L"GlobalResourceSwapper engine2 upstream read-only analysis skipped: queued target is not owned by engine2.dll.");
        return;
    }

    LogTargetBytes(L"GlobalResourceSwapper engine2 upstream queued target", pending_target);

    std::uint32_t virtual_slot = 0;
    const bool have_virtual_slot = TryReadVirtualCallSlot(pending_target, virtual_slot);
    if (have_virtual_slot)
    {
        wchar_t slot_text[32] {};
        swprintf_s(slot_text, L"0x%X", virtual_slot);
        nskinzcs2::Logger::Info(
            std::wstring(L"GlobalResourceSwapper engine2 upstream queued target identity: wrapper starts with a virtual dispatch through vtable slot ") +
            slot_text +
            L".");
    }
    else
    {
        nskinzcs2::Logger::Info(
            L"GlobalResourceSwapper engine2 upstream queued target identity: no simple RCX vcall wrapper signature detected in the first bytes.");
    }

    const auto text = GetTextSectionSpan(module);
    if (!text.has_value())
    {
        {
            std::scoped_lock lock(g_state_mutex);
            g_state.engine2_upstream_analysis_state = Engine2UpstreamAnalysisState::TextMissing;
        }

        nskinzcs2::Logger::Warn(
            L"GlobalResourceSwapper engine2 upstream read-only analysis failed: unable to resolve engine2.dll .text span.");
        return;
    }

    std::array<Engine2IncomingCallerSummary, 8> summaries {};
    std::size_t total_calls = 0;
    const auto summary_count = FindDirectIncomingCallers(*text, pending_target, summaries.data(), summaries.size(), total_calls);

    {
        std::scoped_lock lock(g_state_mutex);
        g_state.engine2_upstream_analysis_total_direct_calls = total_calls;
        g_state.engine2_upstream_analysis_summary_count =
            static_cast<std::uint32_t>(summary_count);
    }

    wchar_t summary_text[64] {};
    swprintf_s(summary_text, L"%llu", static_cast<unsigned long long>(total_calls));
    nskinzcs2::Logger::Info(
        std::wstring(L"GlobalResourceSwapper engine2 upstream read-only xref scan: target=") +
        FormatPointer(pending_target) +
        L", total_direct_calls=" +
        summary_text +
        L".");

    if (summary_count == 0)
    {
        nskinzcs2::Logger::Info(
            L"GlobalResourceSwapper engine2 upstream read-only xref scan found no direct E8 callers to the queued target.");
    }
    else
    {
        for (std::size_t index = 0; index < summary_count; ++index)
        {
            char owner_ansi[64] {};
            char bytes[3 * kMachineByteCount + 8] {};
            char line[768] {};
            WideToAnsiLossy(
                GetModuleFileNameOnly(GetModuleFromAddress(summaries[index].caller_base)),
                owner_ansi,
                std::size(owner_ansi));
            FormatMachineBytesAnsi(summaries[index].caller_base, kMachineByteCount, bytes, std::size(bytes));
            sprintf_s(
                line,
                "[GlobalResourceSwapper] Engine2 Upstream Xref[%zu/%zu]: callsite=%p caller_base=%p caller_owner=%s call_offset=0x%llX hits=%u bytes=%s",
                index + 1,
                summary_count,
                summaries[index].callsite,
                summaries[index].caller_base,
                owner_ansi,
                static_cast<unsigned long long>(summaries[index].call_offset),
                summaries[index].hit_count,
                bytes);
            nskinzcs2::Logger::InfoAnsi(line);
        }
    }

    if (have_virtual_slot)
    {
        static constexpr Engine2VcallObjectCandidate kCandidates[] = {
            {L"rcx", 0x0, false, false},
            {L"rcx[*+0x8]", 0x8, true, false},
            {L"rcx[*+0x10]", 0x10, true, false},
            {L"rcx[*+0x18]", 0x18, true, false},
            {L"rcx{+0x8}", 0x8, false, true},
            {L"rcx{+0x10}", 0x10, false, true},
            {L"rcx{+0x18}", 0x18, false, true},
            {L"rcx{+0x20}", 0x20, false, true},
            {L"rcx{+0x28}", 0x28, false, true},
            {L"rcx{+0x30}", 0x30, false, true},
        };

        const auto slot_index = virtual_slot / sizeof(void*);
        bool any_invalid_target = false;
        wchar_t slot_text[32] {};
        wchar_t slot_index_text[32] {};
        swprintf_s(slot_text, L"0x%X", virtual_slot);
        swprintf_s(slot_index_text, L"0x%llX", static_cast<unsigned long long>(slot_index));

        for (const auto& candidate : kCandidates)
        {
            const void* candidate_object = live_object;
            if (candidate.dereference_from_live_object)
            {
                if (live_object == nullptr)
                {
                    continue;
                }

                if (!SafeReadValue(
                        reinterpret_cast<const std::uint8_t*>(live_object) + candidate.offset,
                        candidate_object) ||
                    candidate_object == nullptr ||
                    reinterpret_cast<std::uintptr_t>(candidate_object) < 0x10000ull)
                {
                    continue;
                }
            }
            else if (candidate.inline_subobject_from_live_object)
            {
                if (live_object == nullptr)
                {
                    continue;
                }

                candidate_object =
                    reinterpret_cast<const std::uint8_t*>(live_object) + candidate.offset;
                if (candidate_object == nullptr ||
                    reinterpret_cast<std::uintptr_t>(candidate_object) < 0x10000ull ||
                    !IsReadableAddress(candidate_object))
                {
                    continue;
                }
            }

            void* raw_vtable = nullptr;
            void* raw_entry_address = nullptr;
            void* raw_target = nullptr;
            if (!TryReadVirtualMethodTargetRaw(
                    candidate_object,
                    virtual_slot,
                    raw_vtable,
                    raw_entry_address,
                    raw_target))
            {
                continue;
            }

            void* resolved_vtable = nullptr;
            void* resolved_entry_address = nullptr;
            void* resolved_target = nullptr;
            void* resolved_followed = nullptr;
            void* resolved_base = nullptr;
            if (ResolveVirtualMethodTargetFromObject(
                    candidate_object,
                    virtual_slot,
                    resolved_vtable,
                    resolved_entry_address,
                    resolved_target,
                    resolved_followed,
                    resolved_base))
            {
                const auto target_owner =
                    GetModuleFileNameOnly(GetModuleFromAddress(resolved_target));
                const auto followed_owner =
                    GetModuleFileNameOnly(GetModuleFromAddress(resolved_followed));
                const auto base_owner =
                    GetModuleFileNameOnly(GetModuleFromAddress(resolved_base));

                nskinzcs2::Logger::Info(
                    std::wstring(L"GlobalResourceSwapper engine2 upstream live vcall target resolved: carrier=") +
                    candidate.label +
                    L", object=" +
                    FormatPointer(candidate_object) +
                    L", vtable=" +
                    FormatPointer(resolved_vtable) +
                    L", slot=" +
                    slot_text +
                    L", index=" +
                    slot_index_text +
                    L", entry=" +
                    FormatPointer(resolved_entry_address) +
                    L", target=" +
                    FormatPointer(resolved_target) +
                    L", target_owner=" +
                    target_owner +
                    L", followed=" +
                    FormatPointer(resolved_followed) +
                    L", followed_owner=" +
                    followed_owner +
                    L", base=" +
                    FormatPointer(resolved_base) +
                    L", base_owner=" +
                    base_owner +
                    L".");

                LogTargetBytes(
                    L"GlobalResourceSwapper engine2 upstream live vcall direct target",
                    resolved_target);
                if (resolved_followed != nullptr && resolved_followed != resolved_target)
                {
                    LogTargetBytes(
                        L"GlobalResourceSwapper engine2 upstream live vcall followed target",
                        resolved_followed);
                }
                if (resolved_base != nullptr &&
                    resolved_base != resolved_followed &&
                    resolved_base != resolved_target)
                {
                    LogTargetBytes(
                        L"GlobalResourceSwapper engine2 upstream live vcall function base",
                        resolved_base);
                }

                {
                    std::scoped_lock lock(g_state_mutex);
                    g_state.engine2_upstream_live_object = candidate_object;
                    g_state.engine2_upstream_live_vtable = resolved_vtable;
                    g_state.engine2_upstream_live_entry_address = resolved_entry_address;
                    g_state.engine2_upstream_live_target = resolved_target;
                    g_state.engine2_upstream_live_target_base = resolved_base;
                    g_state.engine2_upstream_analysis_state = Engine2UpstreamAnalysisState::VcallTargetReady;
                }
                return;
            }

            any_invalid_target = true;
            const auto raw_target_owner =
                GetModuleFileNameOnly(GetModuleFromAddress(raw_target));
            nskinzcs2::Logger::Warn(
                std::wstring(L"GlobalResourceSwapper engine2 upstream live vcall candidate was non-executable: carrier=") +
                candidate.label +
                L", object=" +
                FormatPointer(candidate_object) +
                L", vtable=" +
                FormatPointer(raw_vtable) +
                L", slot=" +
                slot_text +
                L", entry=" +
                FormatPointer(raw_entry_address) +
                L", target=" +
                FormatPointer(raw_target) +
                L", target_owner=" +
                raw_target_owner +
                L".");
        }

        {
            std::scoped_lock lock(g_state_mutex);
            g_state.engine2_upstream_analysis_state =
                live_object == nullptr
                    ? Engine2UpstreamAnalysisState::VcallObjectMissing
                    : any_invalid_target
                        ? Engine2UpstreamAnalysisState::VcallTargetInvalid
                        : Engine2UpstreamAnalysisState::VcallTargetMissing;
        }

        if (TryResolveEngine2UpstreamRegisterCarriers(live_registers, virtual_slot))
        {
            return;
        }

        if (live_object != nullptr)
        {
            TryLogEngine2UpstreamOwnerWalk(live_object, virtual_slot);
        }

        nskinzcs2::Logger::Warn(
            std::wstring(L"GlobalResourceSwapper engine2 upstream live vcall target resolution failed after candidate walk: object=") +
            FormatPointer(live_object) +
            L", slot=" +
            slot_text +
            L".");
        return;
    }

    {
        std::scoped_lock lock(g_state_mutex);
        g_state.engine2_upstream_analysis_state =
            summary_count == 0
                ? Engine2UpstreamAnalysisState::NoDirectCallers
                : Engine2UpstreamAnalysisState::XrefReady;
    }
}

bool TryQueueEngine2UpstreamCallerProbe(
    const CallerInfo& info,
    const void* live_object,
    void* live_vtable) noexcept
{
    if (info.caller_base == nullptr || !ModuleNameEquals(info.caller_owner, L"engine2.dll"))
    {
        return false;
    }

    std::unique_lock lock(g_state_mutex, std::try_to_lock);
    if (!lock.owns_lock())
    {
        return false;
    }

    if (g_state.engine2_upstream_precall_snapshot_captured.load(std::memory_order_relaxed) ||
        g_state.engine2_upstream_probe_active ||
        g_state.engine2_upstream_probe_pending ||
        info.caller_base == g_state.engine2_probe_target)
    {
        return false;
    }

    std::size_t candidate_index = kInvalidSlot;
    for (std::size_t index = 0; index < g_state.engine2_primary_caller_candidate_count; ++index)
    {
        if (g_state.engine2_primary_caller_candidates[index].caller_base == info.caller_base)
        {
            candidate_index = index;
            break;
        }
    }

    if (candidate_index == kInvalidSlot)
    {
        return false;
    }

    auto& candidate = g_state.engine2_primary_caller_candidates[candidate_index];
    if (candidate.queued || candidate.attempted || candidate.success)
    {
        return false;
    }

    if (!IsExecutableAddress(info.caller_base))
    {
        nskinzcs2::Logger::Warn(
            std::wstring(L"GlobalResourceSwapper engine2 upstream caller queue skipped: live caller is not executable, target=") +
            FormatPointer(info.caller_base));
        return false;
    }

    Engine2WrapperDispatchSnapshot live_snapshot {};
    const bool have_live_snapshot =
        live_object != nullptr &&
        TryCaptureEngine2UpstreamWrapperDispatchSnapshot(live_object, live_snapshot);
    if (live_vtable == nullptr && have_live_snapshot)
    {
        live_vtable = live_snapshot.emulated_rax_after_mov;
    }

    void* live_target = nullptr;
    void* live_target_base = nullptr;
    if (have_live_snapshot)
    {
        live_target = live_snapshot.slot_target;
        live_target_base =
            live_snapshot.base_target != nullptr
                ? live_snapshot.base_target
                : live_snapshot.followed_target != nullptr
                    ? live_snapshot.followed_target
                    : live_snapshot.slot_target;
    }

    const auto previous_state = g_state.engine2_upstream_analysis_state;
    static std::atomic<bool> s_same_rejected_logged {false};
    static std::atomic<bool> s_rejected_mismatch_logged {false};
    const bool same_rejected_object =
        g_state.engine2_upstream_last_rejected_caller == info.caller_base &&
        g_state.engine2_upstream_last_rejected_object != nullptr &&
        g_state.engine2_upstream_last_rejected_object == live_object &&
        g_state.engine2_upstream_last_rejected_vtable != nullptr &&
        g_state.engine2_upstream_last_rejected_vtable == live_vtable;
    const bool same_rejected_target =
        g_state.engine2_upstream_last_rejected_caller == info.caller_base &&
        g_state.engine2_upstream_last_rejected_target != nullptr &&
        g_state.engine2_upstream_last_rejected_target == live_target;
    const bool same_rejected_base =
        g_state.engine2_upstream_last_rejected_caller == info.caller_base &&
        g_state.engine2_upstream_last_rejected_target_base != nullptr &&
        g_state.engine2_upstream_last_rejected_target_base == live_target_base;
    const bool same_rejected_instance =
        same_rejected_object ||
        same_rejected_target ||
        same_rejected_base;
    if (same_rejected_instance)
    {
        g_state.engine2_upstream_analysis_state = Engine2UpstreamAnalysisState::AwaitingModelOwner;
        if (!s_same_rejected_logged.exchange(true, std::memory_order_relaxed))
        {
            nskinzcs2::Logger::Info(
                std::wstring(L"GlobalResourceSwapper engine2 upstream caller queue suppressed for repeated rejected fingerprint: caller=") +
                FormatPointer(info.caller_base) +
                L", object=" +
                FormatPointer(live_object) +
                L", vtable=" +
                FormatPointer(live_vtable) +
                L", target=" +
                FormatPointer(live_target) +
                L", base=" +
                FormatPointer(live_target_base) +
                L", match_object=" +
                (same_rejected_object ? L"1" : L"0") +
                L", match_target=" +
                (same_rejected_target ? L"1" : L"0") +
                L", match_base=" +
                (same_rejected_base ? L"1" : L"0") +
                L".");
        }
        return false;
    }

    if ((previous_state == Engine2UpstreamAnalysisState::SkippedNonModelOwner ||
         previous_state == Engine2UpstreamAnalysisState::AwaitingModelOwner) &&
        g_state.engine2_upstream_last_rejected_caller == info.caller_base &&
        !s_rejected_mismatch_logged.exchange(true, std::memory_order_relaxed))
    {
        nskinzcs2::Logger::Info(
            std::wstring(L"GlobalResourceSwapper engine2 upstream rejected fingerprint mismatch: caller=") +
            FormatPointer(info.caller_base) +
            L", rejected_object=" +
            FormatPointer(g_state.engine2_upstream_last_rejected_object) +
            L", rejected_vtable=" +
            FormatPointer(g_state.engine2_upstream_last_rejected_vtable) +
            L", rejected_target=" +
            FormatPointer(g_state.engine2_upstream_last_rejected_target) +
            L", rejected_base=" +
            FormatPointer(g_state.engine2_upstream_last_rejected_target_base) +
            L", incoming_object=" +
            FormatPointer(live_object) +
            L", incoming_vtable=" +
            FormatPointer(live_vtable) +
            L", incoming_target=" +
            FormatPointer(live_target) +
            L", incoming_base=" +
            FormatPointer(live_target_base) +
            L", live_snapshot=" +
            (have_live_snapshot ? L"1" : L"0") +
            L".");
    }

    const bool preserve_analysis =
        previous_state == Engine2UpstreamAnalysisState::SkippedNonModelOwner ||
        previous_state == Engine2UpstreamAnalysisState::AwaitingModelOwner;

    g_state.engine2_upstream_probe_pending_return = info.return_address;
    g_state.engine2_upstream_probe_pending_target = info.caller_base;
    g_state.engine2_upstream_probe_pending_offset = info.caller_offset;
    g_state.engine2_upstream_probe_pending = true;
    g_state.engine2_upstream_source_candidate_index = candidate_index;
    candidate.queued = true;
    g_state.engine2_upstream_probe_analysis_done = preserve_analysis;
    g_state.engine2_upstream_analysis_state = Engine2UpstreamAnalysisState::Queued;
    if (!preserve_analysis)
    {
        g_state.engine2_upstream_analysis_attempts = 0;
        g_state.engine2_upstream_analysis_total_direct_calls = 0;
        g_state.engine2_upstream_analysis_summary_count = 0;
        g_state.engine2_upstream_analysis_exception_code = 0;
    }
    g_state.engine2_upstream_live_object = nullptr;
    g_state.engine2_upstream_live_vtable = nullptr;
    g_state.engine2_upstream_live_entry_address = nullptr;
    g_state.engine2_upstream_live_target = nullptr;
    g_state.engine2_upstream_live_target_base = nullptr;
    g_state.engine2_upstream_live_registers.fill(nullptr);

    wchar_t offset_text[32] {};
    wchar_t retry_text[8] {};
    wchar_t candidate_text[16] {};
    wchar_t depth_text[16] {};
    swprintf_s(offset_text, L"0x%llX", static_cast<unsigned long long>(info.caller_offset));
    swprintf_s(retry_text, L"%u", preserve_analysis ? 1u : 0u);
    swprintf_s(candidate_text, L"%zu", candidate_index + 1);
    swprintf_s(depth_text, L"%u", candidate.sample_depth);
    nskinzcs2::Logger::Info(
        std::wstring(L"GlobalResourceSwapper engine2 upstream caller probe queued: ret=") +
        FormatPointer(info.return_address) +
        L", target=" +
        FormatPointer(info.caller_base) +
        L", depth=" +
        depth_text +
        L", offset=" +
        offset_text +
        L", candidate=" +
        candidate_text +
        L", caller_hits=" +
        std::to_wstring(candidate.hits) +
        L", retry_after_skip=" +
        retry_text +
        L".");
    return true;
}

void TryPumpEngine2UpstreamFromPrimaryProbe(
    const Engine2EntryCapture* capture,
    const void* return_address) noexcept
{
    if (capture == nullptr || return_address == nullptr)
    {
        return;
    }

    CallerInfo upstream_info {};
    if (!ResolveCallerInfo(return_address, upstream_info))
    {
        return;
    }

    void* primary_target = nullptr;
    {
        std::scoped_lock lock(g_state_mutex);
        primary_target = g_state.engine2_probe_target;
    }

    std::array<CallerInfo, kEngine2PrimaryAncestryFrameCapacity> ancestry_infos {};
    const auto ancestry_count =
        primary_target != nullptr
            ? CollectEngine2PrimaryCallerAncestry(capture, primary_target, ancestry_infos)
            : 0;

    for (std::size_t index = 0; index < ancestry_count; ++index)
    {
        if (ancestry_infos[index].return_address == upstream_info.return_address &&
            ancestry_infos[index].caller_base == upstream_info.caller_base)
        {
            upstream_info.stack_depth = ancestry_infos[index].stack_depth;
            break;
        }
    }

    TryTrackEngine2PrimaryCallerSource(upstream_info);
    for (std::size_t index = 0; index < ancestry_count; ++index)
    {
        const auto& ancestry_info = ancestry_infos[index];
        if (ancestry_info.return_address == nullptr ||
            ancestry_info.caller_base == nullptr ||
            (ancestry_info.return_address == upstream_info.return_address &&
             ancestry_info.caller_base == upstream_info.caller_base))
        {
            continue;
        }

        TryTrackEngine2PrimaryCallerSource(ancestry_info);
    }

    TryLogEngine2PrimaryDispatchSnapshotOnce(capture, &upstream_info);
    TryLogEngine2PrimaryCallerChainOnce(upstream_info);
    TryLogEngine2UpstreamCallerChainOnce(upstream_info);
    TryLogEngine2UpstreamWrapperDispatchSnapshotOnce(capture, &upstream_info);
    const auto* live_object = reinterpret_cast<const void*>(capture->rcx);
    void* live_vtable = nullptr;
    if (live_object != nullptr && reinterpret_cast<std::uintptr_t>(live_object) >= kMinUserModePointer)
    {
        SafeReadValue(live_object, live_vtable);
    }

    bool queued = false;
    for (std::size_t index = 0; index < ancestry_count; ++index)
    {
        const auto& ancestry_info = ancestry_infos[index];
        if (ancestry_info.stack_depth == 0 ||
            ancestry_info.caller_base == nullptr ||
            ancestry_info.return_address == nullptr)
        {
            continue;
        }

        if (IsNoisyEngine2UpstreamOwner(ancestry_info.return_owner) ||
            IsNoisyEngine2UpstreamOwner(ancestry_info.caller_owner))
        {
            continue;
        }

        if (TryQueueEngine2UpstreamCallerProbe(ancestry_info, nullptr, nullptr))
        {
            queued = true;
            break;
        }
    }

    if (!queued && TryQueueEngine2UpstreamCallerProbe(upstream_info, live_object, live_vtable))
    {
        queued = true;
    }

    if (queued)
    {
        TryLogEngine2UpstreamProbeQueuedOnce();
    }
    TryCaptureEngine2UpstreamLiveObject(upstream_info, capture);
}

bool TryQueueNextEngine2UpstreamCallerCandidate() noexcept
{
    std::array<CallerInfo, kEngine2PrimaryCallerCandidateCapacity> ordered_infos {};
    std::array<std::size_t, kEngine2PrimaryCallerCandidateCapacity> ordered_indices {};
    std::size_t ordered_count = 0;
    bool have_engine2_deep_candidate = false;

    {
        std::scoped_lock lock(g_state_mutex);
        if (!g_state.installed ||
            g_state.engine2_upstream_precall_snapshot_captured.load(std::memory_order_relaxed) ||
            g_state.engine2_upstream_probe_active ||
            g_state.engine2_upstream_probe_pending)
        {
            return false;
        }

        for (std::size_t index = 0; index < g_state.engine2_primary_caller_candidate_count; ++index)
        {
            const auto& candidate = g_state.engine2_primary_caller_candidates[index];
            CallerInfo info {};
            if (candidate.queued || candidate.attempted || candidate.success ||
                !TryBuildCallerInfoFromCandidate(candidate, info) ||
                IsNoisyEngine2UpstreamOwner(info.return_owner) ||
                IsNoisyEngine2UpstreamOwner(info.caller_owner))
            {
                continue;
            }

            if (ModuleNameEquals(info.caller_owner, L"engine2.dll") && info.stack_depth > 0)
            {
                have_engine2_deep_candidate = true;
            }

            ordered_infos[ordered_count] = std::move(info);
            ordered_indices[ordered_count] = index;
            ++ordered_count;
        }
    }

    while (ordered_count > 0)
    {
        std::size_t best_pos = kInvalidSlot;
        int best_owner_rank = 99;
        std::uint32_t best_depth = 0xFFFFFFFFu;
        std::uint32_t best_hits = 0;

        for (std::size_t pos = 0; pos < ordered_count; ++pos)
        {
            const auto& info = ordered_infos[pos];
            if (have_engine2_deep_candidate &&
                (!ModuleNameEquals(info.caller_owner, L"engine2.dll") || info.stack_depth == 0))
            {
                continue;
            }

            int owner_rank = 2;
            if (ModuleNameEquals(info.caller_owner, L"engine2.dll"))
            {
                owner_rank = 0;
            }
            else if (ModuleNameEquals(info.caller_owner, L"cs2.exe"))
            {
                owner_rank = 1;
            }

            const auto hits = [&]() -> std::uint32_t
            {
                std::scoped_lock lock(g_state_mutex);
                return ordered_indices[pos] < g_state.engine2_primary_caller_candidate_count
                    ? g_state.engine2_primary_caller_candidates[ordered_indices[pos]].hits
                    : 0u;
            }();

            if (best_pos == kInvalidSlot ||
                owner_rank < best_owner_rank ||
                (owner_rank == best_owner_rank &&
                 (info.stack_depth < best_depth ||
                  (info.stack_depth == best_depth && hits > best_hits))))
            {
                best_pos = pos;
                best_owner_rank = owner_rank;
                best_depth = info.stack_depth;
                best_hits = hits;
            }
        }

        if (best_pos == kInvalidSlot)
        {
            return false;
        }

        if (TryQueueEngine2UpstreamCallerProbe(ordered_infos[best_pos], nullptr, nullptr))
        {
            return true;
        }

        ordered_infos[best_pos] = ordered_infos[ordered_count - 1];
        ordered_indices[best_pos] = ordered_indices[ordered_count - 1];
        --ordered_count;
    }

    return false;
}

void TryMarkColdEngine2UpstreamCallerProbeForRemoval() noexcept
{
    std::size_t candidate_index = kInvalidSlot;
    void* target = nullptr;
    std::uint64_t armed_slot68_count = 0;
    std::uint64_t current_slot68_count = 0;
    std::uint32_t candidate_depth = 0;

    {
        std::scoped_lock lock(g_state_mutex);
        if (!g_state.engine2_upstream_probe_active ||
            g_state.engine2_upstream_probe_target == nullptr ||
            g_state.engine2_upstream_probe_entered_count.load(std::memory_order_relaxed) != 0 ||
            g_state.engine2_upstream_precall_capture_state.load(std::memory_order_relaxed) != 0)
        {
            return;
        }

        armed_slot68_count = g_state.engine2_upstream_probe_armed_slot68_count;
        current_slot68_count = g_state.slot68_entered_count.load(std::memory_order_relaxed);
        if (current_slot68_count < armed_slot68_count + kEngine2UpstreamColdSlot68Threshold)
        {
            return;
        }

        candidate_index = g_state.engine2_upstream_source_candidate_index;
        target = g_state.engine2_upstream_probe_target;
        if (candidate_index != kInvalidSlot &&
            candidate_index < g_state.engine2_primary_caller_candidate_count)
        {
            candidate_depth = g_state.engine2_primary_caller_candidates[candidate_index].sample_depth;
        }

        g_state.engine2_upstream_probe_remove_requested.store(true, std::memory_order_relaxed);
    }

    wchar_t candidate_text[16] {};
    wchar_t depth_text[16] {};
    wchar_t waited_text[32] {};
    swprintf_s(candidate_text, L"%zu", candidate_index != kInvalidSlot ? candidate_index + 1 : 0ull);
    swprintf_s(depth_text, L"%u", candidate_depth);
    swprintf_s(waited_text, L"%llu", static_cast<unsigned long long>(current_slot68_count - armed_slot68_count));
    nskinzcs2::Logger::Info(
        std::wstring(L"GlobalResourceSwapper engine2 upstream cold probe retired: target=") +
        FormatPointer(target) +
        L", candidate=" +
        candidate_text +
        L", depth=" +
        depth_text +
        L", slot68_waited=" +
        waited_text +
        L", entered=0.");
}

void TryLogEngine2PrimaryDispatchSnapshotOnce(
    const Engine2EntryCapture* capture,
    const CallerInfo* info) noexcept
{
    static std::atomic<bool> s_logged {false};
    if (capture == nullptr || s_logged.load(std::memory_order_relaxed))
    {
        return;
    }

    Engine2WrapperDispatchSnapshot snapshot {};
    if (!TryCaptureEngine2PrimaryDispatchSnapshot(capture, snapshot))
    {
        return;
    }

    if (s_logged.exchange(true, std::memory_order_relaxed))
    {
        return;
    }

    const auto return_owner =
        info != nullptr ? info->return_owner : GetModuleFileNameOnly(GetModuleFromAddress(info != nullptr ? info->return_address : nullptr));
    const auto caller_owner =
        info != nullptr ? info->caller_owner : std::wstring {};
    const auto vtable_owner = GetModuleFileNameOnly(GetModuleFromAddress(snapshot.emulated_rax_after_mov));
    const auto slot_target_owner = GetModuleFileNameOnly(GetModuleFromAddress(snapshot.slot_target));
    const auto followed_target_owner = GetModuleFileNameOnly(GetModuleFromAddress(snapshot.followed_target));
    const auto base_target_owner = GetModuleFileNameOnly(GetModuleFromAddress(snapshot.base_target));
    const void* current_probe_target = nullptr;
    {
        std::scoped_lock lock(g_state_mutex);
        current_probe_target = g_state.engine2_probe_target;
    }

    wchar_t slot_text[32] {};
    wchar_t direct_text[8] {};
    wchar_t followed_text[8] {};
    wchar_t base_text[8] {};
    wchar_t probe_match_text[8] {};
    swprintf_s(slot_text, L"0x%X", kEngine2PrimaryDispatchSlotOffset);
    swprintf_s(direct_text, L"%u", snapshot.direct_executable ? 1u : 0u);
    swprintf_s(followed_text, L"%u", snapshot.followed_executable ? 1u : 0u);
    swprintf_s(base_text, L"%u", snapshot.base_executable ? 1u : 0u);
    const bool matches_current_probe =
        current_probe_target != nullptr &&
        (current_probe_target == snapshot.base_target ||
         current_probe_target == snapshot.followed_target ||
         current_probe_target == snapshot.slot_target);
    swprintf_s(probe_match_text, L"%u", matches_current_probe ? 1u : 0u);

    nskinzcs2::Logger::Info(
        std::wstring(L"GlobalResourceSwapper engine2 primary vcall live snapshot: ret=") +
        FormatPointer(info != nullptr ? info->return_address : nullptr) +
        L", ret_owner=" +
        return_owner +
        L", caller_base=" +
        FormatPointer(info != nullptr ? info->caller_base : nullptr) +
        L", caller_owner=" +
        caller_owner +
        L", live_rcx=" +
        FormatPointer(snapshot.rcx_object) +
        L", live_rax_vtable=" +
        FormatPointer(snapshot.emulated_rax_after_mov) +
        L", vtable_owner=" +
        vtable_owner +
        L", slot=" +
        slot_text +
        L", live_slot_entry=[rax+0x40]=" +
        FormatPointer(snapshot.slot_entry_address) +
        L", live_call_target=" +
        FormatPointer(snapshot.slot_target) +
        L", call_target_owner=" +
        slot_target_owner +
        L", followed=" +
        FormatPointer(snapshot.followed_target) +
        L", followed_owner=" +
        followed_target_owner +
        L", base=" +
        FormatPointer(snapshot.base_target) +
        L", base_owner=" +
        base_target_owner +
        L", direct_exec=" +
        direct_text +
        L", followed_exec=" +
        followed_text +
        L", base_exec=" +
        base_text +
        L", matches_current_primary_probe=" +
        probe_match_text +
        L".");

    LogTargetBytes(
        L"GlobalResourceSwapper engine2 primary vcall object bytes",
        snapshot.rcx_object);
    LogTargetBytes(
        L"GlobalResourceSwapper engine2 primary vcall vtable bytes",
        snapshot.emulated_rax_after_mov);
    if (snapshot.direct_executable)
    {
        LogTargetBytes(
            L"GlobalResourceSwapper engine2 primary vcall slot target bytes",
            snapshot.slot_target);
    }
    if (snapshot.followed_executable && snapshot.followed_target != snapshot.slot_target)
    {
        LogTargetBytes(
            L"GlobalResourceSwapper engine2 primary vcall followed target bytes",
            snapshot.followed_target);
    }
    if (snapshot.base_executable &&
        snapshot.base_target != snapshot.followed_target &&
        snapshot.base_target != snapshot.slot_target)
    {
        LogTargetBytes(
            L"GlobalResourceSwapper engine2 primary vcall base target bytes",
            snapshot.base_target);
    }
}

void TryCaptureEngine2UpstreamLiveObject(
    const CallerInfo& info,
    const Engine2EntryCapture* capture) noexcept
{
    if (capture == nullptr || info.caller_base == nullptr)
    {
        return;
    }

    const auto* object = reinterpret_cast<const void*>(capture->rcx);
    if (object == nullptr || reinterpret_cast<std::uintptr_t>(object) < 0x10000ull)
    {
        return;
    }

    void* vtable = nullptr;
    SafeReadValue(object, vtable);
    const std::array<const void*, 8> register_candidates {{
        reinterpret_cast<const void*>(capture->rbx),
        reinterpret_cast<const void*>(capture->rbp),
        reinterpret_cast<const void*>(capture->rsi),
        reinterpret_cast<const void*>(capture->rdi),
        reinterpret_cast<const void*>(capture->r12),
        reinterpret_cast<const void*>(capture->r13),
        reinterpret_cast<const void*>(capture->r14),
        reinterpret_cast<const void*>(capture->r15),
    }};

    bool stored = false;
    {
        std::unique_lock lock(g_state_mutex, std::try_to_lock);
        if (!lock.owns_lock() || !g_state.installed)
        {
            return;
        }

        if (g_state.engine2_upstream_probe_pending_target != nullptr &&
            g_state.engine2_upstream_probe_pending_target != info.caller_base)
        {
            return;
        }

        if (g_state.engine2_upstream_live_object == nullptr)
        {
            g_state.engine2_upstream_live_object = object;
            g_state.engine2_upstream_live_vtable = vtable;
            g_state.engine2_upstream_live_registers = register_candidates;
            stored = true;
        }
    }

    if (stored)
    {
        char line[512] {};
        sprintf_s(
            line,
            "[GlobalResourceSwapper] Engine2 primary live registers: rbx=%p rbp=%p rsi=%p rdi=%p r12=%p r13=%p r14=%p r15=%p",
            register_candidates[0],
            register_candidates[1],
            register_candidates[2],
            register_candidates[3],
            register_candidates[4],
            register_candidates[5],
            register_candidates[6],
            register_candidates[7]);

        nskinzcs2::Logger::Info(
            std::wstring(L"GlobalResourceSwapper engine2 upstream live object captured from primary probe: rcx=") +
            FormatPointer(object) +
            L", vtable=" +
            FormatPointer(vtable) +
            L", caller=" +
            FormatPointer(info.caller_base) +
            L".");
        nskinzcs2::Logger::InfoAnsi(line);
    }
}

void TryLogEngine2PrimaryCallerProbeWindow(const Engine2EntryCapture* capture) noexcept
{
    static std::atomic<std::uint32_t> s_probe_attempts {0};
    static std::atomic<bool> s_resource_found {false};
    static std::atomic<bool> s_exhausted_logged {false};

    if (capture == nullptr)
    {
        return;
    }

    const auto original_rsp = static_cast<std::uintptr_t>(capture->original_rsp);
    const void* return_address = nullptr;
    if (original_rsp >= 0x10000ull)
    {
        SafeReadValue(reinterpret_cast<const void*>(original_rsp), return_address);
    }

    TryPumpEngine2UpstreamFromPrimaryProbe(capture, return_address);

    if (s_resource_found.load(std::memory_order_relaxed))
    {
        return;
    }

    const auto attempt = s_probe_attempts.fetch_add(1, std::memory_order_relaxed) + 1;
    if (attempt > kEngine2CallerProbeMaxAttempts)
    {
        if (!s_exhausted_logged.exchange(true, std::memory_order_relaxed))
        {
            char exhausted_log[208] {};
            sprintf_s(
                exhausted_log,
                "[GlobalResourceSwapper] Engine2 Caller Probe Window exhausted after %u attempts without a resource-like carrier.",
                kEngine2CallerProbeMaxAttempts);
            nskinzcs2::Logger::InfoAnsi(exhausted_log);
        }
        return;
    }

    const auto return_owner = GetModuleFileNameOnly(GetModuleFromAddress(return_address));
    char return_owner_ansi[64] {};
    char header[320] {};
    WideToAnsiLossy(return_owner, return_owner_ansi, std::size(return_owner_ansi));
    sprintf_s(
        header,
        "[GlobalResourceSwapper] Engine2 Caller Probe: attempt=%u entered=%llu ret=%p ret_owner=%s rcx=%p rdx=%p r8=%p r9=%p",
        attempt,
        static_cast<unsigned long long>(g_state.engine2_probe_entered_count.load(std::memory_order_relaxed)),
        return_address,
        return_owner_ansi,
        reinterpret_cast<const void*>(capture->rcx),
        reinterpret_cast<const void*>(capture->rdx),
        reinterpret_cast<const void*>(capture->r8),
        reinterpret_cast<const void*>(capture->r9));
    nskinzcs2::Logger::InfoAnsi(header);

    Engine2CallerProbeContext context {};
    context.attempt = attempt;

    const auto* rcx_ptr = reinterpret_cast<const void*>(capture->rcx);
    if (rcx_ptr != nullptr && reinterpret_cast<std::uintptr_t>(rcx_ptr) >= 0x10000ull)
    {
        TryLogEngine2CallerProbeObject("rcx", rcx_ptr, context, 0);
    }

    const auto* rdx_ptr = reinterpret_cast<const void*>(capture->rdx);
    if (!context.logged_resource_sample && rdx_ptr != nullptr && reinterpret_cast<std::uintptr_t>(rdx_ptr) >= 0x10000ull)
    {
        TryLogEngine2CallerProbeObject("rdx", rdx_ptr, context, 0);
    }

    const auto* r8_ptr = reinterpret_cast<const void*>(capture->r8);
    if (!context.logged_resource_sample && r8_ptr != nullptr && reinterpret_cast<std::uintptr_t>(r8_ptr) >= 0x10000ull)
    {
        TryLogEngine2CallerProbeObject("r8", r8_ptr, context, 0);
    }

    const auto* r9_ptr = reinterpret_cast<const void*>(capture->r9);
    if (!context.logged_resource_sample && r9_ptr != nullptr && reinterpret_cast<std::uintptr_t>(r9_ptr) >= 0x10000ull)
    {
        TryLogEngine2CallerProbeObject("r9", r9_ptr, context, 0);
    }

    if (!context.logged_resource_sample)
    {
        for (const auto offset : kEngine2CallerStackPointerOffsets)
        {
            if (context.logged_resource_sample || context.logged_sample_count >= kQueryAbiProbeMaxLogsPerAttempt)
            {
                break;
            }

            TryLogEngine2CallerStackPointerSlot(original_rsp, offset, context);
        }
    }

    if (context.logged_resource_sample)
    {
        s_resource_found.store(true, std::memory_order_relaxed);
        char found_log[176] {};
        sprintf_s(
            found_log,
            "[GlobalResourceSwapper] Engine2 Caller Probe Result: resource-like carrier found on attempt=%u.",
            attempt);
        nskinzcs2::Logger::InfoAnsi(found_log);
    }
    else if (!context.logged_any_sample)
    {
        char none_log[208] {};
        sprintf_s(
            none_log,
            "[GlobalResourceSwapper] Engine2 Caller Probe Result: attempt=%u found no readable ASCII carriers in rcx/rdx/r8/r9/stack-pointer window.",
            attempt);
        nskinzcs2::Logger::InfoAnsi(none_log);
    }
    else
    {
        char no_resource_log[208] {};
        sprintf_s(
            no_resource_log,
            "[GlobalResourceSwapper] Engine2 Caller Probe Result: attempt=%u found ASCII carriers, but none matched resource-like patterns.",
            attempt);
        nskinzcs2::Logger::InfoAnsi(no_resource_log);
    }

    if (attempt == kEngine2CallerProbeMaxAttempts && !context.logged_resource_sample &&
        !s_exhausted_logged.exchange(true, std::memory_order_relaxed))
    {
        char exhausted_log[208] {};
        sprintf_s(
            exhausted_log,
            "[GlobalResourceSwapper] Engine2 Caller Probe Window exhausted after %u attempts without a resource-like carrier.",
            kEngine2CallerProbeMaxAttempts);
        nskinzcs2::Logger::InfoAnsi(exhausted_log);
    }
}

bool TryLogEngine2UpstreamWrapperDispatchSnapshotOnce(
    const Engine2EntryCapture* capture,
    const CallerInfo* info) noexcept
{
    static std::atomic<bool> s_logged {false};
    static std::atomic<bool> s_rejected_logged {false};
    static std::atomic<bool> s_unknown_logged {false};
    if (capture == nullptr)
    {
        return false;
    }

    if (s_logged.load(std::memory_order_relaxed))
    {
        return true;
    }
    Engine2WrapperDispatchSnapshot snapshot {};
    if (!TryCaptureEngine2UpstreamWrapperDispatchSnapshot(capture, snapshot))
    {
        return false;
    }

    const auto incoming_rax = reinterpret_cast<const void*>(capture->rax);
    const auto return_address = info != nullptr ? info->return_address : nullptr;
    const auto caller_base = info != nullptr ? info->caller_base : nullptr;
    const auto return_owner =
        info != nullptr ? info->return_owner : GetModuleFileNameOnly(GetModuleFromAddress(return_address));
    const auto caller_owner = info != nullptr ? info->caller_owner : std::wstring {};
    const auto incoming_rax_owner = GetModuleFileNameOnly(GetModuleFromAddress(incoming_rax));
    const auto emulated_rax_owner = GetModuleFileNameOnly(GetModuleFromAddress(snapshot.emulated_rax_after_mov));
    const auto slot_target_owner = GetModuleFileNameOnly(GetModuleFromAddress(snapshot.slot_target));
    const auto followed_target_owner = GetModuleFileNameOnly(GetModuleFromAddress(snapshot.followed_target));
    const auto base_target_owner = GetModuleFileNameOnly(GetModuleFromAddress(snapshot.base_target));
    const auto owner_decision = SelectEngine2UpstreamOwner(
        return_owner,
        caller_owner,
        emulated_rax_owner,
        slot_target_owner,
        followed_target_owner,
        base_target_owner);

    if (!owner_decision.accepted)
    {
        {
            std::scoped_lock lock(g_state_mutex);
            if (owner_decision.rejected)
            {
                g_state.engine2_upstream_last_rejected_object = snapshot.rcx_object;
                g_state.engine2_upstream_last_rejected_vtable = snapshot.emulated_rax_after_mov;
                g_state.engine2_upstream_last_rejected_target = snapshot.slot_target;
                g_state.engine2_upstream_last_rejected_target_base =
                    snapshot.base_target != nullptr
                        ? snapshot.base_target
                        : snapshot.followed_target != nullptr
                            ? snapshot.followed_target
                            : snapshot.slot_target;
                g_state.engine2_upstream_last_rejected_caller = const_cast<void*>(caller_base);
            }
            g_state.engine2_upstream_analysis_state =
                owner_decision.rejected
                    ? Engine2UpstreamAnalysisState::SkippedNonModelOwner
                    : Engine2UpstreamAnalysisState::AwaitingModelOwner;
        }

        const bool should_log =
            owner_decision.rejected
                ? !s_rejected_logged.exchange(true, std::memory_order_relaxed)
                : !s_unknown_logged.exchange(true, std::memory_order_relaxed);
        if (should_log)
        {
            const void* rejected_base =
                snapshot.base_target != nullptr
                    ? snapshot.base_target
                    : snapshot.followed_target != nullptr
                        ? snapshot.followed_target
                        : snapshot.slot_target;
            nskinzcs2::Logger::Info(
                std::wstring(L"GlobalResourceSwapper engine2 upstream wrapper precall snapshot skipped: owner_source=") +
                owner_decision.source +
                L", owner=" +
                owner_decision.owner +
                L", ret_owner=" +
                return_owner +
                L", caller_owner=" +
                caller_owner +
                L", emulated_rax_owner=" +
                emulated_rax_owner +
                L", call_target_owner=" +
                slot_target_owner +
                L", followed_owner=" +
                followed_target_owner +
                L", base_owner=" +
                base_target_owner +
                L", rejected_object=" +
                FormatPointer(snapshot.rcx_object) +
                L", rejected_vtable=" +
                FormatPointer(snapshot.emulated_rax_after_mov) +
                L", rejected_target=" +
                FormatPointer(snapshot.slot_target) +
                L", rejected_base=" +
                FormatPointer(rejected_base) +
                L", rejected_caller=" +
                FormatPointer(caller_base) +
                L".");
        }

        return false;
    }

    if (s_logged.exchange(true, std::memory_order_relaxed))
    {
        return true;
    }

    wchar_t slot_text[32] {};
    wchar_t caller_offset_text[32] {};
    wchar_t direct_text[8] {};
    wchar_t followed_text[8] {};
    wchar_t base_text[8] {};
    swprintf_s(slot_text, L"0x%X", kEngine2WrapperDispatchSlotOffset);
    swprintf_s(
        caller_offset_text,
        L"0x%llX",
        static_cast<unsigned long long>(info != nullptr ? info->caller_offset : 0ull));
    swprintf_s(direct_text, L"%u", snapshot.direct_executable ? 1u : 0u);
    swprintf_s(followed_text, L"%u", snapshot.followed_executable ? 1u : 0u);
    swprintf_s(base_text, L"%u", snapshot.base_executable ? 1u : 0u);

    {
        std::scoped_lock lock(g_state_mutex);
        g_state.engine2_upstream_live_object = snapshot.rcx_object;
        g_state.engine2_upstream_live_vtable = snapshot.emulated_rax_after_mov;
        g_state.engine2_upstream_live_entry_address = snapshot.slot_entry_address;
        g_state.engine2_upstream_live_target = snapshot.slot_target;
        g_state.engine2_upstream_live_target_base =
            snapshot.base_target != nullptr
                ? snapshot.base_target
                : snapshot.followed_target != nullptr
                    ? snapshot.followed_target
                    : snapshot.slot_target;
        g_state.engine2_upstream_last_rejected_object = nullptr;
        g_state.engine2_upstream_last_rejected_vtable = nullptr;
        g_state.engine2_upstream_last_rejected_target = nullptr;
        g_state.engine2_upstream_last_rejected_target_base = nullptr;
        g_state.engine2_upstream_last_rejected_caller = nullptr;
        g_state.engine2_upstream_analysis_state = Engine2UpstreamAnalysisState::ModelOwnerReady;
        g_state.engine2_upstream_probe_pending_return = nullptr;
        g_state.engine2_upstream_probe_pending_target = nullptr;
        g_state.engine2_upstream_probe_pending_offset = 0;
        g_state.engine2_upstream_probe_pending = false;
    }
    g_state.engine2_upstream_precall_snapshot_captured.store(true, std::memory_order_relaxed);
    g_state.engine2_upstream_probe_remove_requested.store(true, std::memory_order_relaxed);

    nskinzcs2::Logger::Info(
        std::wstring(L"GlobalResourceSwapper engine2 upstream wrapper precall live snapshot: ret=") +
        FormatPointer(return_address) +
        L", ret_owner=" +
        return_owner +
        L", caller_base=" +
        FormatPointer(caller_base) +
        L", caller_owner=" +
        caller_owner +
        L", caller_offset=" +
        caller_offset_text +
        L", incoming_rax=" +
        FormatPointer(incoming_rax) +
        L", incoming_rax_owner=" +
        incoming_rax_owner +
        L", live_rcx=" +
        FormatPointer(snapshot.rcx_object) +
        L", live_rax_after_mov=[rcx]=" +
        FormatPointer(snapshot.emulated_rax_after_mov) +
        L", emulated_rax_owner=" +
        emulated_rax_owner +
        L", slot=" +
        slot_text +
        L", live_slot_entry=[rax+0x2B8]=" +
        FormatPointer(snapshot.slot_entry_address) +
        L", live_call_target=" +
        FormatPointer(snapshot.slot_target) +
        L", call_target_owner=" +
        slot_target_owner +
        L", followed=" +
        FormatPointer(snapshot.followed_target) +
        L", followed_owner=" +
        followed_target_owner +
        L", base=" +
        FormatPointer(snapshot.base_target) +
        L", base_owner=" +
        base_target_owner +
        L", owner_source=" +
        owner_decision.source +
        L", owner=" +
        owner_decision.owner +
        L", direct_exec=" +
        direct_text +
        L", followed_exec=" +
        followed_text +
        L", base_exec=" +
        base_text +
        L", remove_after_snapshot=1" +
        L".");

    if (snapshot.direct_executable)
    {
        LogTargetBytes(
            L"GlobalResourceSwapper engine2 upstream wrapper slot target bytes",
            snapshot.slot_target);
    }
    if (snapshot.followed_executable && snapshot.followed_target != snapshot.slot_target)
    {
        LogTargetBytes(
            L"GlobalResourceSwapper engine2 upstream wrapper followed target bytes",
            snapshot.followed_target);
    }
    if (snapshot.base_executable &&
        snapshot.base_target != snapshot.followed_target &&
        snapshot.base_target != snapshot.slot_target)
    {
        LogTargetBytes(
            L"GlobalResourceSwapper engine2 upstream wrapper function base bytes",
            snapshot.base_target);
    }

    return true;
}

void TryLogEngine2UpstreamCallerProbeWindow(const Engine2EntryCapture* capture) noexcept
{
    if (capture == nullptr ||
        g_state.engine2_upstream_probe_window_resource_found ||
        g_state.engine2_upstream_probe_window_dispatch_snapshot_complete)
    {
        return;
    }

    const auto attempt = ++g_state.engine2_upstream_probe_window_attempts;
    if (attempt > kEngine2CallerProbeMaxAttempts)
    {
        if (!g_state.engine2_upstream_probe_window_exhausted_logged)
        {
            g_state.engine2_upstream_probe_window_exhausted_logged = true;
            char exhausted_log[224] {};
            sprintf_s(
                exhausted_log,
                "[GlobalResourceSwapper] Engine2 Upstream Probe Window exhausted after %u attempts without a resource-like carrier.",
                kEngine2CallerProbeMaxAttempts);
            nskinzcs2::Logger::InfoAnsi(exhausted_log);
        }
        return;
    }

    const auto original_rsp = static_cast<std::uintptr_t>(capture->original_rsp);
    const void* return_address = nullptr;
    if (original_rsp >= 0x10000ull)
    {
        SafeReadValue(reinterpret_cast<const void*>(original_rsp), return_address);
    }

    CallerInfo info {};
    const bool have_info = ResolveCallerInfo(return_address, info);
    if (have_info)
    {
        TryLogEngine2UpstreamCallerChainOnce(info);
    }

    if (TryLogEngine2UpstreamWrapperDispatchSnapshotOnce(capture, have_info ? &info : nullptr))
    {
        g_state.engine2_upstream_probe_window_dispatch_snapshot_complete = true;
        return;
    }

    char return_owner_ansi[64] {};
    char caller_owner_ansi[64] {};
    char header[512] {};
    WideToAnsiLossy(have_info ? info.return_owner : GetModuleFileNameOnly(GetModuleFromAddress(return_address)),
        return_owner_ansi,
        std::size(return_owner_ansi));
    WideToAnsiLossy(have_info ? info.caller_owner : std::wstring {}, caller_owner_ansi, std::size(caller_owner_ansi));
    sprintf_s(
        header,
        "[GlobalResourceSwapper] Engine2 Upstream Probe: attempt=%u entered=%llu ret=%p ret_owner=%s caller_base=%p caller_owner=%s caller_offset=0x%llX rax=%p rcx=%p rdx=%p r8=%p r9=%p",
        attempt,
        static_cast<unsigned long long>(g_state.engine2_upstream_probe_entered_count.load(std::memory_order_relaxed)),
        return_address,
        return_owner_ansi,
        have_info ? info.caller_base : nullptr,
        caller_owner_ansi,
        static_cast<unsigned long long>(have_info ? info.caller_offset : 0ull),
        reinterpret_cast<const void*>(capture->rax),
        reinterpret_cast<const void*>(capture->rcx),
        reinterpret_cast<const void*>(capture->rdx),
        reinterpret_cast<const void*>(capture->r8),
        reinterpret_cast<const void*>(capture->r9));
    nskinzcs2::Logger::InfoAnsi(header);

    Engine2CallerProbeContext context {};
    context.attempt = attempt;

    const auto* rax_ptr = reinterpret_cast<const void*>(capture->rax);
    if (rax_ptr != nullptr && reinterpret_cast<std::uintptr_t>(rax_ptr) >= 0x10000ull)
    {
        TryLogEngine2CallerProbeObject("rax", rax_ptr, context, 0);
    }

    const auto* rcx_ptr = reinterpret_cast<const void*>(capture->rcx);
    if (!context.logged_resource_sample && rcx_ptr != nullptr && reinterpret_cast<std::uintptr_t>(rcx_ptr) >= 0x10000ull)
    {
        TryLogEngine2CallerProbeObject("rcx", rcx_ptr, context, 0);
    }

    const auto* rdx_ptr = reinterpret_cast<const void*>(capture->rdx);
    if (!context.logged_resource_sample && rdx_ptr != nullptr && reinterpret_cast<std::uintptr_t>(rdx_ptr) >= 0x10000ull)
    {
        TryLogEngine2CallerProbeObject("rdx", rdx_ptr, context, 0);
    }

    const auto* r8_ptr = reinterpret_cast<const void*>(capture->r8);
    if (!context.logged_resource_sample && r8_ptr != nullptr && reinterpret_cast<std::uintptr_t>(r8_ptr) >= 0x10000ull)
    {
        TryLogEngine2CallerProbeObject("r8", r8_ptr, context, 0);
    }

    const auto* r9_ptr = reinterpret_cast<const void*>(capture->r9);
    if (!context.logged_resource_sample && r9_ptr != nullptr && reinterpret_cast<std::uintptr_t>(r9_ptr) >= 0x10000ull)
    {
        TryLogEngine2CallerProbeObject("r9", r9_ptr, context, 0);
    }

    if (!context.logged_resource_sample)
    {
        for (const auto offset : kEngine2CallerStackPointerOffsets)
        {
            if (context.logged_resource_sample || context.logged_sample_count >= kQueryAbiProbeMaxLogsPerAttempt)
            {
                break;
            }

            TryLogEngine2CallerStackPointerSlot(original_rsp, offset, context);
        }
    }

    if (context.logged_resource_sample)
    {
        g_state.engine2_upstream_probe_window_resource_found = true;
        char found_log[192] {};
        sprintf_s(
            found_log,
            "[GlobalResourceSwapper] Engine2 Upstream Probe Result: resource-like carrier found on attempt=%u.",
            attempt);
        nskinzcs2::Logger::InfoAnsi(found_log);
    }
    else if (!context.logged_any_sample)
    {
        char none_log[224] {};
        sprintf_s(
            none_log,
            "[GlobalResourceSwapper] Engine2 Upstream Probe Result: attempt=%u found no readable ASCII carriers in rax/rcx/rdx/r8/r9/stack-pointer window.",
            attempt);
        nskinzcs2::Logger::InfoAnsi(none_log);
    }
    else
    {
        char no_resource_log[224] {};
        sprintf_s(
            no_resource_log,
            "[GlobalResourceSwapper] Engine2 Upstream Probe Result: attempt=%u found ASCII carriers, but none matched resource-like patterns.",
            attempt);
        nskinzcs2::Logger::InfoAnsi(no_resource_log);
    }

    if (attempt == kEngine2CallerProbeMaxAttempts && !context.logged_resource_sample &&
        !g_state.engine2_upstream_probe_window_exhausted_logged)
    {
        g_state.engine2_upstream_probe_window_exhausted_logged = true;
        char exhausted_log[224] {};
        sprintf_s(
            exhausted_log,
            "[GlobalResourceSwapper] Engine2 Upstream Probe Window exhausted after %u attempts without a resource-like carrier.",
            kEngine2CallerProbeMaxAttempts);
        nskinzcs2::Logger::InfoAnsi(exhausted_log);
    }
}

void __fastcall Engine2PrimaryCallerProbeCallback(const Engine2EntryCapture* capture) noexcept
{
    g_state.engine2_probe_entered_count.fetch_add(1, std::memory_order_relaxed);
    UpdateThreadLocalEngine2PrimaryCapture(capture);
    TryLogEngine2PrimaryCallerProbeWindow(capture);
}

void __fastcall Engine2UpstreamCallerProbeCallback(const Engine2EntryCapture* capture) noexcept
{
    g_state.engine2_upstream_probe_entered_count.fetch_add(1, std::memory_order_relaxed);
    static thread_local bool s_inside_callback = false;
    if (capture == nullptr || s_inside_callback)
    {
        return;
    }

    s_inside_callback = true;
    std::uint32_t expected = 0;
    if (g_state.engine2_upstream_precall_capture_state.compare_exchange_strong(
            expected,
            1,
            std::memory_order_acq_rel,
            std::memory_order_relaxed))
    {
        std::memcpy(
            &g_state.engine2_upstream_precall_capture,
            capture,
            sizeof(Engine2EntryCapture));

        const auto original_rsp = static_cast<std::uintptr_t>(capture->original_rsp);
        const void* return_address = nullptr;
        if (original_rsp >= kMinUserModePointer)
        {
            SafeReadValue(reinterpret_cast<const void*>(original_rsp), return_address);
        }

        g_state.engine2_upstream_precall_return_address = return_address;
        g_state.engine2_upstream_precall_capture_state.store(2, std::memory_order_release);
    }

    s_inside_callback = false;
}

void TryDeactivateAnimationSlot120CallerProbe() noexcept
{
    if (!g_state.animation_slot120_probe_remove_requested.load(std::memory_order_relaxed))
    {
        return;
    }

    std::scoped_lock lock(g_state_mutex);
    if (!g_state.animation_slot120_probe_active || g_state.animation_slot120_probe_target == nullptr)
    {
        g_state.animation_slot120_probe_remove_requested.store(false, std::memory_order_relaxed);
        return;
    }

    const auto candidate_index = g_state.animation_slot120_source_candidate_index;
    const bool success = g_state.animation_slot120_probe_window_resource_found;
    void* target = g_state.animation_slot120_probe_target;
    void* stub = g_state.animation_slot120_probe_stub;
    MH_DisableHook(target);
    MH_RemoveHook(target);

    g_state.animation_slot120_probe_target = nullptr;
    g_state.animation_slot120_probe_original = nullptr;
    g_state.animation_slot120_probe_stub = nullptr;
    g_state.animation_slot120_probe_active = false;
    g_state.animation_slot120_probe_pending = false;
    g_state.animation_slot120_probe_pending_return = nullptr;
    g_state.animation_slot120_probe_pending_target = nullptr;
    g_state.animation_slot120_probe_pending_offset = 0;
    g_state.animation_slot120_source_candidate_index = kInvalidSlot;
    g_state.animation_slot120_probe_window_attempts = 0;
    g_state.animation_slot120_probe_window_resource_found = false;
    g_state.animation_slot120_probe_window_exhausted_logged = false;
    g_state.animation_slot120_probe_entered_count.store(0, std::memory_order_relaxed);
    g_state.animation_slot120_probe_remove_requested.store(false, std::memory_order_relaxed);

    if (candidate_index != kInvalidSlot &&
        candidate_index < g_state.animation_slot120_caller_candidate_count)
    {
        auto& candidate = g_state.animation_slot120_caller_candidates[candidate_index];
        candidate.queued = false;
        candidate.attempted = true;
        candidate.success = success;
    }

    if (stub != nullptr)
    {
        VirtualFree(stub, 0, MEM_RELEASE);
    }

    char log_line[256] {};
    sprintf_s(
        log_line,
        "[GlobalResourceSwapper] Animation Slot120 caller probe removed: target=%p candidate=%zu success=%u",
        target,
        candidate_index != kInvalidSlot ? candidate_index + 1 : 0ull,
        success ? 1u : 0u);
    nskinzcs2::Logger::InfoAnsi(log_line);
}

void TryActivateDeferredAnimationSlot120CallerProbe() noexcept
{
    std::scoped_lock lock(g_state_mutex);
    const auto candidate_index = g_state.animation_slot120_source_candidate_index;
    if (!g_state.installed ||
        g_state.animation_slot120_probe_active ||
        !g_state.animation_slot120_probe_pending ||
        g_state.animation_slot120_probe_pending_target == nullptr)
    {
        return;
    }

    void* pending_target = g_state.animation_slot120_probe_pending_target;
    if (pending_target == g_state.engine2_probe_target)
    {
        g_state.animation_slot120_probe_pending_return = nullptr;
        g_state.animation_slot120_probe_pending_target = nullptr;
        g_state.animation_slot120_probe_pending_offset = 0;
        g_state.animation_slot120_probe_pending = false;
        g_state.animation_slot120_source_candidate_index = kInvalidSlot;
        if (candidate_index != kInvalidSlot &&
            candidate_index < g_state.animation_slot120_caller_candidate_count)
        {
            auto& candidate = g_state.animation_slot120_caller_candidates[candidate_index];
            candidate.queued = false;
            candidate.attempted = true;
        }
        return;
    }

    if (!IsExecutableAddress(pending_target))
    {
        g_state.animation_slot120_probe_pending_return = nullptr;
        g_state.animation_slot120_probe_pending_target = nullptr;
        g_state.animation_slot120_probe_pending_offset = 0;
        g_state.animation_slot120_probe_pending = false;
        g_state.animation_slot120_source_candidate_index = kInvalidSlot;
        if (candidate_index != kInvalidSlot &&
            candidate_index < g_state.animation_slot120_caller_candidate_count)
        {
            g_state.animation_slot120_caller_candidates[candidate_index].queued = false;
        }
        nskinzcs2::Logger::Warn(
            std::wstring(L"GlobalResourceSwapper animation slot120 deferred activation skipped: queued caller is not executable, target=") +
            FormatPointer(pending_target));
        return;
    }

    LogTargetBytes(L"GlobalResourceSwapper animation slot120 caller deferred target", pending_target);

    void* stub = CreateAnimationSlot120CallerProbeStub();
    if (stub == nullptr)
    {
        g_state.animation_slot120_probe_pending_return = nullptr;
        g_state.animation_slot120_probe_pending_target = nullptr;
        g_state.animation_slot120_probe_pending_offset = 0;
        g_state.animation_slot120_probe_pending = false;
        g_state.animation_slot120_source_candidate_index = kInvalidSlot;
        if (candidate_index != kInvalidSlot &&
            candidate_index < g_state.animation_slot120_caller_candidate_count)
        {
            g_state.animation_slot120_caller_candidates[candidate_index].queued = false;
        }
        nskinzcs2::Logger::Warn(L"GlobalResourceSwapper animation slot120 deferred activation failed: probe stub allocation failed.");
        return;
    }

    void* original_raw = nullptr;
    const auto create_status = MH_CreateHook(pending_target, stub, &original_raw);
    LogMinHookStep(L"MH_CreateHook[animation_slot120_caller_probe]", create_status, pending_target);
    if (create_status != MH_OK)
    {
        g_state.animation_slot120_probe_pending_return = nullptr;
        g_state.animation_slot120_probe_pending_target = nullptr;
        g_state.animation_slot120_probe_pending_offset = 0;
        g_state.animation_slot120_probe_pending = false;
        g_state.animation_slot120_source_candidate_index = kInvalidSlot;
        if (candidate_index != kInvalidSlot &&
            candidate_index < g_state.animation_slot120_caller_candidate_count)
        {
            auto& candidate = g_state.animation_slot120_caller_candidates[candidate_index];
            candidate.queued = false;
            if (create_status == MH_ERROR_ALREADY_CREATED)
            {
                candidate.attempted = true;
            }
        }
        VirtualFree(stub, 0, MEM_RELEASE);
        return;
    }

    if (!PatchStubAddress(stub, kEngine2ProbeOriginalAddressOffset, original_raw))
    {
        nskinzcs2::Logger::Warn(L"GlobalResourceSwapper animation slot120 deferred activation failed: unable to patch original trampoline into stub.");
        g_state.animation_slot120_probe_pending_return = nullptr;
        g_state.animation_slot120_probe_pending_target = nullptr;
        g_state.animation_slot120_probe_pending_offset = 0;
        g_state.animation_slot120_probe_pending = false;
        g_state.animation_slot120_source_candidate_index = kInvalidSlot;
        if (candidate_index != kInvalidSlot &&
            candidate_index < g_state.animation_slot120_caller_candidate_count)
        {
            g_state.animation_slot120_caller_candidates[candidate_index].queued = false;
        }
        MH_RemoveHook(pending_target);
        VirtualFree(stub, 0, MEM_RELEASE);
        return;
    }

    const auto enable_status = MH_EnableHook(pending_target);
    LogMinHookStep(L"MH_EnableHook[animation_slot120_caller_probe]", enable_status, pending_target);
    if (enable_status != MH_OK && enable_status != MH_ERROR_ENABLED)
    {
        g_state.animation_slot120_probe_pending_return = nullptr;
        g_state.animation_slot120_probe_pending_target = nullptr;
        g_state.animation_slot120_probe_pending_offset = 0;
        g_state.animation_slot120_probe_pending = false;
        g_state.animation_slot120_source_candidate_index = kInvalidSlot;
        if (candidate_index != kInvalidSlot &&
            candidate_index < g_state.animation_slot120_caller_candidate_count)
        {
            g_state.animation_slot120_caller_candidates[candidate_index].queued = false;
        }
        MH_RemoveHook(pending_target);
        VirtualFree(stub, 0, MEM_RELEASE);
        return;
    }

    g_state.animation_slot120_probe_target = pending_target;
    g_state.animation_slot120_probe_original = original_raw;
    g_state.animation_slot120_probe_stub = stub;
    g_state.animation_slot120_probe_active = true;
    g_state.animation_slot120_probe_pending_return = nullptr;
    g_state.animation_slot120_probe_pending_target = nullptr;
    g_state.animation_slot120_probe_pending_offset = 0;
    g_state.animation_slot120_probe_pending = false;
    g_state.animation_slot120_probe_entered_count.store(0, std::memory_order_relaxed);
    g_state.animation_slot120_probe_window_attempts = 0;
    g_state.animation_slot120_probe_window_resource_found = false;
    g_state.animation_slot120_probe_window_exhausted_logged = false;
    g_state.animation_slot120_probe_remove_requested.store(false, std::memory_order_relaxed);

    char log_line[256] {};
    sprintf_s(
        log_line,
        "[GlobalResourceSwapper] Animation Slot120 caller probe armed: target=%p candidate=%zu",
        pending_target,
        candidate_index != kInvalidSlot ? candidate_index + 1 : 0ull);
    nskinzcs2::Logger::InfoAnsi(log_line);
}

void TryDeactivateOneShotEngine2UpstreamCallerProbe() noexcept
{
    if (!g_state.engine2_upstream_probe_remove_requested.load(std::memory_order_relaxed))
    {
        return;
    }

    std::scoped_lock lock(g_state_mutex);
    if (!g_state.engine2_upstream_probe_active || g_state.engine2_upstream_probe_target == nullptr)
    {
        g_state.engine2_upstream_probe_remove_requested.store(false, std::memory_order_relaxed);
        return;
    }

    const auto candidate_index = g_state.engine2_upstream_source_candidate_index;
    const bool success = g_state.engine2_upstream_precall_snapshot_captured.load(std::memory_order_relaxed);
    void* target = g_state.engine2_upstream_probe_target;
    void* stub = g_state.engine2_upstream_probe_stub;
    MH_DisableHook(target);
    MH_RemoveHook(target);

    g_state.engine2_upstream_probe_target = nullptr;
    g_state.engine2_upstream_probe_original = nullptr;
    g_state.engine2_upstream_probe_stub = nullptr;
    g_state.engine2_upstream_probe_active = false;
    g_state.engine2_upstream_probe_pending = false;
    g_state.engine2_upstream_probe_armed_slot68_count = 0;
    g_state.engine2_upstream_probe_pending_return = nullptr;
    g_state.engine2_upstream_probe_pending_target = nullptr;
    g_state.engine2_upstream_probe_pending_offset = 0;
    g_state.engine2_upstream_source_candidate_index = kInvalidSlot;
    g_state.engine2_upstream_probe_window_attempts = 0;
    g_state.engine2_upstream_probe_window_resource_found = false;
    g_state.engine2_upstream_probe_window_dispatch_snapshot_complete = false;
    g_state.engine2_upstream_probe_window_exhausted_logged = false;
    g_state.engine2_upstream_keep_armed_logged = false;
    g_state.engine2_upstream_wait_exhausted_logged = false;
    g_state.engine2_upstream_probe_remove_requested.store(false, std::memory_order_relaxed);

    if (candidate_index != kInvalidSlot &&
        candidate_index < g_state.engine2_primary_caller_candidate_count)
    {
        auto& candidate = g_state.engine2_primary_caller_candidates[candidate_index];
        candidate.queued = false;
        candidate.attempted = true;
        candidate.success = success;
    }

    if (stub != nullptr)
    {
        VirtualFree(stub, 0, MEM_RELEASE);
    }

    wchar_t candidate_text[16] {};
    wchar_t success_text[8] {};
    swprintf_s(candidate_text, L"%zu", candidate_index != kInvalidSlot ? candidate_index + 1 : 0ull);
    swprintf_s(success_text, L"%u", success ? 1u : 0u);
    nskinzcs2::Logger::Info(
        std::wstring(L"GlobalResourceSwapper engine2 upstream caller probe removed after live precall snapshot: target=") +
        FormatPointer(target) +
        L", candidate=" +
        candidate_text +
        L", success=" +
        success_text +
        L".");
}

void TryProcessEngine2UpstreamPrecallCapture() noexcept
{
    std::uint32_t expected = 2;
    if (!g_state.engine2_upstream_precall_capture_state.compare_exchange_strong(
            expected,
            3,
            std::memory_order_acq_rel,
            std::memory_order_relaxed))
    {
        return;
    }

    const Engine2EntryCapture capture = g_state.engine2_upstream_precall_capture;
    const void* return_address = g_state.engine2_upstream_precall_return_address;

    CallerInfo info {};
    const bool have_info = ResolveCallerInfo(return_address, info);
    if (!TryLogEngine2UpstreamWrapperDispatchSnapshotOnce(&capture, have_info ? &info : nullptr))
    {
        const auto entered = g_state.engine2_upstream_probe_entered_count.load(std::memory_order_relaxed);
        if (!g_state.engine2_upstream_keep_armed_logged)
        {
            g_state.engine2_upstream_keep_armed_logged = true;
            nskinzcs2::Logger::Info(
                L"GlobalResourceSwapper engine2 upstream caller probe kept armed after non-model snapshot; it will now wait for the first model-owner instance instead of removing on the first localize/tier0 hit.");
        }

        if (entered >= kEngine2UpstreamProbeMaxEntryCaptures &&
            !g_state.engine2_upstream_wait_exhausted_logged)
        {
            g_state.engine2_upstream_wait_exhausted_logged = true;
            wchar_t entered_text[32] {};
            swprintf_s(entered_text, L"%llu", static_cast<unsigned long long>(entered));
            nskinzcs2::Logger::Warn(
                std::wstring(L"GlobalResourceSwapper engine2 upstream caller probe exhausted its live wait window without a model-owner snapshot: entered=") +
                entered_text +
                L".");
            g_state.engine2_upstream_probe_remove_requested.store(true, std::memory_order_relaxed);
        }

        g_state.engine2_upstream_precall_capture = {};
        g_state.engine2_upstream_precall_return_address = nullptr;
        g_state.engine2_upstream_precall_capture_state.store(0, std::memory_order_release);
        return;
    }

    g_state.engine2_upstream_precall_snapshot_captured.store(true, std::memory_order_relaxed);
    g_state.engine2_upstream_probe_remove_requested.store(true, std::memory_order_relaxed);
}

void TryActivateDeferredEngine2UpstreamCallerProbe() noexcept
{
    std::scoped_lock lock(g_state_mutex);
    const auto candidate_index = g_state.engine2_upstream_source_candidate_index;
    if (g_state.engine2_upstream_precall_snapshot_captured.load(std::memory_order_relaxed))
    {
        g_state.engine2_upstream_probe_pending_return = nullptr;
        g_state.engine2_upstream_probe_pending_target = nullptr;
        g_state.engine2_upstream_probe_pending_offset = 0;
        g_state.engine2_upstream_probe_pending = false;
        g_state.engine2_upstream_source_candidate_index = kInvalidSlot;
        if (candidate_index != kInvalidSlot &&
            candidate_index < g_state.engine2_primary_caller_candidate_count)
        {
            g_state.engine2_primary_caller_candidates[candidate_index].queued = false;
        }
        return;
    }

    if (!g_state.installed ||
        g_state.engine2_upstream_probe_active ||
        !g_state.engine2_upstream_probe_pending ||
        g_state.engine2_upstream_probe_pending_target == nullptr)
    {
        return;
    }

    const void* pending_return = g_state.engine2_upstream_probe_pending_return;
    void* pending_target = g_state.engine2_upstream_probe_pending_target;
    const auto pending_offset = g_state.engine2_upstream_probe_pending_offset;

    if (pending_target == nullptr)
    {
        return;
    }

    if (!IsExecutableAddress(pending_target))
    {
        g_state.engine2_upstream_probe_pending_return = nullptr;
        g_state.engine2_upstream_probe_pending_target = nullptr;
        g_state.engine2_upstream_probe_pending_offset = 0;
        g_state.engine2_upstream_probe_pending = false;
        g_state.engine2_upstream_source_candidate_index = kInvalidSlot;
        if (candidate_index != kInvalidSlot &&
            candidate_index < g_state.engine2_primary_caller_candidate_count)
        {
            g_state.engine2_primary_caller_candidates[candidate_index].queued = false;
        }
        nskinzcs2::Logger::Warn(
            std::wstring(L"GlobalResourceSwapper engine2 upstream deferred activation skipped: queued caller is not executable, target=") +
            FormatPointer(pending_target));
        return;
    }

    LogTargetBytes(L"GlobalResourceSwapper engine2 upstream caller deferred target", pending_target);

    void* stub = CreateEngine2UpstreamCallerProbeStub();
    if (stub == nullptr)
    {
        g_state.engine2_upstream_probe_pending_return = nullptr;
        g_state.engine2_upstream_probe_pending_target = nullptr;
        g_state.engine2_upstream_probe_pending_offset = 0;
        g_state.engine2_upstream_probe_pending = false;
        g_state.engine2_upstream_source_candidate_index = kInvalidSlot;
        if (candidate_index != kInvalidSlot &&
            candidate_index < g_state.engine2_primary_caller_candidate_count)
        {
            g_state.engine2_primary_caller_candidates[candidate_index].queued = false;
        }
        nskinzcs2::Logger::Warn(L"GlobalResourceSwapper engine2 upstream caller deferred activation failed: probe stub allocation failed.");
        return;
    }

    void* original_raw = nullptr;
    const auto create_status = MH_CreateHook(pending_target, stub, &original_raw);
    LogMinHookStep(L"MH_CreateHook[engine2_upstream_caller_probe]", create_status, pending_target);
    if (create_status != MH_OK)
    {
        g_state.engine2_upstream_probe_pending_return = nullptr;
        g_state.engine2_upstream_probe_pending_target = nullptr;
        g_state.engine2_upstream_probe_pending_offset = 0;
        g_state.engine2_upstream_probe_pending = false;
        g_state.engine2_upstream_source_candidate_index = kInvalidSlot;
        if (candidate_index != kInvalidSlot &&
            candidate_index < g_state.engine2_primary_caller_candidate_count)
        {
            g_state.engine2_primary_caller_candidates[candidate_index].queued = false;
        }
        VirtualFree(stub, 0, MEM_RELEASE);
        return;
    }

    if (!PatchStubAddress(stub, kEngine2ProbeOriginalAddressOffset, original_raw))
    {
        nskinzcs2::Logger::Warn(L"GlobalResourceSwapper engine2 upstream deferred activation failed: unable to patch original trampoline into stub.");
        g_state.engine2_upstream_probe_pending_return = nullptr;
        g_state.engine2_upstream_probe_pending_target = nullptr;
        g_state.engine2_upstream_probe_pending_offset = 0;
        g_state.engine2_upstream_probe_pending = false;
        g_state.engine2_upstream_source_candidate_index = kInvalidSlot;
        if (candidate_index != kInvalidSlot &&
            candidate_index < g_state.engine2_primary_caller_candidate_count)
        {
            g_state.engine2_primary_caller_candidates[candidate_index].queued = false;
        }
        MH_RemoveHook(pending_target);
        VirtualFree(stub, 0, MEM_RELEASE);
        return;
    }

    const auto enable_status = MH_EnableHook(pending_target);
    LogMinHookStep(L"MH_EnableHook[engine2_upstream_caller_probe]", enable_status, pending_target);
    if (enable_status != MH_OK && enable_status != MH_ERROR_ENABLED)
    {
        g_state.engine2_upstream_probe_pending_return = nullptr;
        g_state.engine2_upstream_probe_pending_target = nullptr;
        g_state.engine2_upstream_probe_pending_offset = 0;
        g_state.engine2_upstream_probe_pending = false;
        g_state.engine2_upstream_source_candidate_index = kInvalidSlot;
        if (candidate_index != kInvalidSlot &&
            candidate_index < g_state.engine2_primary_caller_candidate_count)
        {
            g_state.engine2_primary_caller_candidates[candidate_index].queued = false;
        }
        MH_RemoveHook(pending_target);
        VirtualFree(stub, 0, MEM_RELEASE);
        return;
    }

    g_state.engine2_upstream_probe_target = pending_target;
    g_state.engine2_upstream_probe_original = original_raw;
    g_state.engine2_upstream_probe_stub = stub;
    g_state.engine2_upstream_probe_active = true;
    g_state.engine2_upstream_probe_pending_return = nullptr;
    g_state.engine2_upstream_probe_pending_target = nullptr;
    g_state.engine2_upstream_probe_pending_offset = 0;
    g_state.engine2_upstream_probe_pending = false;
    g_state.engine2_upstream_probe_armed_slot68_count =
        g_state.slot68_entered_count.load(std::memory_order_relaxed);
    g_state.engine2_upstream_probe_entered_count.store(0, std::memory_order_relaxed);
    g_state.engine2_upstream_probe_window_attempts = 0;
    g_state.engine2_upstream_probe_window_resource_found = false;
    g_state.engine2_upstream_probe_window_dispatch_snapshot_complete = false;
    g_state.engine2_upstream_probe_window_exhausted_logged = false;
    g_state.engine2_upstream_keep_armed_logged = false;
    g_state.engine2_upstream_wait_exhausted_logged = false;

    wchar_t offset_text[32] {};
    swprintf_s(offset_text, L"0x%llX", static_cast<unsigned long long>(pending_offset));
    nskinzcs2::Logger::Info(
        std::wstring(L"GlobalResourceSwapper engine2 upstream caller probe armed from deferred queue: ret=") +
        FormatPointer(pending_return) +
        L", target=" +
        FormatPointer(pending_target) +
        L", original=" +
        FormatPointer(original_raw) +
        L", offset=" +
        offset_text +
        L".");
}

void TryInstallEngine2PrimaryCallerProbe(const CallerInfo& info) noexcept
{
    if (info.caller_base == nullptr ||
        info.caller_offset == 0 ||
        !ModuleNameEquals(info.caller_owner, L"engine2.dll") ||
        !IsExecutableAddress(info.caller_base))
    {
        return;
    }

    std::unique_lock lock(g_state_mutex, std::try_to_lock);
    if (!lock.owns_lock())
    {
        return;
    }

    if (g_state.engine2_probe_active)
    {
        return;
    }

    if (info.caller_offset != kLegacyEngine2PrimaryCallerOffset)
    {
        wchar_t offset_text[32] {};
        swprintf_s(offset_text, L"0x%llX", static_cast<unsigned long long>(info.caller_offset));
        nskinzcs2::Logger::Info(
            std::wstring(L"GlobalResourceSwapper learned current engine2 primary caller from live resource-query provenance: offset=") +
            offset_text + L" (legacy hint was 0x2B8C)." );
    }

    LogTargetBytes(L"GlobalResourceSwapper engine2 primary caller live target", info.caller_base);

    void* stub = CreateEngine2PrimaryCallerProbeStub();
    if (stub == nullptr)
    {
        nskinzcs2::Logger::Warn(L"GlobalResourceSwapper engine2 caller probe stub allocation failed.");
        return;
    }

    void* original_raw = nullptr;
    const auto create_status = MH_CreateHook(info.caller_base, stub, &original_raw);
    LogMinHookStep(L"MH_CreateHook[engine2_primary_caller_probe]", create_status, info.caller_base);
    if (create_status != MH_OK)
    {
        VirtualFree(stub, 0, MEM_RELEASE);
        return;
    }

    if (!PatchStubAddress(stub, kEngine2ProbeOriginalAddressOffset, original_raw))
    {
        nskinzcs2::Logger::Warn(L"GlobalResourceSwapper engine2 caller probe failed: unable to patch original trampoline into stub.");
        MH_RemoveHook(info.caller_base);
        VirtualFree(stub, 0, MEM_RELEASE);
        return;
    }

    const auto enable_status = MH_EnableHook(info.caller_base);
    LogMinHookStep(L"MH_EnableHook[engine2_primary_caller_probe]", enable_status, info.caller_base);
    if (enable_status != MH_OK && enable_status != MH_ERROR_ENABLED)
    {
        MH_RemoveHook(info.caller_base);
        VirtualFree(stub, 0, MEM_RELEASE);
        return;
    }

    g_state.engine2_probe_target = info.caller_base;
    g_state.engine2_probe_original = original_raw;
    g_state.engine2_probe_stub = stub;
    g_state.engine2_probe_active = true;
    g_state.engine2_probe_entered_count.store(0, std::memory_order_relaxed);

    nskinzcs2::Logger::Info(
        std::wstring(L"GlobalResourceSwapper engine2 primary caller probe armed from live Query caller: target=") +
        FormatPointer(info.caller_base) +
        L", original=" +
        FormatPointer(original_raw) +
        L", offset=0x2B8C.");
}

void TryLogQueryCallerProbe(
    const std::uint32_t attempt,
    const void* return_address) noexcept
{
    CallerInfo info {};
    if (!ResolveCallerInfo(return_address, info))
    {
        return;
    }

    char return_owner_ansi[64] {};
    char caller_owner_ansi[64] {};
    char return_bytes[3 * kMachineByteCount + 8] {};
    char caller_bytes[3 * kMachineByteCount + 8] {};
    char line[640] {};

    WideToAnsiLossy(info.return_owner, return_owner_ansi, std::size(return_owner_ansi));
    WideToAnsiLossy(info.caller_owner, caller_owner_ansi, std::size(caller_owner_ansi));
    FormatMachineBytesAnsi(info.before_return, kMachineByteCount, return_bytes, std::size(return_bytes));
    FormatMachineBytesAnsi(info.caller_base, kMachineByteCount, caller_bytes, std::size(caller_bytes));

    sprintf_s(
        line,
        "[GlobalResourceSwapper] Slot68 Caller Probe: attempt=%u ret=%p ret_owner=%s caller_base=%p caller_owner=%s caller_offset=0x%llX",
        attempt,
        info.return_address,
        return_owner_ansi,
        info.caller_base,
        caller_owner_ansi,
        static_cast<unsigned long long>(info.caller_offset));
    nskinzcs2::Logger::InfoAnsi(line);

    sprintf_s(
        line,
        "[GlobalResourceSwapper] Slot68 Caller Probe Bytes[ret-32..ret): attempt=%u base=%p bytes=%s",
        attempt,
        info.before_return,
        return_bytes);
    nskinzcs2::Logger::InfoAnsi(line);

    sprintf_s(
        line,
        "[GlobalResourceSwapper] Slot68 Caller Probe Bytes[caller_base]: attempt=%u base=%p bytes=%s",
        attempt,
        info.caller_base,
        caller_bytes);
    nskinzcs2::Logger::InfoAnsi(line);

    TryLogEngine2CallerCallWindowOnce(info);
    TryInstallEngine2PrimaryCallerProbe(info);
}

void TryLogQueryAbiProbeWindow(
    void* this_ptr,
    const char* path,
    const std::uint64_t arg3,
    const std::uint64_t arg4,
    const void* return_address) noexcept
{
    static std::atomic<std::uint32_t> s_query_abi_probe_attempts {0};
    static std::atomic<bool> s_query_abi_probe_resource_found {false};
    static std::atomic<bool> s_query_abi_probe_exhausted_logged {false};

    if (s_query_abi_probe_resource_found.load(std::memory_order_relaxed))
    {
        return;
    }

    const auto attempt = s_query_abi_probe_attempts.fetch_add(1, std::memory_order_relaxed) + 1;
    if (attempt > kQueryAbiProbeMaxAttempts)
    {
        if (!s_query_abi_probe_exhausted_logged.exchange(true, std::memory_order_relaxed))
        {
            char exhausted_log[192] {};
            sprintf_s(
                exhausted_log,
                "[GlobalResourceSwapper] Slot68 ABI Probe Window exhausted after %u attempts without a resource-like carrier.",
                kQueryAbiProbeMaxAttempts);
            nskinzcs2::Logger::InfoAnsi(exhausted_log);
        }
        return;
    }

    char header[256] {};
    sprintf_s(
        header,
        "[GlobalResourceSwapper] Slot68 ABI Probe: attempt=%u entered=%llu this=%p arg2=%p arg3=%p arg4=%p ret=%p",
        attempt,
        static_cast<unsigned long long>(g_state.entered_count.load(std::memory_order_relaxed)),
        this_ptr,
        path,
        reinterpret_cast<const void*>(arg3),
        reinterpret_cast<const void*>(arg4),
        return_address);
    nskinzcs2::Logger::InfoAnsi(header);
    TryLogQueryCallerProbe(attempt, return_address);

    QueryAbiProbeContext context {};
    context.attempt = attempt;

    if (path != nullptr && reinterpret_cast<std::uintptr_t>(path) >= 0x10000ull)
    {
        TryLogQueryAbiProbeObject("arg2", path, context, 0);
    }

    const auto* arg3_ptr = reinterpret_cast<const void*>(arg3);
    if (!context.logged_resource_sample && arg3_ptr != nullptr && reinterpret_cast<std::uintptr_t>(arg3_ptr) >= 0x10000ull)
    {
        TryLogQueryAbiProbeObject("arg3", arg3_ptr, context, 0);
    }

    const auto* arg4_ptr = reinterpret_cast<const void*>(arg4);
    if (!context.logged_resource_sample && arg4_ptr != nullptr && reinterpret_cast<std::uintptr_t>(arg4_ptr) >= 0x10000ull)
    {
        TryLogQueryAbiProbeObject("arg4", arg4_ptr, context, 0);
    }

    if (context.logged_resource_sample)
    {
        s_query_abi_probe_resource_found.store(true, std::memory_order_relaxed);
        char found_log[160] {};
        sprintf_s(
            found_log,
            "[GlobalResourceSwapper] Slot68 ABI Probe Result: resource-like carrier found on attempt=%u.",
            attempt);
        nskinzcs2::Logger::InfoAnsi(found_log);
    }
    else if (!context.logged_any_sample)
    {
        char none_log[192] {};
        sprintf_s(
            none_log,
            "[GlobalResourceSwapper] Slot68 ABI Probe Result: attempt=%u found no readable ASCII carriers in arg2/arg3/arg4 recursive probe window.",
            attempt);
        nskinzcs2::Logger::InfoAnsi(none_log);
    }
    else
    {
        char no_resource_log[192] {};
        sprintf_s(
            no_resource_log,
            "[GlobalResourceSwapper] Slot68 ABI Probe Result: attempt=%u found ASCII carriers, but none matched resource-like patterns.",
            attempt);
        nskinzcs2::Logger::InfoAnsi(no_resource_log);
    }

    if (attempt == kQueryAbiProbeMaxAttempts && !context.logged_resource_sample &&
        !s_query_abi_probe_exhausted_logged.exchange(true, std::memory_order_relaxed))
    {
        char exhausted_log[192] {};
        sprintf_s(
            exhausted_log,
            "[GlobalResourceSwapper] Slot68 ABI Probe Window exhausted after %u attempts without a resource-like carrier.",
            kQueryAbiProbeMaxAttempts);
        nskinzcs2::Logger::InfoAnsi(exhausted_log);
    }
}

void TryLogQueryPathSampleOnce(const char* path) noexcept
{
    static std::atomic<bool> s_path_sample_logged {false};

    if (path == nullptr || s_path_sample_logged.load(std::memory_order_relaxed))
    {
        return;
    }

    __try
    {
        if (std::strstr(path, "knife") == nullptr && std::strstr(path, ".vmdl") == nullptr)
        {
            return;
        }

        if (!s_path_sample_logged.exchange(true, std::memory_order_relaxed))
        {
            char sample_log[640] {};
            sprintf_s(
                sample_log,
                "[GlobalResourceSwapper] Slot68 Path Sample: entered=%llu path=%s",
                static_cast<unsigned long long>(g_state.entered_count.load(std::memory_order_relaxed)),
                path);
            nskinzcs2::Logger::InfoAnsi(sample_log);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

void TryLogQueryCarrierPathSampleOnce(const char* label, const char* path) noexcept
{
    static std::atomic<bool> s_carrier_path_sample_logged {false};
    if (label == nullptr || path == nullptr || s_carrier_path_sample_logged.load(std::memory_order_relaxed))
    {
        return;
    }

    __try
    {
        if (std::strstr(path, "knife") == nullptr && std::strstr(path, ".vmdl") == nullptr)
        {
            return;
        }

        if (!s_carrier_path_sample_logged.exchange(true, std::memory_order_relaxed))
        {
            char sample_log[704] {};
            sprintf_s(
                sample_log,
                "[GlobalResourceSwapper] Slot68 Carrier Path Sample: entered=%llu field=%s path=%s",
                static_cast<unsigned long long>(g_state.entered_count.load(std::memory_order_relaxed)),
                label,
                path);
            nskinzcs2::Logger::InfoAnsi(sample_log);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

void TryLogQueryDetourEnteredOnce() noexcept
{
    static std::atomic<bool> s_detour_entered_logged {false};

    if (!s_detour_entered_logged.exchange(true, std::memory_order_relaxed))
    {
        char log_line[192] {};
        sprintf_s(
            log_line,
            "[GlobalResourceSwapper] Slot68 detour entered: entered=%llu",
            static_cast<unsigned long long>(g_state.entered_count.load(std::memory_order_relaxed)));
        nskinzcs2::Logger::InfoAnsi(log_line);
    }
}

std::uint64_t DispatchSiblingProbe(
    const std::size_t descriptor_index,
    void* this_ptr,
    const std::uint64_t arg2,
    const std::uint64_t arg3,
    const std::uint64_t arg4) noexcept
{
    const auto original = descriptor_index < g_state.sibling_hooks.size()
        ? g_state.sibling_hooks[descriptor_index].original
        : nullptr;
    if (original == nullptr)
    {
        return 0;
    }

    TryCaptureSiblingProbeSample(descriptor_index, this_ptr, arg2, arg3, arg4);
    if (descriptor_index == 2)
    {
        TryLogAnimationCarrierProbeWindow(this_ptr, arg2, arg3, arg4);
    }
    return original(this_ptr, arg2, arg3, arg4);
}

std::uint64_t __fastcall hkSiblingProbe248(
    void* this_ptr,
    const std::uint64_t arg2,
    const std::uint64_t arg3,
    const std::uint64_t arg4) noexcept
{
    return DispatchSiblingProbe(0, this_ptr, arg2, arg3, arg4);
}

std::uint64_t __fastcall hkSiblingProbe0A0(
    void* this_ptr,
    const std::uint64_t arg2,
    const std::uint64_t arg3,
    const std::uint64_t arg4) noexcept
{
    return DispatchSiblingProbe(1, this_ptr, arg2, arg3, arg4);
}

std::uint64_t __fastcall hkSiblingProbe120(
    void* this_ptr,
    const std::uint64_t arg2,
    const std::uint64_t arg3,
    const std::uint64_t arg4) noexcept
{
    CallerInfo info {};
    if (ResolveCallerInfo(_ReturnAddress(), info))
    {
        TryTrackAnimationSlot120CallerSource(info);
    }
    return DispatchSiblingProbe(2, this_ptr, arg2, arg3, arg4);
}

std::uint64_t __fastcall hkFileSystemSlotProbe(
    void* this_ptr,
    std::uint64_t arg2,
    const std::uint64_t arg3,
    const std::uint64_t arg4) noexcept
{
    const auto original = g_state.original;
    if (original == nullptr)
    {
        return 0;
    }

    g_state.entered_count.fetch_add(1, std::memory_order_relaxed);
    g_state.file_system_entered_count.fetch_add(1, std::memory_order_relaxed);
    g_state.file_system_probe_call_count.fetch_add(1, std::memory_order_relaxed);

    if (this_ptr == nullptr)
    {
        return original(this_ptr, arg2, arg3, arg4);
    }

    if (g_reentry_guard)
    {
        return original(this_ptr, arg2, arg3, arg4);
    }

    struct Guard
    {
        Guard() { g_reentry_guard = true; }
        ~Guard() { g_reentry_guard = false; }
    } guard {};

    TryCaptureFileSystemCarrierProbe(this_ptr, arg2, arg3, arg4, _ReturnAddress());

    if (g_state.file_system_capture_count.load(std::memory_order_acquire) == 0)
    {
        if (!TryCaptureFileSystemAmbushFromValue(arg2) &&
            !TryCaptureFileSystemAmbushFromValue(arg3))
        {
            TryCaptureFileSystemAmbushFromValue(arg4);
        }
    }

    return original(this_ptr, arg2, arg3, arg4);
}

std::uint64_t __fastcall hkResourceSystemSlot68(
    void* this_ptr,
    std::uint64_t arg2,
    const std::uint64_t arg3,
    const std::uint64_t arg4) noexcept
{
    const auto original = g_state.original;
    if (original == nullptr)
    {
        return 0;
    }

    g_state.entered_count.fetch_add(1, std::memory_order_relaxed);
    g_state.slot68_entered_count.fetch_add(1, std::memory_order_relaxed);
    TryLogQueryDetourEnteredOnce();

    if (this_ptr == nullptr)
    {
        return original(this_ptr, arg2, arg3, arg4);
    }

    if (g_reentry_guard)
    {
        return original(this_ptr, arg2, arg3, arg4);
    }

    struct Guard
    {
        Guard() { g_reentry_guard = true; }
        ~Guard() { g_reentry_guard = false; }
    } guard {};

    const void* return_address = _ReturnAddress();
    const auto* arg2_path = reinterpret_cast<const char*>(arg2);
    TryLogQueryAbiProbeWindow(this_ptr, arg2_path, arg3, arg4, return_address);

    bool redirected = TryRedirectQueryArg4Carrier(arg4);
    if (arg2_path != nullptr && reinterpret_cast<std::uintptr_t>(arg2_path) >= kMinUserModePointer)
    {
        char sample[256] {};
        if (TryCaptureAsciiSample(arg2_path, sample, std::size(sample)) && LooksLikeResourceSample(sample))
        {
            TryLogQueryPathSampleOnce(arg2_path);
        }

        if (const auto direct_redirect = TryResolveRedirectPath(arg2_path); direct_redirect != nullptr)
        {
            arg2 = reinterpret_cast<std::uint64_t>(direct_redirect);
            redirected = true;
        }
    }

    const auto* arg3_path = reinterpret_cast<const char*>(arg3);
    if (arg3_path != nullptr && reinterpret_cast<std::uintptr_t>(arg3_path) >= kMinUserModePointer)
    {
        char sample[256] {};
        if (TryCaptureAsciiSample(arg3_path, sample, std::size(sample)) && LooksLikeResourceSample(sample))
        {
            TryLogQueryCarrierPathSampleOnce("arg3", arg3_path);
        }
    }

    if (redirected)
    {
        nskinzcs2::GlobalResourceSwapperIncrementSuccess();
    }

    return original(this_ptr, arg2, arg3, arg4);
}
}

nskinzcs2::GlobalResourceSwapperInstallResult nskinzcs2::EnsureGlobalResourceSwapperInstalled(
    void* resource_handle_utils_interface,
    void* resource_system_interface,
    void* file_system_interface) noexcept
{
    std::scoped_lock lock(g_state_mutex);
    (void)resource_handle_utils_interface;
    (void)resource_system_interface;

    GlobalResourceSwapperInstallResult result {};
    if (g_state.installed)
    {
        if (file_system_interface != nullptr && file_system_interface != g_state.file_system_interface)
        {
            std::wstring line = L"GlobalResourceSwapper observed alternate file_system_interface while installed: current=";
            line += FormatPointer(g_state.file_system_interface);
            line += L", incoming=";
            line += FormatPointer(file_system_interface);
            line += L", route=";
            line += RouteText(g_state.route);
            line += L", entered=";
            line += std::to_wstring(g_state.file_system_entered_count.load(std::memory_order_relaxed));
            line += L", captures=";
            line += std::to_wstring(g_state.file_system_capture_count.load(std::memory_order_relaxed));
            nskinzcs2::Logger::Warn(line);
        }

        result.installed = true;
        result.already_installed = true;
        result.slot = g_state.slot;
        result.route = RouteText(g_state.route);
        result.reason = L"already_installed";
        return result;
    }

    LogOwnerRadar();
    {
        char build_line[192] {};
        sprintf_s(
            build_line,
            "[GlobalResourceSwapper] Build stamp: %s",
            kGlobalResourceSwapperBuildStamp);
        nskinzcs2::Logger::InfoAnsi(build_line);
    }
    {
        const HMODULE self_module = GetModuleFromAddress(reinterpret_cast<const void*>(&nskinzcs2::EnsureGlobalResourceSwapperInstalled));
        const auto self_path = GetModuleFilePath(self_module);
        nskinzcs2::Logger::Info(
            std::wstring(L"GlobalResourceSwapper loaded module path: ") +
            (self_path.empty() ? std::wstring(L"<unknown>") : self_path));
    }
    const auto file_system_owner = CollectOwnerSummary(
        L"file_system",
        file_system_interface,
        kFileSystemFirstSlotIndex,
        kFileSystemLastSlotIndex + 1);
    nskinzcs2::Logger::Info(
        L"GlobalResourceSwapper probable live owner: " +
        (file_system_owner.dominant_module.empty() ? std::wstring(L"<unknown>") : file_system_owner.dominant_module));

    if (file_system_interface == nullptr)
    {
        result.reason = L"file_system_interface_missing";
        return result;
    }

    const auto init_status = MH_Initialize();
    LogMinHookStep(L"MH_Initialize", init_status, file_system_interface);
    if (init_status != MH_OK && init_status != MH_ERROR_ALREADY_INITIALIZED)
    {
        result.reason = L"mh_initialize_failed";
        return result;
    }

    g_state.file_system_interface = file_system_interface;
    g_state.file_system_next_slot = kFileSystemKnownHotSlotIndex;
    g_state.file_system_scan_complete = false;
    g_state.file_system_flushed = false;
    g_state.file_system_carrier_probe_flushed = false;
    g_state.file_system_capture = {};
    g_state.file_system_carrier_probe_capture = {};
    g_state.file_system_entered_count.store(0, std::memory_order_relaxed);
    g_state.file_system_capture_count.store(0, std::memory_order_relaxed);
    g_state.file_system_capture_state.store(
        static_cast<std::uint32_t>(FileSystemCaptureState::Inactive),
        std::memory_order_relaxed);
    g_state.file_system_probe_call_count.store(0, std::memory_order_relaxed);
    g_state.file_system_pending_flush.store(false, std::memory_order_relaxed);
    g_state.file_system_carrier_probe_attempts.store(0, std::memory_order_relaxed);
    g_state.file_system_carrier_probe_done.store(false, std::memory_order_relaxed);
    g_state.file_system_carrier_probe_pending_flush.store(false, std::memory_order_relaxed);
    g_state.signature_match = nullptr;
    g_state.target = nullptr;
    g_state.original = nullptr;
    g_state.slot = kInvalidSlot;
    g_state.route = Route::FileSystemAmbush;
    g_state.vmt_patch = {};
    g_state.minhook_active = false;
    g_state.installed = false;
    g_state.entered_count.store(0, std::memory_order_relaxed);
    g_state.slot68_entered_count.store(0, std::memory_order_relaxed);
    g_state.success_count.store(0, std::memory_order_relaxed);
    g_state.sibling_active_mask.store(0, std::memory_order_relaxed);
    g_state.engine2_probe_entered_count.store(0, std::memory_order_relaxed);
    g_state.engine2_upstream_probe_entered_count.store(0, std::memory_order_relaxed);
    g_state.engine2_upstream_probe_pending_return = nullptr;
    g_state.engine2_upstream_probe_pending_target = nullptr;
    g_state.engine2_upstream_probe_pending_offset = 0;
    g_state.engine2_upstream_live_object = nullptr;
    g_state.engine2_upstream_live_vtable = nullptr;
    g_state.engine2_upstream_live_entry_address = nullptr;
    g_state.engine2_upstream_live_target = nullptr;
    g_state.engine2_upstream_live_target_base = nullptr;
    g_state.engine2_upstream_last_rejected_object = nullptr;
    g_state.engine2_upstream_last_rejected_vtable = nullptr;
    g_state.engine2_upstream_last_rejected_target = nullptr;
    g_state.engine2_upstream_last_rejected_target_base = nullptr;
    g_state.engine2_upstream_last_rejected_caller = nullptr;
    g_state.engine2_upstream_live_registers.fill(nullptr);
    g_state.engine2_upstream_probe_pending = false;
    g_state.engine2_upstream_probe_analysis_done = false;
    g_state.engine2_upstream_analysis_state = Engine2UpstreamAnalysisState::None;
    g_state.engine2_upstream_analysis_attempts = 0;
    g_state.engine2_upstream_analysis_total_direct_calls = 0;
    g_state.engine2_upstream_analysis_summary_count = 0;
    g_state.engine2_upstream_analysis_exception_code = 0;
    g_state.engine2_upstream_probe_armed_slot68_count = 0;
    g_state.engine2_upstream_probe_window_attempts = 0;
    g_state.engine2_upstream_probe_window_resource_found = false;
    g_state.engine2_upstream_probe_window_dispatch_snapshot_complete = false;
    g_state.engine2_upstream_probe_window_exhausted_logged = false;
    g_state.engine2_upstream_keep_armed_logged = false;
    g_state.engine2_upstream_wait_exhausted_logged = false;
    g_state.engine2_upstream_probe_remove_requested.store(false, std::memory_order_relaxed);
    g_state.engine2_upstream_precall_snapshot_captured.store(false, std::memory_order_relaxed);
    g_state.engine2_upstream_precall_capture_state.store(0, std::memory_order_relaxed);
    g_state.engine2_upstream_precall_capture = {};
    g_state.engine2_upstream_precall_return_address = nullptr;
    g_state.animation_slot120_probe_target = nullptr;
    g_state.animation_slot120_probe_original = nullptr;
    g_state.animation_slot120_probe_stub = nullptr;
    g_state.animation_slot120_probe_pending_return = nullptr;
    g_state.animation_slot120_probe_pending_target = nullptr;
    g_state.animation_slot120_probe_pending_offset = 0;
    g_state.animation_slot120_probe_active = false;
    g_state.animation_slot120_probe_pending = false;
    g_state.animation_slot120_probe_window_attempts = 0;
    g_state.animation_slot120_probe_window_resource_found = false;
    g_state.animation_slot120_probe_window_exhausted_logged = false;
    g_state.animation_slot120_probe_entered_count.store(0, std::memory_order_relaxed);
    g_state.animation_slot120_probe_remove_requested.store(false, std::memory_order_relaxed);
    g_state.animation_slot120_caller_candidate_count = 0;
    g_state.animation_slot120_source_candidate_index = kInvalidSlot;
    for (auto& candidate : g_state.animation_slot120_caller_candidates)
    {
        candidate = {};
    }
    for (auto& sibling_hook : g_state.sibling_hooks)
    {
        sibling_hook = {};
    }

    std::wstring reason;
    if (!TryActivateNextFileSystemProbeLocked(reason))
    {
        g_state.route = Route::None;
        g_state.file_system_interface = nullptr;
        result.reason = reason == L"file_system_interface_missing"
            ? L"file_system_interface_missing"
            : L"no_vfilesystem_path_slot";
        return result;
    }

    g_state.installed = g_state.original != nullptr;
    nskinzcs2::Logger::Info(
        std::wstring(L"GlobalResourceSwapper installed via VFileSystem probe/MinHook: slot=0x") +
        [slot = g_state.slot]() {
            wchar_t text[32] {};
            swprintf_s(text, L"%zX", slot);
            return std::wstring(text);
        }() +
        L", target=" +
        FormatPointer(g_state.target) +
        L".");

    result.installed = g_state.installed;
    result.slot = g_state.slot;
    result.route = RouteText(g_state.route);
    result.reason = g_state.installed ? L"installed" : L"filesystem_probe_original_missing";
    return result;
}

void nskinzcs2::RemoveGlobalResourceSwapper() noexcept
{
    std::scoped_lock lock(g_state_mutex);

    for (auto& sibling_hook : g_state.sibling_hooks)
    {
        if (sibling_hook.active && sibling_hook.target != nullptr)
        {
            MH_DisableHook(sibling_hook.target);
            MH_RemoveHook(sibling_hook.target);
        }
    }
    if (g_state.vmt_patch.installed)
    {
        g_state.vmt_patch.Remove();
    }
    if (g_state.engine2_upstream_probe_active && g_state.engine2_upstream_probe_target != nullptr)
    {
        MH_DisableHook(g_state.engine2_upstream_probe_target);
        MH_RemoveHook(g_state.engine2_upstream_probe_target);
    }
    if (g_state.animation_slot120_probe_active && g_state.animation_slot120_probe_target != nullptr)
    {
        MH_DisableHook(g_state.animation_slot120_probe_target);
        MH_RemoveHook(g_state.animation_slot120_probe_target);
    }
    if (g_state.engine2_probe_active && g_state.engine2_probe_target != nullptr)
    {
        MH_DisableHook(g_state.engine2_probe_target);
        MH_RemoveHook(g_state.engine2_probe_target);
    }
    if (g_state.minhook_active && g_state.target != nullptr)
    {
        MH_DisableHook(g_state.target);
        MH_RemoveHook(g_state.target);
    }
    if (g_state.engine2_probe_stub != nullptr)
    {
        VirtualFree(g_state.engine2_probe_stub, 0, MEM_RELEASE);
    }
    if (g_state.engine2_upstream_probe_stub != nullptr)
    {
        VirtualFree(g_state.engine2_upstream_probe_stub, 0, MEM_RELEASE);
    }
    if (g_state.animation_slot120_probe_stub != nullptr)
    {
        VirtualFree(g_state.animation_slot120_probe_stub, 0, MEM_RELEASE);
    }

    g_state.file_system_interface = nullptr;
    g_state.signature_match = nullptr;
    g_state.target = nullptr;
    g_state.original = nullptr;
    g_state.file_system_next_slot = kInvalidSlot;
    g_state.engine2_probe_target = nullptr;
    g_state.engine2_probe_original = nullptr;
    g_state.engine2_probe_stub = nullptr;
    g_state.engine2_upstream_probe_pending_return = nullptr;
    g_state.engine2_upstream_probe_pending_target = nullptr;
    g_state.engine2_upstream_probe_pending_offset = 0;
    g_state.engine2_upstream_live_object = nullptr;
    g_state.engine2_upstream_live_vtable = nullptr;
    g_state.engine2_upstream_live_entry_address = nullptr;
    g_state.engine2_upstream_live_target = nullptr;
    g_state.engine2_upstream_live_target_base = nullptr;
    g_state.engine2_upstream_last_rejected_object = nullptr;
    g_state.engine2_upstream_last_rejected_vtable = nullptr;
    g_state.engine2_upstream_last_rejected_target = nullptr;
    g_state.engine2_upstream_last_rejected_target_base = nullptr;
    g_state.engine2_upstream_last_rejected_caller = nullptr;
    g_state.engine2_upstream_live_registers.fill(nullptr);
    g_state.engine2_upstream_probe_target = nullptr;
    g_state.engine2_upstream_probe_original = nullptr;
    g_state.engine2_upstream_probe_stub = nullptr;
    g_state.animation_slot120_probe_pending_return = nullptr;
    g_state.animation_slot120_probe_pending_target = nullptr;
    g_state.animation_slot120_probe_pending_offset = 0;
    g_state.animation_slot120_probe_target = nullptr;
    g_state.animation_slot120_probe_original = nullptr;
    g_state.animation_slot120_probe_stub = nullptr;
    g_state.slot = kInvalidSlot;
    g_state.route = Route::None;
    g_state.vmt_patch = {};
    g_state.minhook_active = false;
    g_state.engine2_probe_active = false;
    g_state.engine2_upstream_probe_active = false;
    g_state.engine2_upstream_probe_pending = false;
    g_state.animation_slot120_probe_active = false;
    g_state.animation_slot120_probe_pending = false;
    g_state.engine2_upstream_probe_analysis_done = false;
    g_state.file_system_scan_complete = false;
    g_state.file_system_flushed = false;
    g_state.file_system_carrier_probe_flushed = false;
    g_state.engine2_upstream_analysis_state = Engine2UpstreamAnalysisState::None;
    g_state.engine2_upstream_analysis_attempts = 0;
    g_state.engine2_upstream_analysis_total_direct_calls = 0;
    g_state.engine2_upstream_analysis_summary_count = 0;
    g_state.engine2_upstream_analysis_exception_code = 0;
    g_state.engine2_upstream_probe_armed_slot68_count = 0;
    g_state.engine2_upstream_probe_window_attempts = 0;
    g_state.engine2_upstream_probe_window_resource_found = false;
    g_state.engine2_upstream_probe_window_dispatch_snapshot_complete = false;
    g_state.engine2_upstream_probe_window_exhausted_logged = false;
    g_state.engine2_upstream_keep_armed_logged = false;
    g_state.engine2_upstream_wait_exhausted_logged = false;
    g_state.animation_slot120_probe_window_attempts = 0;
    g_state.animation_slot120_probe_window_resource_found = false;
    g_state.animation_slot120_probe_window_exhausted_logged = false;
    g_state.file_system_probe_armed_tick = 0;
    g_state.installed = false;
    g_state.entered_count.store(0, std::memory_order_relaxed);
    g_state.slot68_entered_count.store(0, std::memory_order_relaxed);
    g_state.success_count.store(0, std::memory_order_relaxed);
    g_state.file_system_entered_count.store(0, std::memory_order_relaxed);
    g_state.file_system_capture_count.store(0, std::memory_order_relaxed);
    g_state.file_system_capture_state.store(
        static_cast<std::uint32_t>(FileSystemCaptureState::Inactive),
        std::memory_order_relaxed);
    g_state.file_system_probe_call_count.store(0, std::memory_order_relaxed);
    g_state.file_system_pending_flush.store(false, std::memory_order_relaxed);
    g_state.file_system_carrier_probe_attempts.store(0, std::memory_order_relaxed);
    g_state.file_system_carrier_probe_done.store(false, std::memory_order_relaxed);
    g_state.file_system_carrier_probe_pending_flush.store(false, std::memory_order_relaxed);
    g_state.sibling_active_mask.store(0, std::memory_order_relaxed);
    g_state.engine2_probe_entered_count.store(0, std::memory_order_relaxed);
    g_state.engine2_upstream_probe_entered_count.store(0, std::memory_order_relaxed);
    g_state.animation_slot120_probe_entered_count.store(0, std::memory_order_relaxed);
    g_state.engine2_upstream_probe_remove_requested.store(false, std::memory_order_relaxed);
    g_state.engine2_upstream_precall_snapshot_captured.store(false, std::memory_order_relaxed);
    g_state.engine2_upstream_precall_capture_state.store(0, std::memory_order_relaxed);
    g_state.engine2_upstream_precall_capture = {};
    g_state.engine2_upstream_precall_return_address = nullptr;
    g_state.animation_slot120_probe_remove_requested.store(false, std::memory_order_relaxed);
    g_state.animation_slot120_caller_candidate_count = 0;
    g_state.animation_slot120_source_candidate_index = kInvalidSlot;
    g_state.file_system_capture = {};
    g_state.file_system_carrier_probe_capture = {};
    for (auto& candidate : g_state.animation_slot120_caller_candidates)
    {
        candidate = {};
    }
    for (auto& sibling_hook : g_state.sibling_hooks)
    {
        sibling_hook = {};
    }
}

bool nskinzcs2::GlobalResourceSwapperInstalled() noexcept
{
    std::scoped_lock lock(g_state_mutex);
    return g_state.installed;
}

std::size_t nskinzcs2::GlobalResourceSwapperCurrentSlot() noexcept
{
    std::scoped_lock lock(g_state_mutex);
    return g_state.slot;
}

void nskinzcs2::PumpGlobalResourceSwapperDeferredWork() noexcept
{
    if (g_state.route == Route::FileSystemAmbush)
    {
        TryFlushFileSystemAmbushCapture();
        TryFlushFileSystemCarrierProbe();
        TryAdvanceFileSystemProbe();
        return;
    }

    TryActivatePendingSiblingHooks();
    TryAnalyzeQueuedEngine2UpstreamCallerOnce();
    TryMarkColdEngine2UpstreamCallerProbeForRemoval();
    TryProcessEngine2UpstreamPrecallCapture();
    TryDeactivateOneShotEngine2UpstreamCallerProbe();
    TryQueueNextEngine2UpstreamCallerCandidate();
    TryActivateDeferredEngine2UpstreamCallerProbe();
    TryDeactivateAnimationSlot120CallerProbe();
    TryQueueNextAnimationSlot120CallerCandidate();
    TryActivateDeferredAnimationSlot120CallerProbe();
}

std::uint64_t nskinzcs2::GlobalResourceSwapperEnteredCount() noexcept
{
    return g_state.entered_count.load(std::memory_order_relaxed);
}

std::uint64_t nskinzcs2::GlobalResourceSwapperSlot68EnteredCount() noexcept
{
    return g_state.slot68_entered_count.load(std::memory_order_relaxed);
}

void nskinzcs2::GlobalResourceSwapperIncrementSuccess() noexcept
{
    g_state.success_count.fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t nskinzcs2::GlobalResourceSwapperSuccessCount() noexcept
{
    return g_state.success_count.load(std::memory_order_relaxed);
}

std::uint64_t nskinzcs2::GlobalResourceSwapperFileSystemEnteredCount() noexcept
{
    return g_state.file_system_entered_count.load(std::memory_order_relaxed);
}

std::uint64_t nskinzcs2::GlobalResourceSwapperFileSystemCaptureCount() noexcept
{
    return g_state.file_system_capture_count.load(std::memory_order_relaxed);
}

const wchar_t* nskinzcs2::GlobalResourceSwapperFileSystemCaptureStateText() noexcept
{
    return FileSystemCaptureStateText(
        static_cast<FileSystemCaptureState>(g_state.file_system_capture_state.load(std::memory_order_relaxed)));
}

std::uint32_t nskinzcs2::GlobalResourceSwapperSiblingActiveMask() noexcept
{
    return g_state.sibling_active_mask.load(std::memory_order_relaxed);
}

bool nskinzcs2::GlobalResourceSwapperEngine2UpstreamProbeActive() noexcept
{
    std::scoped_lock lock(g_state_mutex);
    return g_state.engine2_upstream_probe_active;
}

bool nskinzcs2::GlobalResourceSwapperEngine2UpstreamProbePending() noexcept
{
    std::scoped_lock lock(g_state_mutex);
    return g_state.engine2_upstream_probe_pending;
}

std::uint64_t nskinzcs2::GlobalResourceSwapperEngine2UpstreamProbeEnteredCount() noexcept
{
    return g_state.engine2_upstream_probe_entered_count.load(std::memory_order_relaxed);
}

const wchar_t* nskinzcs2::GlobalResourceSwapperEngine2UpstreamAnalysisStateText() noexcept
{
    std::scoped_lock lock(g_state_mutex);
    return Engine2UpstreamAnalysisStateText(g_state.engine2_upstream_analysis_state);
}

std::uint32_t nskinzcs2::GlobalResourceSwapperEngine2UpstreamAnalysisAttempts() noexcept
{
    std::scoped_lock lock(g_state_mutex);
    return g_state.engine2_upstream_analysis_attempts;
}

std::uint64_t nskinzcs2::GlobalResourceSwapperEngine2UpstreamAnalysisTotalDirectCalls() noexcept
{
    std::scoped_lock lock(g_state_mutex);
    return g_state.engine2_upstream_analysis_total_direct_calls;
}

std::uint32_t nskinzcs2::GlobalResourceSwapperEngine2UpstreamAnalysisSummaryCount() noexcept
{
    std::scoped_lock lock(g_state_mutex);
    return g_state.engine2_upstream_analysis_summary_count;
}

std::uint32_t nskinzcs2::GlobalResourceSwapperEngine2UpstreamAnalysisExceptionCode() noexcept
{
    std::scoped_lock lock(g_state_mutex);
    return g_state.engine2_upstream_analysis_exception_code;
}
