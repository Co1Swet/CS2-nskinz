#include "UI/MainWindow.hpp"

#include "Core/Config.hpp"
#include "Core/Log.hpp"
#include "UI/UiText.hpp"

#include <d3d11.h>
#include <dxgi.h>

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cfloat>
#include <cctype>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "d3d11.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace
{
using nskinzcs2::AppState;
using nskinzcs2::ConfigStore;
using nskinzcs2::Logger;
using nskinzcs2::model::Catalog;
using nskinzcs2::model::ConfigDocument;
using nskinzcs2::model::CosmeticProfile;
using nskinzcs2::model::GloveProfile;
using nskinzcs2::model::GloveProfileMode;
using nskinzcs2::model::KeychainProfile;
using nskinzcs2::model::KnifeProfile;
using nskinzcs2::model::KnifeProfileMode;
using nskinzcs2::model::NamedId;
using nskinzcs2::ui::UiTextId;

constexpr wchar_t kWindowClassName[] = L"nSkinzCS2.ToolWindow";
constexpr wchar_t kWindowTitle[] = L"nSkinzCS2";
// Local regression automation can request the same runtime rebuild as Update
// without stealing foreground input from another full-screen application.
// This does not mutate the staged editor document or change the public UI.
constexpr UINT kAutomatedRuntimeRefreshMessage = WM_APP + 0x153;

const char* Ui(const UiTextId id) noexcept
{
    return nskinzcs2::ui::Text(id);
}

const char* YesNoText(const bool value) noexcept
{
    return Ui(value ? UiTextId::Yes : UiTextId::No);
}

LONG LogStructuredThreadException(const wchar_t* thread_name, EXCEPTION_POINTERS* exception_info)
{
    std::wstringstream stream;
    stream << thread_name << L" raised SEH 0x";
    if (exception_info != nullptr && exception_info->ExceptionRecord != nullptr)
    {
        stream << std::hex << std::uppercase << exception_info->ExceptionRecord->ExceptionCode;
        if (exception_info->ExceptionRecord->ExceptionAddress != nullptr)
        {
            stream << L" at " << exception_info->ExceptionRecord->ExceptionAddress;

            HMODULE module = nullptr;
            if (GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(exception_info->ExceptionRecord->ExceptionAddress),
                    &module) &&
                module != nullptr)
            {
                std::wstring buffer(MAX_PATH, L'\0');
                const auto length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
                buffer.resize(length);
                if (!buffer.empty())
                {
                    stream << L" in " << buffer;
                }
            }
        }
    }
    else
    {
        stream << L"UNKNOWN";
    }

    Logger::Error(stream.str());
    return EXCEPTION_EXECUTE_HANDLER;
}

std::filesystem::path ResolveConfigPath()
{
    static int anchor = 0;
    HMODULE module = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&anchor),
        &module);

    std::wstring buffer(MAX_PATH, L'\0');
    const auto length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path() / L"nSkinzCS2.json";
}

bool IsKnifeBaseItem(const int item_id)
{
    return item_id == 42;
}

bool IsGloveBaseItem(const int item_id)
{
    return item_id == 5028;
}

bool IsCatalogKnifeItem(const Catalog& catalog, const int item_id)
{
    const auto visual_it = catalog.visuals.find(item_id);
    return visual_it != catalog.visuals.end() && visual_it->second.is_knife;
}

bool IsSelectableKnifeModelDefinition(const Catalog& catalog, const int item_id)
{
    constexpr int kDefaultCtKnifeDefinition = 42;
    constexpr int kDefaultTKnifeDefinition = 59;
    const bool listed_in_knife_items =
        std::any_of(
            catalog.knife_items.begin(),
            catalog.knife_items.end(),
            [item_id](const NamedId& named_id)
            {
                return named_id.id == item_id;
            });
    return item_id != 0 &&
        item_id != kDefaultCtKnifeDefinition &&
        item_id != kDefaultTKnifeDefinition &&
        listed_in_knife_items &&
        IsCatalogKnifeItem(catalog, item_id);
}

bool IsCatalogGloveItem(const Catalog& catalog, const int item_id)
{
    const auto visual_it = catalog.visuals.find(item_id);
    return visual_it != catalog.visuals.end() && visual_it->second.is_glove;
}

bool ContainsNamedId(const std::vector<NamedId>& values, const int id)
{
    return std::any_of(
        values.begin(),
        values.end(),
        [id](const NamedId& value)
        {
            return value.id == id;
        });
}

std::vector<NamedId> BuildVisibleWeaponItemList(const Catalog& catalog)
{
    std::vector<NamedId> filtered;
    filtered.reserve(catalog.base_items.size());
    for (const auto& item : catalog.base_items)
    {
        if (!IsCatalogGloveItem(catalog, item.id) && !IsCatalogKnifeItem(catalog, item.id) && !IsKnifeBaseItem(item.id))
        {
            filtered.push_back(item);
        }
    }

    return filtered;
}

bool HasLegacyGloveProfiles(const ConfigDocument& document)
{
    return std::any_of(
        document.profiles.begin(),
        document.profiles.end(),
        [](const CosmeticProfile& profile)
        {
            return IsGloveBaseItem(profile.base_item);
        });
}

bool HasEnabledNonKeychainProfiles(const ConfigDocument& document)
{
    const auto has_enabled_weapon_profile =
        std::any_of(
            document.profiles.begin(),
            document.profiles.end(),
            [](const CosmeticProfile& profile)
            {
                return profile.enabled && profile.xuid != 0;
            });
    if (has_enabled_weapon_profile)
    {
        return true;
    }

    const auto has_enabled_knife_profile =
        std::any_of(
            document.knife_profiles.begin(),
            document.knife_profiles.end(),
            [](const KnifeProfile& profile)
            {
                return profile.enabled && profile.xuid != 0;
            });
    if (has_enabled_knife_profile)
    {
        return true;
    }

    return std::any_of(
        document.glove_profiles.begin(),
        document.glove_profiles.end(),
        [](const GloveProfile& profile)
        {
            return profile.enabled && profile.xuid != 0;
        });
}

bool HasEnabledKeychainProfiles(const ConfigDocument& document)
{
    return std::any_of(
        document.keychain_profiles.begin(),
        document.keychain_profiles.end(),
        [](const KeychainProfile& profile)
        {
            return profile.enabled && profile.xuid != 0 && profile.keychain_kit > 0;
        });
}

bool ShouldUseLightweightDemoKeychainRefresh(
    const nskinzcs2::runtime::Status& status,
    const ConfigDocument& document)
{
    return status.hook_installed &&
        status.frame_callbacks_active &&
        HasEnabledKeychainProfiles(document) &&
        !HasEnabledNonKeychainProfiles(document);
}

int CompatibleGloveItemForPaint(const Catalog& catalog, const int paint_kit)
{
    if (paint_kit == 0)
    {
        return 0;
    }

    const auto it = catalog.glove_paint_compatibility.find(paint_kit);
    return it == catalog.glove_paint_compatibility.end() ? 0 : it->second;
}

bool IsCompatibleGlovePaint(const Catalog& catalog, const int glove_item_id, const int paint_kit)
{
    if (paint_kit == 0 || glove_item_id == 0)
    {
        return true;
    }

    const auto compatible_glove_item = CompatibleGloveItemForPaint(catalog, paint_kit);
    return compatible_glove_item != 0 && compatible_glove_item == glove_item_id;
}

std::vector<NamedId> BuildVisibleGlovePaintList(const Catalog& catalog, const int glove_item_id)
{
    if (glove_item_id == 0)
    {
        return catalog.glove_paint_kits;
    }

    std::vector<NamedId> filtered;
    filtered.reserve(catalog.glove_paint_kits.size());
    for (const auto& paint : catalog.glove_paint_kits)
    {
        if (paint.id == 0 || IsCompatibleGlovePaint(catalog, glove_item_id, paint.id))
        {
            filtered.push_back(paint);
        }
    }

    return filtered;
}

const char* GloveModeText(const GloveProfileMode mode)
{
    switch (mode)
    {
    case GloveProfileMode::FinishOnly:
        return "finish_only";
    case GloveProfileMode::ModelAndFinish:
        return "model_and_finish";
    default:
        return "finish_only";
    }
}

const char* GloveModeLabel(const GloveProfileMode mode)
{
    switch (mode)
    {
    case GloveProfileMode::FinishOnly:
        return Ui(UiTextId::FinishOnly);
    case GloveProfileMode::ModelAndFinish:
        return Ui(UiTextId::ModelAndFinish);
    default:
        return Ui(UiTextId::FinishOnly);
    }
}

const char* KnifeModeText(const KnifeProfileMode mode)
{
    switch (mode)
    {
    case KnifeProfileMode::FinishOnly:
        return "finish_only";
    case KnifeProfileMode::ModelAndFinish:
        return "model_and_finish";
    default:
        return "finish_only";
    }
}

