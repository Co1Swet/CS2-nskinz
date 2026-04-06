#pragma once

#include <cstddef>
#include <cstdint>

namespace nskinzcs2
{
struct GlobalResourceSwapperSignatureHint
{
    const char* symbol = nullptr;
    const char* ida_pattern = nullptr;
    const wchar_t* module_name = nullptr;
};

struct GlobalResourceSwapperInstallResult
{
    bool installed = false;
    bool already_installed = false;
    std::size_t slot = static_cast<std::size_t>(-1);
    const wchar_t* route = L"none";
    const wchar_t* reason = L"not_attempted";
};

inline constexpr GlobalResourceSwapperSignatureHint kGlobalResourceSwapperSignatureHints[] = {
    {
        "CResourceSystem::Query (offline reference)",
        "48 83 EC 38 41 B1 01 C6 44 24 20 01 E8 ? ? ? ? 48 83 C4 38 C3",
        L"resourcesystem.dll",
    },
    {
        "CModelService::BindModel (offline reference)",
        "48 8B C4 53 57 48 83 EC 68 48 89 70 18 41 0F B6 D8 4C 89 70 20 48 8B F9 4C 89 78 E8 4C 8B F2 48 8B 41 30 45 8B F9 48 85 C0 74 33",
        L"client.dll",
    },
    {
        "CGameSceneNode::SetMesh (offline reference)",
        "40 53 48 83 EC 20 48 8B DA 48 8B 49 ? E8 ? ? ? ? 48 8B D3 48 83 C4 20 5B E9 ? ? ? ?",
        L"client.dll",
    },
};

GlobalResourceSwapperInstallResult EnsureGlobalResourceSwapperInstalled(
    void* resource_handle_utils_interface,
    void* resource_system_interface,
    void* file_system_interface) noexcept;
void PumpGlobalResourceSwapperDeferredWork() noexcept;
void RemoveGlobalResourceSwapper() noexcept;
bool GlobalResourceSwapperInstalled() noexcept;
std::size_t GlobalResourceSwapperCurrentSlot() noexcept;
std::uint64_t GlobalResourceSwapperEnteredCount() noexcept;
std::uint64_t GlobalResourceSwapperSlot68EnteredCount() noexcept;
void GlobalResourceSwapperIncrementSuccess() noexcept;
std::uint64_t GlobalResourceSwapperSuccessCount() noexcept;
std::uint64_t GlobalResourceSwapperFileSystemEnteredCount() noexcept;
std::uint64_t GlobalResourceSwapperFileSystemCaptureCount() noexcept;
const wchar_t* GlobalResourceSwapperFileSystemCaptureStateText() noexcept;
std::uint32_t GlobalResourceSwapperSiblingActiveMask() noexcept;
bool GlobalResourceSwapperEngine2UpstreamProbeActive() noexcept;
bool GlobalResourceSwapperEngine2UpstreamProbePending() noexcept;
std::uint64_t GlobalResourceSwapperEngine2UpstreamProbeEnteredCount() noexcept;
const wchar_t* GlobalResourceSwapperEngine2UpstreamAnalysisStateText() noexcept;
std::uint32_t GlobalResourceSwapperEngine2UpstreamAnalysisAttempts() noexcept;
std::uint64_t GlobalResourceSwapperEngine2UpstreamAnalysisTotalDirectCalls() noexcept;
std::uint32_t GlobalResourceSwapperEngine2UpstreamAnalysisSummaryCount() noexcept;
std::uint32_t GlobalResourceSwapperEngine2UpstreamAnalysisExceptionCode() noexcept;
}
