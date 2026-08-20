#pragma once

#include "Cs2/Compatibility.hpp"

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
        compatibility::kResourceSystemQueryPattern,
        L"resourcesystem.dll",
    },
    {
        "CModelService::BindModel (offline reference)",
        compatibility::kBindModelPattern,
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