const char* KnifeModeLabel(const KnifeProfileMode mode)
{
    switch (mode)
    {
    case KnifeProfileMode::FinishOnly:
        return Ui(UiTextId::FinishOnly);
    case KnifeProfileMode::ModelAndFinish:
        return Ui(UiTextId::ModelAndFinish);
    default:
        return Ui(UiTextId::FinishOnly);
    }
}

bool DrawKnifeModeCombo(KnifeProfileMode& mode)
{
    static constexpr std::array<KnifeProfileMode, 2> kModes {
        KnifeProfileMode::FinishOnly,
        KnifeProfileMode::ModelAndFinish,
    };

    int current_index = 0;
    for (int index = 0; index < static_cast<int>(kModes.size()); ++index)
    {
        if (kModes[static_cast<std::size_t>(index)] == mode)
        {
            current_index = index;
            break;
        }
    }

    bool changed = false;
    if (ImGui::BeginCombo(Ui(UiTextId::Mode), KnifeModeLabel(mode)))
    {
        for (int index = 0; index < static_cast<int>(kModes.size()); ++index)
        {
            const auto selected = current_index == index;
            if (ImGui::Selectable(KnifeModeLabel(kModes[static_cast<std::size_t>(index)]), selected))
            {
                mode = kModes[static_cast<std::size_t>(index)];
                changed = true;
            }

            if (selected)
            {
                ImGui::SetItemDefaultFocus();
            }
        }

        ImGui::EndCombo();
    }

    return changed;
}

    bool DrawGloveModeCombo(GloveProfileMode& mode)
    {
        static constexpr std::array<GloveProfileMode, 2> kModes {
            GloveProfileMode::FinishOnly,
            GloveProfileMode::ModelAndFinish,
    };

    int current_index = 0;
    for (int index = 0; index < static_cast<int>(kModes.size()); ++index)
    {
        if (kModes[static_cast<std::size_t>(index)] == mode)
        {
            current_index = index;
            break;
        }
    }

    bool changed = false;
    if (ImGui::BeginCombo(Ui(UiTextId::Mode), GloveModeLabel(mode)))
    {
        for (int index = 0; index < static_cast<int>(kModes.size()); ++index)
        {
            const auto selected = current_index == index;
            if (ImGui::Selectable(
                    GloveModeLabel(kModes[static_cast<std::size_t>(index)]),
                    selected))
            {
                mode = kModes[static_cast<std::size_t>(index)];
                changed = true;
            }

            if (selected)
            {
                ImGui::SetItemDefaultFocus();
            }
        }

        ImGui::EndCombo();
    }

        return changed;
    }

    bool TryValidateKnifeProfilesForApply(
        const ConfigDocument& document,
        const Catalog& catalog,
        std::wstring& issue)
    {
        issue.clear();
        for (const auto& profile : document.knife_profiles)
        {
            if (!profile.enabled)
            {
                continue;
            }

            if (profile.xuid == 0)
            {
                issue = L"Knife profile '" + std::wstring(profile.name.begin(), profile.name.end()) +
                    L"' is invalid: enabled knife slots require a non-zero XUID.";
                return false;
            }

            if (profile.mode == KnifeProfileMode::ModelAndFinish && profile.model_definition == 0)
            {
                issue = L"Knife profile '" + std::wstring(profile.name.begin(), profile.name.end()) +
                    L"' is invalid: model_and_finish requires a knife model definition.";
                return false;
            }

            if (profile.mode == KnifeProfileMode::ModelAndFinish &&
                !IsSelectableKnifeModelDefinition(catalog, profile.model_definition))
            {
                issue = L"Knife profile '" + std::wstring(profile.name.begin(), profile.name.end()) +
                    L"' is invalid: model_and_finish requires a supported non-default knife model definition.";
                return false;
            }
        }

        return true;
    }

    bool TryValidateGloveProfilesForApply(
        const ConfigDocument& document,
        const Catalog& catalog,
        std::wstring& issue)
    {
        issue.clear();
        for (const auto& profile : document.glove_profiles)
        {
            if (!profile.enabled)
            {
                continue;
            }

            if (profile.mode == GloveProfileMode::ModelAndFinish && profile.model_definition == 0)
            {
                issue = L"Glove profile '" + std::wstring(profile.name.begin(), profile.name.end()) +
                    L"' is invalid: model_and_finish requires a glove model definition.";
                return false;
            }

            if (profile.paint_kit != 0)
            {
                int effective_definition =
                    profile.mode == GloveProfileMode::ModelAndFinish
                        ? profile.model_definition
                        : 0;
                if (profile.mode != GloveProfileMode::ModelAndFinish)
                {
                    const auto compatibility_it = catalog.glove_paint_compatibility.find(profile.paint_kit);
                    effective_definition =
                        compatibility_it == catalog.glove_paint_compatibility.end()
                            ? 0
                            : compatibility_it->second;
                }
                if (effective_definition == 0 ||
                    !IsCompatibleGlovePaint(catalog, effective_definition, profile.paint_kit))
                {
                    issue = L"Glove profile '" + std::wstring(profile.name.begin(), profile.name.end()) +
                        L"' is invalid: the selected paint kit does not match the glove definition.";
                    return false;
                }
            }
        }

        return true;
    }

    bool TryValidateKeychainProfilesForApply(
        const ConfigDocument& document,
        const Catalog& catalog,
        std::wstring& issue)
    {
        issue.clear();
        for (const auto& profile : document.keychain_profiles)
        {
            if (!profile.enabled)
            {
                continue;
            }

            if (profile.xuid == 0)
            {
                issue = L"Keychain profile '" + std::wstring(profile.name.begin(), profile.name.end()) +
                    L"' is invalid: enabled keychain slots require a non-zero XUID.";
                return false;
            }

            if (!ContainsNamedId(catalog.base_items, profile.base_item) ||
                IsKnifeBaseItem(profile.base_item) ||
                IsGloveBaseItem(profile.base_item) ||
                IsCatalogKnifeItem(catalog, profile.base_item) ||
                IsCatalogGloveItem(catalog, profile.base_item))
            {
                issue = L"Keychain profile '" + std::wstring(profile.name.begin(), profile.name.end()) +
                    L"' is invalid: keychains only support standard weapon definitions.";
                return false;
            }

            if (profile.keychain_kit <= 0)
            {
                issue = L"Keychain profile '" + std::wstring(profile.name.begin(), profile.name.end()) +
                    L"' is invalid: modify-only demo keychain slots require a non-zero keychain kit.";
                return false;
            }

            if (!ContainsNamedId(catalog.keychain_kits, profile.keychain_kit))
            {
                issue = L"Keychain profile '" + std::wstring(profile.name.begin(), profile.name.end()) +
                    L"' is invalid: selected keychain kit was not found in the catalog.";
                return false;
            }
        }

        return true;
    }

const char* AvailabilityText(const nskinzcs2::runtime::Availability availability)
{
    switch (availability)
    {
    case nskinzcs2::runtime::Availability::Unknown:
        return Ui(UiTextId::AvailabilityUnknown);
    case nskinzcs2::runtime::Availability::MissingProcess:
        return Ui(UiTextId::AvailabilityMissingProcess);
    case nskinzcs2::runtime::Availability::MissingHlae:
        return Ui(UiTextId::AvailabilityMissingHlae);
    case nskinzcs2::runtime::Availability::MissingInterfaces:
        return Ui(UiTextId::AvailabilityMissingInterfaces);
    case nskinzcs2::runtime::Availability::MissingHook:
        return Ui(UiTextId::AvailabilityRuntimeIdle);
    case nskinzcs2::runtime::Availability::HookedPendingDemoGuard:
        return Ui(UiTextId::AvailabilityHookedPending);
    case nskinzcs2::runtime::Availability::Ready:
        return Ui(UiTextId::AvailabilityReady);
    default:
        return Ui(UiTextId::AvailabilityUnknown);
    }
}

const char* DemoKeychainStateText(const nskinzcs2::runtime::DemoKeychainState state)
{
    switch (state)
    {
    case nskinzcs2::runtime::DemoKeychainState::Idle:
        return Ui(UiTextId::DemoKeychainIdle);
    case nskinzcs2::runtime::DemoKeychainState::WaitingCooldown:
        return Ui(UiTextId::DemoKeychainWaitingCooldown);
    case nskinzcs2::runtime::DemoKeychainState::WaitingStableFrames:
        return Ui(UiTextId::DemoKeychainWaitingStableFrames);
    case nskinzcs2::runtime::DemoKeychainState::WaitingFactory:
        return Ui(UiTextId::DemoKeychainWaitingFactory);
    case nskinzcs2::runtime::DemoKeychainState::Applied:
        return Ui(UiTextId::DemoKeychainApplied);
    case nskinzcs2::runtime::DemoKeychainState::ClearPending:
        return Ui(UiTextId::DemoKeychainClearPending);
    case nskinzcs2::runtime::DemoKeychainState::Failed:
        return Ui(UiTextId::DemoKeychainFailed);
    default:
        return Ui(UiTextId::DemoKeychainIdle);
    }
}

int FindIndexById(const std::vector<NamedId>& values, const int id)
{
    for (std::size_t index = 0; index < values.size(); ++index)
    {
        if (values[index].id == id)
        {
            return static_cast<int>(index);
        }
    }

    return 0;
}

std::string Lowercase(std::string_view value)
{
    std::string result(value);
    std::transform(
        result.begin(),
        result.end(),
        result.begin(),
        [](unsigned char ch)
        {
            return static_cast<char>(std::tolower(ch));
        });
    return result;
}

bool ContainsCaseInsensitive(std::string_view haystack, std::string_view needle)
{
    if (needle.empty())
    {
        return true;
    }

    const auto lower_haystack = Lowercase(haystack);
    const auto lower_needle = Lowercase(needle);
    return lower_haystack.find(lower_needle) != std::string::npos;
}

bool ComboById(const char* label, const std::vector<NamedId>& values, int& id, const int popup_count = 10)
{
    if (values.empty())
    {
        return false;
    }

    int current_index = FindIndexById(values, id);
    bool changed = false;
    static std::unordered_map<ImGuiID, std::string> filters;
    const ImGuiID combo_id = ImGui::GetID(label);

    if (popup_count > 0)
    {
        ImGui::SetNextWindowSizeConstraints(
            ImVec2(0.0f, 0.0f),
            ImVec2(FLT_MAX, ImGui::GetTextLineHeightWithSpacing() * static_cast<float>(popup_count + 2)));
    }

    if (ImGui::BeginCombo(label, values[current_index].name.c_str()))
    {
        auto& filter = filters[combo_id];
        char filter_buffer[128] {};
        const auto copy_size = std::min<std::size_t>(filter.size(), sizeof(filter_buffer) - 1);
        std::copy_n(filter.data(), copy_size, filter_buffer);

        ImGui::PushID(static_cast<int>(combo_id));
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputTextWithHint("##search", Ui(UiTextId::SearchHint), filter_buffer, sizeof(filter_buffer)))
        {
            filter = filter_buffer;
        }
        ImGui::PopID();

        ImGui::Separator();

        const int count = static_cast<int>(values.size());
        std::vector<int> visible_indices;
        visible_indices.reserve(values.size());
        for (int index = 0; index < count; ++index)
        {
            if (ContainsCaseInsensitive(values[index].name, filter))
            {
                visible_indices.push_back(index);
            }
        }

        if (visible_indices.empty())
        {
            ImGui::TextDisabled("%s", Ui(UiTextId::NoMatches));
        }
        else
        {
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(visible_indices.size()));
            while (clipper.Step())
            {
                for (int visible = clipper.DisplayStart; visible < clipper.DisplayEnd; ++visible)
                {
                    const int index = visible_indices[visible];
                    const bool selected = index == current_index;
                    ImGui::PushID(values[index].id);
                    if (ImGui::Selectable(values[index].name.c_str(), selected))
                    {
                        current_index = index;
                        id = values[index].id;
                        changed = true;
                        filter.clear();
                    }

                    if (selected)
                    {
                        ImGui::SetItemDefaultFocus();
                    }

                    ImGui::PopID();
                }
            }
        }
        ImGui::EndCombo();
    }

    return changed;
}

std::optional<std::filesystem::path> ResolveChineseUiFontPath()
{
    static constexpr std::array<const wchar_t*, 5> kCandidates {
        L"C:\\Windows\\Fonts\\msyh.ttc",
        L"C:\\Windows\\Fonts\\msyhl.ttc",
        L"C:\\Windows\\Fonts\\msyhbd.ttc",
        L"C:\\Windows\\Fonts\\simhei.ttf",
        L"C:\\Windows\\Fonts\\simsun.ttc",
    };

    for (const auto* candidate : kCandidates)
    {
        const std::filesystem::path path(candidate);
        if (std::filesystem::exists(path))
        {
            return path;
        }
    }

    return std::nullopt;
}

std::string Narrow(const std::wstring& value)
{
    if (value.empty())
    {
        return {};
    }

    const auto count = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string output(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), output.data(), count, nullptr, nullptr);
    return output;
}

class WindowController
{
public:
    WindowController(AppState& state, std::atomic_bool& stop_requested)
        : state_(state)
        , stop_requested_(stop_requested)
        , config_path_(ResolveConfigPath())
    {
        state_.SetWindowVisible(false);
        working_config_ = state_.SnapshotConfig();
        if (working_config_.profiles.empty())
        {
            working_config_ = ConfigStore::CreateDefault();
            state_.ReplaceConfig(working_config_);
        }

        working_config_dirty_ = false;
    }

    ~WindowController()
    {
        Cleanup();
    }

    int Run()
    {
        Logger::Info(L"UI thread started in lazy-init mode. Press Insert to create the tool window.");

        MSG message {};
        while (!stop_requested_.load() && !state_.ShutdownRequested())
        {
            while (PeekMessageW(&message, nullptr, 0U, 0U, PM_REMOVE))
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
                if (message.message == WM_QUIT)
                {
                    stop_requested_.store(true);
                    break;
                }
            }

            HandleInsertToggle();

            if (resources_created_ && state_.WindowVisible())
            {
                DrawFrame();
            }
            else
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(resources_created_ ? 16 : 50));
            }
        }

        Logger::Info(L"UI thread stopped.");
        return EXIT_SUCCESS;
    }

private:
    bool CreateDeviceIndependentResources()
    {
        if (resources_created_)
        {
            return true;
        }

        WNDCLASSEXW window_class {};
        window_class.cbSize = sizeof(window_class);
        window_class.style = CS_CLASSDC;
        window_class.lpfnWndProc = &WindowController::StaticWndProc;
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.lpszClassName = kWindowClassName;
        const auto register_result = RegisterClassExW(&window_class);
        if (register_result == 0)
        {
            const auto last_error = GetLastError();
            if (last_error != ERROR_CLASS_ALREADY_EXISTS)
            {
                Logger::Error(L"Failed to register nSkinzCS2 UI window class.");
                return false;
            }
        }
        window_class_registered_ = true;

        hwnd_ = CreateWindowW(
            kWindowClassName,
            kWindowTitle,
            WS_OVERLAPPEDWINDOW,
            100,
            100,
            1100,
            760,
            nullptr,
            nullptr,
            window_class.hInstance,
            this);

        if (!hwnd_)
        {
            Logger::Error(L"Failed to create nSkinzCS2 UI window.");
            return false;
        }

        if (!CreateDeviceD3D())
        {
            Logger::Error(L"Failed to create D3D11 device for UI window.");
            Cleanup();
            return false;
        }

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ConfigureFonts();
        ImGui::StyleColorsDark();
        ApplyStyle();

        if (!ImGui_ImplWin32_Init(hwnd_))
        {
            Logger::Error(L"ImGui Win32 backend initialization failed.");
            Cleanup();
            return false;
        }
        if (!ImGui_ImplDX11_Init(device_, device_context_))
        {
            Logger::Error(L"ImGui D3D11 backend initialization failed.");
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            Cleanup();
            return false;
        }
        imgui_initialized_ = true;

        ShowWindow(hwnd_, SW_HIDE);
        UpdateWindow(hwnd_);
        state_.SetWindowVisible(false);
        resources_created_ = true;
        return true;
    }

    void ConfigureFonts()
    {
        auto& io = ImGui::GetIO();
        io.Fonts->AddFontDefault();

        if (!nskinzcs2::build::kUiLanguageChinese)
        {
            return;
        }

        const auto font_path = ResolveChineseUiFontPath();
        if (!font_path.has_value())
        {
            Logger::Warn(L"Chinese UI font was not found on this system; UI will fall back to the default ImGui font.");
            return;
        }

        ImFontConfig config {};
        config.OversampleH = 2;
        config.OversampleV = 2;
        config.PixelSnapH = false;

        const auto utf8_path = font_path->string();
        if (ImFont* font = io.Fonts->AddFontFromFileTTF(
                utf8_path.c_str(),
                18.0f,
                &config,
                io.Fonts->GetGlyphRangesChineseFull());
            font != nullptr)
        {
            io.FontDefault = font;
            Logger::Info(L"Chinese UI font loaded from " + font_path->wstring());
        }
        else
        {
            Logger::Warn(L"Chinese UI font load failed for " + font_path->wstring() +
                L"; UI will fall back to the default ImGui font.");
        }
    }

    void Cleanup()
    {
        if (device_context_)
        {
            device_context_->ClearState();
        }

        if (imgui_initialized_)
        {
            ImGui_ImplDX11_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            imgui_initialized_ = false;
        }

        CleanupRenderTarget();

        if (swap_chain_)
        {
            swap_chain_->Release();
            swap_chain_ = nullptr;
        }

        if (device_context_)
        {
            device_context_->Release();
            device_context_ = nullptr;
        }

        if (device_)
        {
            device_->Release();
            device_ = nullptr;
        }

        if (hwnd_)
        {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }

        resources_created_ = false;

        if (window_class_registered_)
        {
            UnregisterClassW(kWindowClassName, GetModuleHandleW(nullptr));
            window_class_registered_ = false;
        }
    }

    bool CreateDeviceD3D()
    {
        DXGI_SWAP_CHAIN_DESC desc {};
        desc.BufferCount = 2;
        desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.OutputWindow = hwnd_;
        desc.SampleDesc.Count = 1;
        desc.Windowed = TRUE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        constexpr D3D_FEATURE_LEVEL feature_levels[] {
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_0,
        };

        D3D_FEATURE_LEVEL created_level {};
        auto result = D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            0,
            feature_levels,
            static_cast<UINT>(std::size(feature_levels)),
            D3D11_SDK_VERSION,
            &desc,
            &swap_chain_,
            &device_,
            &created_level,
            &device_context_);

        if (SUCCEEDED(result))
        {
            Logger::Info(L"UI D3D11 device created using WARP software rasterizer for stability.");
        }
        else
        {
            result = D3D11CreateDeviceAndSwapChain(
                nullptr,
                D3D_DRIVER_TYPE_HARDWARE,
                nullptr,
                0,
                feature_levels,
                static_cast<UINT>(std::size(feature_levels)),
                D3D11_SDK_VERSION,
                &desc,
                &swap_chain_,
                &device_,
                &created_level,
                &device_context_);

            if (SUCCEEDED(result))
            {
                Logger::Warn(L"UI WARP device creation failed; falling back to hardware D3D11 for the tool window.");
            }
        }

        if (FAILED(result))
        {
            return false;
        }

        CreateRenderTarget();
        return true;
    }

    void CreateRenderTarget()
    {
        ID3D11Texture2D* back_buffer = nullptr;
        if (SUCCEEDED(swap_chain_->GetBuffer(0, IID_PPV_ARGS(&back_buffer))))
        {
            device_->CreateRenderTargetView(back_buffer, nullptr, &render_target_view_);
            back_buffer->Release();
        }
    }

    void CleanupRenderTarget()
    {
        if (render_target_view_)
        {
            render_target_view_->Release();
            render_target_view_ = nullptr;
        }
    }

    void ResetDevice()
    {
        CleanupRenderTarget();
        swap_chain_->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
        CreateRenderTarget();
    }

    void HandleInsertToggle()
    {
        const bool down = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
        if (down && !last_insert_state_)
        {
            if (!resources_created_)
            {
                if (!CreateDeviceIndependentResources())
                {
                    state_.SetWindowVisible(false);
                    Logger::Error(L"UI initialization failed after Insert. The tool window stayed disabled to keep CS2 stable.");
                    last_insert_state_ = down;
                    return;
                }

                Logger::Info(L"UI resources initialized on demand after Insert.");
            }

            const bool new_visibility = !state_.WindowVisible();
            state_.SetWindowVisible(new_visibility);
            if (hwnd_ != nullptr)
            {
                ShowWindow(hwnd_, new_visibility ? SW_SHOW : SW_HIDE);
                if (new_visibility)
                {
                    SetForegroundWindow(hwnd_);
                }
            }
        }

        last_insert_state_ = down;
    }

    void ApplyStyle()
    {
        auto& style = ImGui::GetStyle();
        style.WindowRounding = 0.0f;
        style.ChildRounding = 0.0f;
        style.FrameRounding = 0.0f;
        style.GrabRounding = 0.0f;
        style.ScrollbarRounding = 0.0f;
        style.WindowPadding = ImVec2(10.0f, 10.0f);
        style.FramePadding = ImVec2(6.0f, 4.0f);

        style.Colors[ImGuiCol_WindowBg] = ImVec4(0.16f, 0.16f, 0.16f, 1.0f);
        style.Colors[ImGuiCol_TitleBg] = ImVec4(0.23f, 0.23f, 0.23f, 1.0f);
        style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.23f, 0.23f, 0.23f, 1.0f);
        style.Colors[ImGuiCol_Button] = ImVec4(0.28f, 0.28f, 0.28f, 1.0f);
        style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.36f, 0.36f, 0.36f, 1.0f);
        style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.17f, 0.17f, 0.17f, 1.0f);
    }

    void DrawFrame()
    {
        if (swap_chain_ == nullptr || device_context_ == nullptr || render_target_view_ == nullptr)
        {
            return;
        }

        if (!first_frame_started_)
        {
            first_frame_started_ = true;
            Logger::Info(L"UI first frame begin.");
        }

        ImGui_ImplDX11_NewFrame();
        if (!first_dx11_frame_completed_)
        {
            first_dx11_frame_completed_ = true;
            Logger::Info(L"UI first D3D11 backend frame completed.");
        }
        ImGui_ImplWin32_NewFrame();
        if (!first_win32_frame_completed_)
        {
            first_win32_frame_completed_ = true;
            Logger::Info(L"UI first Win32 backend frame completed.");
        }
        ImGui::NewFrame();
        if (!first_imgui_frame_started_)
        {
            first_imgui_frame_started_ = true;
            Logger::Info(L"UI first ImGui frame started.");
        }

        DrawMainWindow();

        ImGui::Render();
        constexpr float clear_color[4] {0.08f, 0.08f, 0.08f, 1.0f};
        device_context_->OMSetRenderTargets(1, &render_target_view_, nullptr);
        device_context_->ClearRenderTargetView(render_target_view_, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        swap_chain_->Present(1, 0);
        if (!first_frame_presented_)
        {
            first_frame_presented_ = true;
            Logger::Info(L"UI first frame presented successfully.");
        }
    }

    void DrawMainWindow()
    {
        auto status = state_.SnapshotStatus();
        auto catalog = state_.SnapshotCatalog();

        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize, ImGuiCond_Always);

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoTitleBar;

        if (!ImGui::Begin("nSkinzCS2Root", nullptr, flags))
        {
            ImGui::End();
            return;
        }

        ImGui::TextUnformatted(Ui(UiTextId::AppTitle));
        ImGui::SameLine();
        ImGui::TextDisabled("%s", Ui(UiTextId::AppSubtitle));
        ImGui::Separator();

        EnsureSelection();
        if (HasLegacyGloveProfiles(working_config_))
        {
            ImGui::TextColored(
                ImVec4(0.94f, 0.42f, 0.25f, 1.0f),
                "%s",
                Ui(UiTextId::LegacyGloveWarningTitle));
            ImGui::TextWrapped(
                "%s",
                Ui(UiTextId::LegacyGloveWarningBody));
            ImGui::Separator();
        }

        if (ImGui::BeginTabBar("EquipmentSlots"))
        {
            if (ImGui::BeginTabItem(Ui(UiTextId::TabWeapons)))
            {
                ImGui::Columns(2, nullptr, false);
                DrawProfileList(catalog);
                ImGui::NextColumn();
                DrawProfileEditor(catalog);
                ImGui::Columns(1);

                if (!working_config_.profiles.empty())
                {
                    ImGui::Separator();
                    DrawStickersEditor(catalog, CurrentProfile().stickers, "weapon_stickers", selected_weapon_sticker_);
                }
                else
                {
                    ImGui::Separator();
                    ImGui::TextDisabled("%s", Ui(UiTextId::StickersUnavailable));
                }

                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem(Ui(UiTextId::TabKnives)))
            {
                ImGui::Columns(2, nullptr, false);
                DrawKnifeProfileList();
                ImGui::NextColumn();
                DrawKnifeProfileEditor(catalog, status);
                ImGui::Columns(1);
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem(Ui(UiTextId::TabGloves)))
            {
                ImGui::Columns(2, nullptr, false);
                DrawGloveProfileList();
                ImGui::NextColumn();
                DrawGloveProfileEditor(catalog, status);
                ImGui::Columns(1);
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem(Ui(UiTextId::TabKeychains)))
            {
                ImGui::Columns(2, nullptr, false);
                DrawKeychainProfileList(catalog);
                ImGui::NextColumn();
                DrawKeychainProfileEditor(catalog);
                ImGui::Columns(1);
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

        ImGui::Separator();
        DrawActions();

        ImGui::Separator();
        ImGui::Text(Ui(UiTextId::RuntimeFormat), AvailabilityText(status.availability));
        ImGui::TextWrapped(Ui(UiTextId::DetailFormat), Narrow(status.detail).c_str());
        ImGui::Text(Ui(UiTextId::FrameCallbacksFormat), static_cast<unsigned long long>(status.frame_callback_count));
        if (status.last_frame_stage >= 0)
        {
            ImGui::Text(Ui(UiTextId::LastStageFormat), status.last_frame_stage);
        }
        ImGui::Text(
            Ui(UiTextId::DemoFormat),
            status.demo_playing
                ? (status.demo_paused ? Ui(UiTextId::DemoPlayingPaused) : Ui(UiTextId::DemoPlaying))
                : Ui(UiTextId::DemoWaiting));
        if (!status.level_name.empty())
        {
            ImGui::Text(Ui(UiTextId::LevelFormat), Narrow(status.level_name).c_str());
        }
        if (status.demo_tick >= 0)
        {
            ImGui::Text(Ui(UiTextId::DemoTickFormat), status.demo_tick);
        }
        ImGui::Text(
            Ui(UiTextId::SchemaFormat),
            status.schema_bound ? Ui(UiTextId::StateBound) : Ui(UiTextId::StatePending));
        ImGui::Text(
            Ui(UiTextId::EntitySystemFormat),
            status.entity_system_bound ? Ui(UiTextId::StateBound) : Ui(UiTextId::StatePending));
        ImGui::Text(
            Ui(UiTextId::EntityFactoryFormat),
            !status.client_entity_factory_requested
                ? Ui(UiTextId::StateIdle)
                : (status.client_entity_factory_resolved ? Ui(UiTextId::StateResolved) : Ui(UiTextId::StateUnavailable)));
        if (status.highest_entity_index >= 0)
        {
            ImGui::Text(Ui(UiTextId::HighestEntityFormat), status.highest_entity_index);
        }
        if (status.watched_xuid != 0)
        {
            ImGui::Text(Ui(UiTextId::WatchedXuidFormat), static_cast<unsigned long long>(status.watched_xuid));
        }
        ImGui::Text(Ui(UiTextId::ProfilesEnabledFormat), status.enabled_profile_count);
        if (status.stable_wearable_bound ||
            status.runtime_created_wearable_bound ||
            status.glove_pawn_fallback_bound ||
            status.knife_viewmodel_sync_ready)
        {
            ImGui::Text(
                Ui(UiTextId::BindingsFormat),
                YesNoText(status.stable_wearable_bound),
                YesNoText(status.runtime_created_wearable_bound),
                YesNoText(status.glove_pawn_fallback_bound),
                YesNoText(status.knife_viewmodel_sync_ready));
        }
        if (!status.knife_runtime_path.empty())
        {
            ImGui::TextWrapped(Ui(UiTextId::KnifeRuntimePathFormat), Narrow(status.knife_runtime_path).c_str());
        }
        if (!status.knife_runtime_reason.empty())
        {
            ImGui::TextWrapped(Ui(UiTextId::KnifeRuntimeReasonFormat), Narrow(status.knife_runtime_reason).c_str());
        }
        if (status.player_controller_count > 0 || status.player_pawn_count > 0 || status.preview_character_count > 0)
        {
            ImGui::Text(
                Ui(UiTextId::PlayersFormat),
                status.player_controller_count,
                status.player_pawn_count,
                status.preview_character_count,
                status.active_weapon_count,
                status.owned_weapon_count,
                status.matched_profile_count);
            ImGui::Text(
                Ui(UiTextId::AppliedFormat),
                status.applied_weapon_count,
                status.applied_glove_count);
        }
        if (!status.last_error.empty())
        {
            ImGui::TextWrapped(Ui(UiTextId::LastErrorFormat), Narrow(status.last_error).c_str());
        }

        if (!status.diagnostics.empty())
        {
            ImGui::Spacing();
            ImGui::TextUnformatted(Ui(UiTextId::RuntimeDiagnostics));
            for (const auto& line : status.diagnostics)
            {
                ImGui::BulletText("%s", Narrow(line).c_str());
            }
        }

        if (!catalog.warnings.empty())
        {
            ImGui::Spacing();
            ImGui::TextUnformatted(Ui(UiTextId::CatalogWarnings));
            for (const auto& warning : catalog.warnings)
            {
                ImGui::BulletText("%s", warning.c_str());
            }
        }

        ImGui::End();
    }

    void DrawProfileList(const Catalog& catalog)
    {
        auto& profiles = working_config_.profiles;

        ImGui::BeginChild("Profiles", ImVec2(0.0f, 260.0f), true);
        for (int index = 0; index < static_cast<int>(profiles.size()); ++index)
        {
            if (IsKnifeBaseItem(profiles[index].base_item) || IsGloveBaseItem(profiles[index].base_item))
            {
                continue;
            }

            std::string label = profiles[index].name;
            label += " (";
            label += LookupName(catalog.base_items, profiles[index].base_item);
            label += ")";

            ImGui::PushID(index);
            if (ImGui::Selectable(label.c_str(), selected_profile_ == index))
            {
                selected_profile_ = index;
            }
            ImGui::PopID();
        }
        ImGui::EndChild();

        const auto button_width = ImGui::GetContentRegionAvail().x;
        if (ImGui::Button(Ui(UiTextId::Add), ImVec2(button_width * 0.48f, 28.0f)))
        {
            profiles.push_back(CosmeticProfile {});
            selected_profile_ = static_cast<int>(profiles.size()) - 1;
            PushWorkingConfig();
        }
        ImGui::SameLine();
        if (ImGui::Button(Ui(UiTextId::Remove), ImVec2(button_width * 0.48f, 28.0f)) && profiles.size() > 1)
        {
            profiles.erase(profiles.begin() + selected_profile_);
            selected_profile_ = std::clamp(selected_profile_, 0, static_cast<int>(profiles.size()) - 1);
            PushWorkingConfig();
        }
    }

    void DrawProfileEditor(const Catalog& catalog)
    {
        auto& profile = CurrentProfile();

        char name_buffer[64] {};
        CopyToBuffer(profile.name, name_buffer, sizeof(name_buffer));
        if (ImGui::InputText(Ui(UiTextId::Name), name_buffer, sizeof(name_buffer)))
        {
            profile.name = name_buffer;
            PushWorkingConfig();
        }

        const auto visible_items = BuildVisibleWeaponItemList(catalog);
        if (ComboById(Ui(UiTextId::Item), visible_items, profile.base_item, 8))
        {
            profile.override_item = 0;
            PushWorkingConfig();
        }

        if (ImGui::Checkbox(Ui(UiTextId::Enabled), &profile.enabled))
        {
            PushWorkingConfig();
        }

        char xuid_buffer[32] {};
        _ui64toa_s(profile.xuid, xuid_buffer, sizeof(xuid_buffer), 10);
        if (ImGui::InputText(Ui(UiTextId::Xuid), xuid_buffer, sizeof(xuid_buffer), ImGuiInputTextFlags_CharsDecimal))
        {
            profile.xuid = std::strtoull(xuid_buffer, nullptr, 10);
            PushWorkingConfig();
        }

        if (ImGui::InputInt(Ui(UiTextId::Seed), &profile.seed))
        {
            PushWorkingConfig();
        }

        if (ImGui::InputInt(Ui(UiTextId::StatTrak), &profile.stat_trak))
        {
            PushWorkingConfig();
        }

        if (ImGui::SliderFloat(Ui(UiTextId::Wear), &profile.wear, 0.0000001f, 1.0f, "%.7f"))
        {
            PushWorkingConfig();
        }

        if (ComboById(Ui(UiTextId::PaintKit), catalog.weapon_paint_kits, profile.paint_kit, 12))
        {
            PushWorkingConfig();
        }

        if (ComboById(Ui(UiTextId::Quality), catalog.qualities, profile.quality, 8))
        {
            PushWorkingConfig();
        }

        char custom_name_buffer[64] {};
        CopyToBuffer(profile.custom_name, custom_name_buffer, sizeof(custom_name_buffer));
        if (ImGui::InputText(Ui(UiTextId::NameTag), custom_name_buffer, sizeof(custom_name_buffer)))
        {
            profile.custom_name = custom_name_buffer;
            PushWorkingConfig();
        }
    }

    void DrawKnifeProfileList()
    {
        auto& knife_profiles = working_config_.knife_profiles;

        ImGui::BeginChild("KnifeProfiles", ImVec2(0.0f, 260.0f), true);
        for (int index = 0; index < static_cast<int>(knife_profiles.size()); ++index)
        {
            ImGui::PushID(index + 5000);
            if (ImGui::Selectable(knife_profiles[index].name.c_str(), selected_knife_profile_ == index))
            {
                selected_knife_profile_ = index;
            }
            ImGui::PopID();
        }
        ImGui::EndChild();

        const auto button_width = ImGui::GetContentRegionAvail().x;
        if (ImGui::Button(Ui(UiTextId::AddKnife), ImVec2(button_width * 0.48f, 28.0f)))
        {
            knife_profiles.push_back(KnifeProfile {});
            selected_knife_profile_ = static_cast<int>(knife_profiles.size()) - 1;
            PushWorkingConfig();
        }
        ImGui::SameLine();
        if (ImGui::Button(Ui(UiTextId::RemoveKnife), ImVec2(button_width * 0.48f, 28.0f)) && knife_profiles.size() > 1)
        {
            knife_profiles.erase(knife_profiles.begin() + selected_knife_profile_);
            selected_knife_profile_ = std::clamp(selected_knife_profile_, 0, static_cast<int>(knife_profiles.size()) - 1);
            PushWorkingConfig();
        }
    }

    void DrawKnifeProfileEditor(const Catalog& catalog, const nskinzcs2::runtime::Status& status)
    {
        auto& profile = CurrentKnifeProfile();
        std::vector<NamedId> knife_model_items;
        knife_model_items.reserve(catalog.knife_items.size() + 1);
        knife_model_items.push_back(NamedId {0, Ui(UiTextId::SelectKnifeModel)});
        for (const auto& knife_item : catalog.knife_items)
        {
            if (IsSelectableKnifeModelDefinition(catalog, knife_item.id))
            {
                knife_model_items.push_back(knife_item);
            }
        }
        const bool missing_model_definition =
            profile.mode == KnifeProfileMode::ModelAndFinish && profile.model_definition == 0;
        const bool model_and_finish_requested = profile.mode == KnifeProfileMode::ModelAndFinish;
        const bool runtime_finish_only_active =
            status.knife_runtime_path == L"Knife finish-only runtime path active";
        const bool runtime_model_swap_active =
            status.knife_runtime_path == L"Knife model swap active";
        const bool runtime_model_swap_degraded =
            status.knife_runtime_path == L"Knife model swap degraded";
        const bool runtime_degraded = runtime_model_swap_degraded && !status.knife_runtime_reason.empty();

        char name_buffer[64] {};
        CopyToBuffer(profile.name, name_buffer, sizeof(name_buffer));
        if (ImGui::InputText(Ui(UiTextId::Name), name_buffer, sizeof(name_buffer)))
        {
            profile.name = name_buffer;
            PushWorkingConfig();
        }

        if (ImGui::Checkbox(Ui(UiTextId::Enabled), &profile.enabled))
        {
            PushWorkingConfig();
        }

        char xuid_buffer[32] {};
        _ui64toa_s(profile.xuid, xuid_buffer, sizeof(xuid_buffer), 10);
        if (ImGui::InputText(Ui(UiTextId::Xuid), xuid_buffer, sizeof(xuid_buffer), ImGuiInputTextFlags_CharsDecimal))
        {
            profile.xuid = std::strtoull(xuid_buffer, nullptr, 10);
            PushWorkingConfig();
        }
        if (DrawKnifeModeCombo(profile.mode))
        {
            PushWorkingConfig();
        }

        if (ComboById(Ui(UiTextId::KnifeModel), knife_model_items, profile.model_definition, 10))
        {
            PushWorkingConfig();
        }

        if (ComboById(Ui(UiTextId::PaintKit), catalog.weapon_paint_kits, profile.paint_kit, 12))
        {
            PushWorkingConfig();
        }

        if (ComboById(Ui(UiTextId::Quality), catalog.qualities, profile.quality, 8))
        {
            PushWorkingConfig();
        }

        if (ImGui::InputInt(Ui(UiTextId::Seed), &profile.seed))
        {
            PushWorkingConfig();
        }

        if (ImGui::InputInt(Ui(UiTextId::StatTrak), &profile.stat_trak))
        {
            PushWorkingConfig();
        }

        if (ImGui::SliderFloat(Ui(UiTextId::Wear), &profile.wear, 0.0000001f, 1.0f, "%.7f"))
        {
            PushWorkingConfig();
        }

        char custom_name_buffer[64] {};
        CopyToBuffer(profile.custom_name, custom_name_buffer, sizeof(custom_name_buffer));
        if (ImGui::InputText(Ui(UiTextId::NameTag), custom_name_buffer, sizeof(custom_name_buffer)))
        {
            profile.custom_name = custom_name_buffer;
            PushWorkingConfig();
        }

        if (missing_model_definition)
        {
            ImGui::TextWrapped("%s", Ui(UiTextId::KnifeModelRequired));
        }
        else if (runtime_model_swap_active)
        {
            ImGui::TextColored(
                ImVec4(0.42f, 0.82f, 0.58f, 1.0f),
                "%s",
                Ui(UiTextId::KnifeModelSwapActive));
            if (!status.knife_runtime_reason.empty())
            {
                ImGui::TextWrapped("%s", Narrow(status.knife_runtime_reason).c_str());
            }
        }
        else if (runtime_degraded)
        {
            ImGui::TextColored(
                ImVec4(0.94f, 0.68f, 0.25f, 1.0f),
                "%s",
                Ui(UiTextId::KnifeModelSwapDegraded));
            ImGui::TextWrapped("%s", Narrow(status.knife_runtime_reason).c_str());
        }
        else if (runtime_finish_only_active || profile.mode == KnifeProfileMode::FinishOnly)
        {
            ImGui::TextColored(
                ImVec4(0.42f, 0.82f, 0.58f, 1.0f),
                "%s",
                Ui(UiTextId::KnifeFinishOnlyRuntime));
            ImGui::TextWrapped(
                model_and_finish_requested
                    ? Ui(UiTextId::KnifeFinishOnlyFallback)
                    : Ui(UiTextId::KnifeFinishOnlyDescription));
        }
        else if (!status.knife_runtime_path.empty())
        {
            ImGui::TextDisabled("%s", Narrow(status.knife_runtime_path).c_str());
        }
    }

    void DrawGloveProfileList()
    {
        auto& glove_profiles = working_config_.glove_profiles;

        ImGui::BeginChild("GloveProfiles", ImVec2(0.0f, 260.0f), true);
        for (int index = 0; index < static_cast<int>(glove_profiles.size()); ++index)
        {
            ImGui::PushID(index + 10000);
            if (ImGui::Selectable(glove_profiles[index].name.c_str(), selected_glove_profile_ == index))
            {
                selected_glove_profile_ = index;
            }
            ImGui::PopID();
        }
        ImGui::EndChild();

        const auto button_width = ImGui::GetContentRegionAvail().x;
        if (ImGui::Button(Ui(UiTextId::AddGlove), ImVec2(button_width * 0.48f, 28.0f)))
        {
            glove_profiles.push_back(GloveProfile {});
            selected_glove_profile_ = static_cast<int>(glove_profiles.size()) - 1;
            PushWorkingConfig();
        }
        ImGui::SameLine();
        if (ImGui::Button(Ui(UiTextId::RemoveGlove), ImVec2(button_width * 0.48f, 28.0f)) && glove_profiles.size() > 1)
        {
            glove_profiles.erase(glove_profiles.begin() + selected_glove_profile_);
            selected_glove_profile_ = std::clamp(selected_glove_profile_, 0, static_cast<int>(glove_profiles.size()) - 1);
            PushWorkingConfig();
        }
    }

    void DrawGloveProfileEditor(const Catalog& catalog, const nskinzcs2::runtime::Status& status)
    {
        auto& profile = CurrentGloveProfile();

        char name_buffer[64] {};
        CopyToBuffer(profile.name, name_buffer, sizeof(name_buffer));
        if (ImGui::InputText(Ui(UiTextId::Name), name_buffer, sizeof(name_buffer)))
        {
            profile.name = name_buffer;
            PushWorkingConfig();
        }

        if (ImGui::Checkbox(Ui(UiTextId::Enabled), &profile.enabled))
        {
            PushWorkingConfig();
        }

        char xuid_buffer[32] {};
        _ui64toa_s(profile.xuid, xuid_buffer, sizeof(xuid_buffer), 10);
        if (ImGui::InputText(Ui(UiTextId::Xuid), xuid_buffer, sizeof(xuid_buffer), ImGuiInputTextFlags_CharsDecimal))
        {
            profile.xuid = std::strtoull(xuid_buffer, nullptr, 10);
            PushWorkingConfig();
        }

        if (DrawGloveModeCombo(profile.mode))
        {
            if (profile.mode == GloveProfileMode::FinishOnly)
            {
                profile.model_definition = 0;
            }
            else if (profile.paint_kit != 0)
            {
                const auto compatible_glove_item = CompatibleGloveItemForPaint(catalog, profile.paint_kit);
                if (compatible_glove_item != 0)
                {
                    profile.model_definition = compatible_glove_item;
                }
                else if (profile.model_definition != 0 &&
                         !IsCompatibleGlovePaint(catalog, profile.model_definition, profile.paint_kit))
                {
                    profile.paint_kit = 0;
                }
            }
            PushWorkingConfig();
        }

        if (profile.mode == GloveProfileMode::ModelAndFinish)
        {
            if (ComboById(Ui(UiTextId::GloveModel), catalog.glove_items, profile.model_definition, 10))
            {
                if (profile.model_definition != 0 &&
                    !IsCompatibleGlovePaint(catalog, profile.model_definition, profile.paint_kit))
                {
                    profile.paint_kit = 0;
                }
                PushWorkingConfig();
            }
        }
        else
        {
            ImGui::TextDisabled("%s", Ui(UiTextId::GloveModel));
            ImGui::SameLine();
            ImGui::TextUnformatted(Ui(UiTextId::CurrentLiveWearableModel));
        }

        const auto glove_item_id =
            profile.mode == GloveProfileMode::ModelAndFinish
                ? profile.model_definition
                : 0;
        const auto glove_paint_list = BuildVisibleGlovePaintList(catalog, glove_item_id);
        if (ComboById(Ui(UiTextId::PaintKit), glove_paint_list, profile.paint_kit, 12))
        {
            if (profile.mode == GloveProfileMode::ModelAndFinish && profile.paint_kit != 0)
            {
                const auto compatible_glove_item = CompatibleGloveItemForPaint(catalog, profile.paint_kit);
                if (compatible_glove_item != 0)
                {
                    profile.model_definition = compatible_glove_item;
                }
            }
            PushWorkingConfig();
        }

        if (ComboById(Ui(UiTextId::Quality), catalog.qualities, profile.quality, 8))
        {
            PushWorkingConfig();
        }

        if (ImGui::InputInt(Ui(UiTextId::Seed), &profile.seed))
        {
            PushWorkingConfig();
        }

        if (ImGui::SliderFloat(Ui(UiTextId::Wear), &profile.wear, 0.0000001f, 1.0f, "%.7f"))
        {
            PushWorkingConfig();
        }

        if (profile.mode == GloveProfileMode::FinishOnly)
        {
            ImGui::TextColored(
                ImVec4(0.42f, 0.82f, 0.58f, 1.0f),
                "%s",
                Ui(UiTextId::GloveFinishOnlyTitle));
            ImGui::TextWrapped(
                "%s",
                Ui(UiTextId::GloveFinishOnlyBody));
        }
        else if (profile.model_definition == 0)
        {
            ImGui::TextColored(
                ImVec4(0.94f, 0.42f, 0.25f, 1.0f),
                "%s",
                Ui(UiTextId::GloveModelRequiredTitle));
            ImGui::TextWrapped(
                "%s",
                Ui(UiTextId::GloveModelRequiredBody));
        }
        else if (profile.paint_kit != 0 && !IsCompatibleGlovePaint(catalog, profile.model_definition, profile.paint_kit))
        {
            ImGui::TextColored(
                ImVec4(0.94f, 0.42f, 0.25f, 1.0f),
                "%s",
                Ui(UiTextId::GlovePaintMismatch));
        }

        if (status.glove_pawn_fallback_bound)
        {
            ImGui::TextColored(ImVec4(0.42f, 0.82f, 0.58f, 1.0f), "%s", Ui(UiTextId::GloveCarrierPawnFallback));
        }
        else if (status.stable_wearable_bound)
        {
            ImGui::TextColored(ImVec4(0.42f, 0.82f, 0.58f, 1.0f), "%s", Ui(UiTextId::GloveCarrierLiveWearable));
        }
        else if (status.runtime_created_wearable_bound)
        {
            ImGui::TextColored(ImVec4(0.42f, 0.82f, 0.58f, 1.0f), "%s", Ui(UiTextId::GloveCarrierRuntimeWearable));
        }
        else if (profile.mode == GloveProfileMode::ModelAndFinish && !status.client_entity_factory_resolved)
        {
            ImGui::TextColored(ImVec4(0.94f, 0.42f, 0.25f, 1.0f), "%s", Ui(UiTextId::GloveFactoryUnavailable));
        }
        else
        {
            ImGui::TextColored(ImVec4(0.93f, 0.77f, 0.32f, 1.0f), "%s", Ui(UiTextId::GloveWaitingForBind));
        }
    }

    void DrawKeychainProfileList(const Catalog& catalog)
    {
        auto& keychain_profiles = working_config_.keychain_profiles;

        ImGui::BeginChild("KeychainProfiles", ImVec2(0.0f, 260.0f), true);
        for (int index = 0; index < static_cast<int>(keychain_profiles.size()); ++index)
        {
            std::string label = keychain_profiles[index].name;
            label += " (";
            label += LookupName(catalog.base_items, keychain_profiles[index].base_item);
            label += ")";

            ImGui::PushID(index + 15000);
            if (ImGui::Selectable(label.c_str(), selected_keychain_profile_ == index))
            {
                selected_keychain_profile_ = index;
            }
            ImGui::PopID();
        }
        ImGui::EndChild();

        const auto button_width = ImGui::GetContentRegionAvail().x;
        if (ImGui::Button(Ui(UiTextId::AddKeychain), ImVec2(button_width * 0.48f, 28.0f)))
        {
            keychain_profiles.push_back(KeychainProfile {});
            selected_keychain_profile_ = static_cast<int>(keychain_profiles.size()) - 1;
            PushWorkingConfig();
        }
        ImGui::SameLine();
        if (ImGui::Button(Ui(UiTextId::RemoveKeychain), ImVec2(button_width * 0.48f, 28.0f)) &&
            keychain_profiles.size() > 1)
        {
            keychain_profiles.erase(keychain_profiles.begin() + selected_keychain_profile_);
            selected_keychain_profile_ =
                std::clamp(selected_keychain_profile_, 0, static_cast<int>(keychain_profiles.size()) - 1);
            PushWorkingConfig();
        }
    }

    void DrawKeychainProfileEditor(const Catalog& catalog)
    {
        auto& profile = CurrentKeychainProfile();

        char name_buffer[64] {};
        CopyToBuffer(profile.name, name_buffer, sizeof(name_buffer));
        if (ImGui::InputText(Ui(UiTextId::Name), name_buffer, sizeof(name_buffer)))
        {
            profile.name = name_buffer;
            PushWorkingConfig();
        }

        if (ImGui::Checkbox(Ui(UiTextId::Enabled), &profile.enabled))
        {
            PushWorkingConfig();
        }

        char xuid_buffer[32] {};
        _ui64toa_s(profile.xuid, xuid_buffer, sizeof(xuid_buffer), 10);
        if (ImGui::InputText(Ui(UiTextId::Xuid), xuid_buffer, sizeof(xuid_buffer), ImGuiInputTextFlags_CharsDecimal))
        {
            profile.xuid = std::strtoull(xuid_buffer, nullptr, 10);
            PushWorkingConfig();
        }

        const auto visible_items = BuildVisibleWeaponItemList(catalog);
        if (ComboById(Ui(UiTextId::Weapon), visible_items, profile.base_item, 8))
        {
            PushWorkingConfig();
        }

        auto keychain_items = catalog.keychain_kits;
        if (keychain_items.empty())
        {
            keychain_items.push_back(NamedId {0, Ui(UiTextId::None)});
        }
        if (ComboById(Ui(UiTextId::Keychain), keychain_items, profile.keychain_kit, 12))
        {
            PushWorkingConfig();
        }

        if (profile.keychain_kit == 0)
        {
            ImGui::TextColored(
                ImVec4(0.94f, 0.42f, 0.25f, 1.0f),
                "%s",
                Ui(UiTextId::KeychainClearDescription));
        }

        ImGui::Spacing();
        ImGui::TextWrapped("%s", Ui(UiTextId::KeychainRuntimeDescription));
    }

    void DrawStickersEditor(
        const Catalog& catalog,
        std::array<nskinzcs2::model::StickerProfile, 5>& stickers,
        const char* scope_id,
        int& selected_sticker)
    {
        ImGui::Columns(2, nullptr, false);
        ImGui::PushID(scope_id);
        ImGui::BeginChild("StickerList", ImVec2(0.0f, 180.0f), true);
        for (int index = 0; index < static_cast<int>(stickers.size()); ++index)
        {
            std::string label = "#";
            label += std::to_string(index + 1);
            label += " (";
            label += LookupName(catalog.sticker_kits, stickers[static_cast<std::size_t>(index)].kit_id);
            label += ")";

            ImGui::PushID(index);
            if (ImGui::Selectable(label.c_str(), selected_sticker == index))
            {
                selected_sticker = index;
            }
            ImGui::PopID();
        }
        ImGui::EndChild();

        ImGui::NextColumn();
        selected_sticker = std::clamp(selected_sticker, 0, 4);
        auto& sticker = stickers[static_cast<std::size_t>(selected_sticker)];
        if (ComboById(Ui(UiTextId::StickerKit), catalog.sticker_kits, sticker.kit_id, 10))
        {
            PushWorkingConfig();
        }

        if (ImGui::SliderFloat(Ui(UiTextId::StickerWear), &sticker.wear, 0.0000001f, 1.0f, "%.7f"))
        {
            PushWorkingConfig();
        }

        if (ImGui::SliderFloat(Ui(UiTextId::Scale), &sticker.scale, 0.1f, 5.0f, "%.3f"))
        {
            PushWorkingConfig();
        }

        if (ImGui::SliderFloat(Ui(UiTextId::Rotation), &sticker.rotation, 0.0f, 360.0f, "%.1f"))
        {
            PushWorkingConfig();
        }

        ImGui::Columns(1);
        ImGui::PopID();
    }

    void DrawActions()
    {
        const float width = ImGui::GetContentRegionAvail().x / 3.0f;
        const auto catalog = state_.SnapshotCatalog();
        const auto runtime_status = state_.SnapshotStatus();
        if (ImGui::Button(Ui(UiTextId::Update), ImVec2(width - 4.0f, 30.0f)))
        {
            std::wstring validation_issue;
            if (!TryValidateKnifeProfilesForApply(working_config_, catalog, validation_issue) ||
                !TryValidateGloveProfilesForApply(working_config_, catalog, validation_issue) ||
                !TryValidateKeychainProfilesForApply(working_config_, catalog, validation_issue))
            {
                Logger::Warn(L"UI rejected staged config update: " + validation_issue);
            }
            else
            {
                state_.ReplaceConfig(working_config_);
                if (ShouldUseLightweightDemoKeychainRefresh(runtime_status, working_config_))
                {
                    state_.RequestDemoKeychainRefresh();
                }
                else
                {
                    state_.RequestManualRefresh();
                }
                working_config_dirty_ = false;
                Logger::Info(L"UI applied staged config via Update.");
            }
        }

        ImGui::SameLine();
        if (ImGui::Button(Ui(UiTextId::Save), ImVec2(width - 4.0f, 30.0f)))
        {
            std::wstring validation_issue;
            if (!TryValidateKnifeProfilesForApply(working_config_, catalog, validation_issue) ||
                !TryValidateGloveProfilesForApply(working_config_, catalog, validation_issue) ||
                !TryValidateKeychainProfilesForApply(working_config_, catalog, validation_issue))
            {
                Logger::Warn(L"UI rejected config save: " + validation_issue);
            }
            else
            {
                ConfigStore::Save(config_path_, working_config_);
                Logger::Info(L"UI saved the staged config to nSkinzCS2.json.");
            }
        }

        ImGui::SameLine();
        if (ImGui::Button(Ui(UiTextId::Load), ImVec2(width - 4.0f, 30.0f)))
        {
            working_config_ = ConfigStore::Load(config_path_);
            EnsureSelection();
            working_config_dirty_ = true;
            Logger::Info(L"UI loaded config into the editor only. Press Update to apply it to the runtime.");
        }

        if (working_config_dirty_)
        {
            ImGui::TextColored(
                ImVec4(0.94f, 0.68f, 0.25f, 1.0f),
                "%s",
                Ui(UiTextId::EditorStagedOnly));
        }
        else
        {
            ImGui::TextDisabled("%s", Ui(UiTextId::EditorMatchesRuntime));
        }
    }

    void EnsureSelection()
    {
        if (working_config_.profiles.empty())
        {
            working_config_ = ConfigStore::CreateDefault();
        }

        if (working_config_.glove_profiles.empty())
        {
            working_config_.glove_profiles.push_back(GloveProfile {});
        }

        if (working_config_.knife_profiles.empty())
        {
            working_config_.knife_profiles.push_back(KnifeProfile {});
        }

        if (working_config_.keychain_profiles.empty())
        {
            working_config_.keychain_profiles.push_back(KeychainProfile {});
        }

        const auto has_standard_weapon_profile =
            std::any_of(
                working_config_.profiles.begin(),
                working_config_.profiles.end(),
                [](const CosmeticProfile& profile)
                {
                    return !IsKnifeBaseItem(profile.base_item) && !IsGloveBaseItem(profile.base_item);
                });
        if (!has_standard_weapon_profile)
        {
            working_config_.profiles.push_back(CosmeticProfile {});
        }

        selected_profile_ = std::clamp(selected_profile_, 0, static_cast<int>(working_config_.profiles.size()) - 1);
        if (!working_config_.profiles.empty() &&
            (IsKnifeBaseItem(working_config_.profiles[static_cast<std::size_t>(selected_profile_)].base_item) ||
             IsGloveBaseItem(working_config_.profiles[static_cast<std::size_t>(selected_profile_)].base_item)))
        {
            for (int index = 0; index < static_cast<int>(working_config_.profiles.size()); ++index)
            {
                if (!IsKnifeBaseItem(working_config_.profiles[static_cast<std::size_t>(index)].base_item) &&
                    !IsGloveBaseItem(working_config_.profiles[static_cast<std::size_t>(index)].base_item))
                {
                    selected_profile_ = index;
                    break;
                }
            }
        }

        selected_knife_profile_ =
            std::clamp(selected_knife_profile_, 0, static_cast<int>(working_config_.knife_profiles.size()) - 1);
        selected_glove_profile_ =
            std::clamp(selected_glove_profile_, 0, static_cast<int>(working_config_.glove_profiles.size()) - 1);
        selected_keychain_profile_ =
            std::clamp(selected_keychain_profile_, 0, static_cast<int>(working_config_.keychain_profiles.size()) - 1);
        selected_weapon_sticker_ = std::clamp(selected_weapon_sticker_, 0, 4);
    }

    CosmeticProfile& CurrentProfile()
    {
        EnsureSelection();
        return working_config_.profiles[static_cast<std::size_t>(selected_profile_)];
    }

    GloveProfile& CurrentGloveProfile()
    {
        EnsureSelection();
        return working_config_.glove_profiles[static_cast<std::size_t>(selected_glove_profile_)];
    }

    KnifeProfile& CurrentKnifeProfile()
    {
        EnsureSelection();
        return working_config_.knife_profiles[static_cast<std::size_t>(selected_knife_profile_)];
    }

    KeychainProfile& CurrentKeychainProfile()
    {
        EnsureSelection();
        return working_config_.keychain_profiles[static_cast<std::size_t>(selected_keychain_profile_)];
    }

    void PushWorkingConfig()
    {
        working_config_dirty_ = true;
    }

    static void CopyToBuffer(const std::string& value, char* buffer, const std::size_t buffer_size)
    {
        if (buffer_size == 0)
        {
            return;
        }

        std::fill(buffer, buffer + buffer_size, '\0');
        const auto copy_size = std::min<std::size_t>(buffer_size - 1, value.size());
        std::copy_n(value.data(), copy_size, buffer);
    }

    std::string LookupName(const std::vector<NamedId>& values, const int id) const
    {
        const auto iterator = std::find_if(
            values.begin(),
            values.end(),
            [id](const NamedId& value)
            {
                return value.id == id;
            });

        return iterator == values.end() ? std::string(Ui(UiTextId::LookupUnknown)) : iterator->name;
    }

    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        if (ImGui_ImplWin32_WndProcHandler(hwnd, message, wparam, lparam))
        {
            return TRUE;
        }

        if (message == WM_NCCREATE)
        {
            const auto create_struct = reinterpret_cast<CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create_struct->lpCreateParams));
        }

        if (auto* self = reinterpret_cast<WindowController*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)))
        {
            return self->WndProc(hwnd, message, wparam, lparam);
        }

        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    LRESULT WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        switch (message)
        {
        case kAutomatedRuntimeRefreshMessage:
            state_.RequestManualRefresh();
            Logger::Info(L"Runtime Update requested by local regression automation.");
            return 0;
        case WM_SIZE:
            if (device_ != nullptr && swap_chain_ != nullptr && wparam != SIZE_MINIMIZED)
            {
                CleanupRenderTarget();
                swap_chain_->ResizeBuffers(0, static_cast<UINT>(LOWORD(lparam)), static_cast<UINT>(HIWORD(lparam)), DXGI_FORMAT_UNKNOWN, 0);
                CreateRenderTarget();
            }
            return 0;
        case WM_SYSCOMMAND:
            if ((wparam & 0xfff0) == SC_KEYMENU)
            {
                return 0;
            }
            break;
        case WM_CLOSE:
            state_.SetWindowVisible(false);
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
        }

        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    AppState& state_;
    std::atomic_bool& stop_requested_;
    std::filesystem::path config_path_;
    ConfigDocument working_config_;
    bool working_config_dirty_ = false;
    int selected_profile_ = 0;
    int selected_knife_profile_ = 0;
    int selected_glove_profile_ = 0;
    int selected_keychain_profile_ = 0;
    int selected_weapon_sticker_ = 0;
    bool last_insert_state_ = false;
    bool imgui_initialized_ = false;
    bool resources_created_ = false;
    bool window_class_registered_ = false;
    bool first_frame_started_ = false;
    bool first_dx11_frame_completed_ = false;
    bool first_win32_frame_completed_ = false;
    bool first_imgui_frame_started_ = false;
    bool first_frame_presented_ = false;

    HWND hwnd_ = nullptr;
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* device_context_ = nullptr;
    IDXGISwapChain* swap_chain_ = nullptr;
    ID3D11RenderTargetView* render_target_view_ = nullptr;
};
}

nskinzcs2::MainWindow::MainWindow(AppState& state)
    : state_(state)
{
}

nskinzcs2::MainWindow::~MainWindow()
{
    Stop();
}

void nskinzcs2::MainWindow::Start()
{
    if (thread_.joinable())
    {
        return;
    }

    stop_requested_.store(false);
    thread_ = std::thread(&MainWindow::ThreadMain, this);
}

void nskinzcs2::MainWindow::Stop()
{
    stop_requested_.store(true);
    state_.RequestShutdown();
    if (thread_.joinable())
    {
        thread_.join();
    }
}

void nskinzcs2::MainWindow::ThreadMain()
{
    __try
    {
        ThreadMainImpl();
    }
    __except (LogStructuredThreadException(L"UI thread", GetExceptionInformation()))
    {
        stop_requested_.store(true);
        state_.SetWindowVisible(false);
    }
}

void nskinzcs2::MainWindow::ThreadMainImpl()
{
    try
    {
        WindowController controller(state_, stop_requested_);
        controller.Run();
    }
    catch (const std::exception& exception)
    {
        (void)exception;
        Logger::Error(L"UI thread raised a C++ exception and was disabled.");
        stop_requested_.store(true);
        state_.SetWindowVisible(false);
    }
    catch (...)
    {
        Logger::Error(L"UI thread raised an unknown C++ exception and was disabled.");
        stop_requested_.store(true);
        state_.SetWindowVisible(false);
    }
}
