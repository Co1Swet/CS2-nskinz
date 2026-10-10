#include "UI/MainWindow.hpp"

#include "Core/BuildConfig.hpp"
#include "Core/Config.hpp"
#include "Core/Log.hpp"
#include "Cs2/Catalog.hpp"
#include "Runtime/DemoSkinRewrite.hpp"
#include "Runtime/WorkshopMaps.hpp"
#include "UI/ExternalUiHost.hpp"
#include "UI/StickerPreviewPlacement.generated.hpp"
#include "UI/UiText.hpp"
#include "UI/WeaponPreviewGeometry.generated.hpp"

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
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <future>
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
using nskinzcs2::runtime::WorkshopMapEntry;
using nskinzcs2::runtime::WorkshopMapScanResult;
using nskinzcs2::runtime::IsLikelyAuxiliaryWorkshopMap;
using nskinzcs2::model::kMaximumCosmeticWear;
using nskinzcs2::model::kMinimumCosmeticWear;
using nskinzcs2::model::NormalizeCosmeticSeed;
using nskinzcs2::ui::UiTextId;

constexpr wchar_t kWindowClassName[] = L"nSkinzCS2.ToolWindow";
constexpr const wchar_t* kWindowTitle = nskinzcs2::build::kDisplayName.data();
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

struct StickerWeaponPreviewLayout
{
    std::array<ImVec2, 5> preset_offsets {};
};

StickerWeaponPreviewLayout ResolveStickerWeaponPreviewLayout(const int definition_index)
{
    StickerWeaponPreviewLayout result;
    for (std::size_t slot = 0; slot < result.preset_offsets.size(); ++slot)
    {
        const auto preset = nskinzcs2::ui::sticker_preview::DefaultPreset(definition_index, slot);
        result.preset_offsets[slot] = ImVec2(preset.offset_x, preset.offset_y);
    }
    return result;
}

struct StickerWeaponPreviewBounds
{
    ImVec2 minimum {};
    ImVec2 maximum {};
    const nskinzcs2::ui::weapon_preview::Model* model = nullptr;
    const nskinzcs2::ui::sticker_preview::WeaponLayout* placement_layout = nullptr;
};

StickerWeaponPreviewBounds ResolveStickerWeaponPreviewBounds(
    const ImVec2 minimum,
    const ImVec2 maximum,
    const int definition_index,
    const bool legacy_model)
{
    using namespace nskinzcs2::ui::weapon_preview;
    const auto* placement_layout =
        nskinzcs2::ui::sticker_preview::FindWeaponLayout(definition_index, legacy_model);
    const auto model = std::find_if(
        kModels.begin(),
        kModels.end(),
        [definition_index](const Model& candidate) { return candidate.definition == definition_index; });
    if (model == kModels.end())
    {
        return {minimum, maximum, nullptr, placement_layout};
    }

    const auto available_width = maximum.x - minimum.x;
    const auto available_height = maximum.y - minimum.y;
    const auto aspect = placement_layout != nullptr
        ? static_cast<float>(nskinzcs2::ui::sticker_preview::kSilhouetteWidth) /
            static_cast<float>(nskinzcs2::ui::sticker_preview::kSilhouetteHeight)
        : model->aspect;
    const auto target_width = std::min(available_width, available_height * aspect);
    const auto target_height = target_width / aspect;
    const ImVec2 model_minimum(
        minimum.x + (available_width - target_width) * 0.5f,
        minimum.y + (available_height - target_height) * 0.5f);
    return {
        model_minimum,
        ImVec2(model_minimum.x + target_width, model_minimum.y + target_height),
        &*model,
        placement_layout,
    };
}

void DrawStickerWeaponPreviewSilhouette(
    ImDrawList* draw,
    const StickerWeaponPreviewBounds& bounds)
{
    if (bounds.placement_layout != nullptr)
    {
        using namespace nskinzcs2::ui::sticker_preview;
        const auto width = bounds.maximum.x - bounds.minimum.x;
        const auto height = bounds.maximum.y - bounds.minimum.y;
        const ImU32 fill = IM_COL32(67, 75, 86, 230);
        const ImU32 outline = IM_COL32(153, 164, 180, 240);
        const auto first = bounds.placement_layout->first_point;
        const auto last = first + bounds.placement_layout->point_count;
        std::vector<ImVec2> vertices;
        vertices.reserve(bounds.placement_layout->point_count);
        for (std::uint32_t index = first; index < last; ++index)
        {
            const auto& point = kSilhouettePoints[index];
            vertices.emplace_back(
                bounds.minimum.x + static_cast<float>(point.x) /
                    static_cast<float>(kSilhouetteWidth - 1u) * width,
                bounds.minimum.y + static_cast<float>(point.y) /
                    static_cast<float>(kSilhouetteHeight - 1u) * height);
        }
        if (vertices.size() >= 3)
        {
            draw->AddConcavePolyFilled(vertices.data(), static_cast<int>(vertices.size()), fill);
            draw->AddPolyline(vertices.data(), static_cast<int>(vertices.size()), outline, true, 1.35f);
        }
        return;
    }

    using namespace nskinzcs2::ui::weapon_preview;
    if (bounds.model == nullptr)
    {
        draw->AddRectFilled(bounds.minimum, bounds.maximum, IM_COL32(61, 68, 78, 130), 5.0f);
        draw->AddRect(bounds.minimum, bounds.maximum, IM_COL32(133, 143, 158, 210), 5.0f);
        return;
    }

    const auto target_width = bounds.maximum.x - bounds.minimum.x;
    const auto target_height = bounds.maximum.y - bounds.minimum.y;
    const ImU32 fill = IM_COL32(67, 75, 86, 225);
    const ImU32 outline = IM_COL32(153, 164, 180, 240);
    std::vector<ImVec2> vertices;
    for (std::uint32_t path_index = bounds.model->first_path;
         path_index < bounds.model->first_path + bounds.model->path_count;
         ++path_index)
    {
        const auto& path = kPaths[path_index];
        vertices.clear();
        vertices.reserve(path.point_count);
        for (std::uint32_t point_index = path.first_point;
             point_index < path.first_point + path.point_count;
             ++point_index)
        {
            const auto& source = kPoints[point_index];
            vertices.emplace_back(
                bounds.minimum.x + (1.0f - static_cast<float>(source.x) / 65535.0f) * target_width,
                bounds.minimum.y + static_cast<float>(source.y) / 65535.0f * target_height);
        }
        if (vertices.size() >= 3)
        {
            draw->AddConcavePolyFilled(vertices.data(), static_cast<int>(vertices.size()), fill);
            draw->AddPolyline(vertices.data(), static_cast<int>(vertices.size()), outline, true, 1.35f);
        }
    }
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

            if (profile.keychain_kit <= 0 && !profile.explicit_clear)
            {
                continue;
            }

            if (profile.keychain_kit > 0 &&
                !ContainsNamedId(catalog.keychain_kits, profile.keychain_kit))
            {
                issue = L"Keychain profile '" + std::wstring(profile.name.begin(), profile.name.end()) +
                    L"' is invalid: selected keychain kit was not found in the catalog.";
                return false;
            }
        }

        return true;
    }

    bool TryValidateMapRemapForApply(
        const ConfigDocument& document,
        std::wstring& issue)
    {
        issue.clear();
        const auto& profile = document.map_remap;
        if (!profile.enabled)
        {
            return true;
        }
        const bool valid_workshop_id = !profile.workshop_id.empty() &&
            std::all_of(profile.workshop_id.begin(), profile.workshop_id.end(), [](const unsigned char character)
            {
                return std::isdigit(character) != 0;
            });
        const auto valid_map_name = [](const std::string& value, const bool allow_empty)
        {
            if (value.empty())
            {
                return allow_empty;
            }
            return value.size() < 256 &&
                std::all_of(value.begin(), value.end(), [](const unsigned char character)
                {
                    return std::isalnum(character) != 0 || character == '_' || character == '-' ||
                        character == '/' || character == '\\' || character == '.';
                });
        };
        if (!valid_workshop_id || !valid_map_name(profile.target_map_name, false) ||
            !valid_map_name(profile.source_map_name, true))
        {
            issue = L"Map replacement is invalid: select an installed workshop map and use a numeric workshop ID.";
            return false;
        }
        if (IsLikelyAuxiliaryWorkshopMap(profile.target_map_name))
        {
            issue = L"Map replacement is invalid: the selected target is an auxiliary skybox/background map. Rescan and select the playable map from the same workshop item.";
            return false;
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

// Fold only the ASCII range while comparing UTF-8 strings.  Applying the C
// locale's std::tolower() to each byte is both needlessly expensive for a
// large catalog and not Unicode-aware (the bytes of a Chinese code point are
// not individual characters).  Keeping non-ASCII bytes unchanged still gives
// exact UTF-8 substring matching for localized names and avoids allocating a
// second string for every item on every ImGui frame.
unsigned char FoldAscii(const unsigned char ch) noexcept
{
    return ch >= static_cast<unsigned char>('A') && ch <= static_cast<unsigned char>('Z')
        ? static_cast<unsigned char>(ch + ('a' - 'A'))
        : ch;
}

bool ContainsCaseInsensitive(const std::string_view haystack, const std::string_view needle) noexcept
{
    if (needle.empty())
    {
        return true;
    }

    if (needle.size() > haystack.size())
    {
        return false;
    }

    for (std::size_t start = 0; start + needle.size() <= haystack.size(); ++start)
    {
        bool matches = true;
        for (std::size_t offset = 0; offset < needle.size(); ++offset)
        {
            if (FoldAscii(static_cast<unsigned char>(haystack[start + offset])) !=
                FoldAscii(static_cast<unsigned char>(needle[offset])))
            {
                matches = false;
                break;
            }
        }

        if (matches)
        {
            return true;
        }
    }

    return false;
}

// Return the largest prefix that ends on a complete UTF-8 code point.  The
// search edit control stores UTF-8 in a fixed ImGui buffer; truncating a
// Chinese query in the middle of a three-byte code point can leave invalid
// text for the renderer.  This helper keeps the buffer valid even when a
// future catalog contains names longer than the control's capacity.
std::size_t Utf8PrefixSize(const std::string_view value, const std::size_t max_bytes) noexcept
{
    const std::size_t limit = std::min(value.size(), max_bytes);
    std::size_t position = 0;

    while (position < limit)
    {
        const unsigned char lead = static_cast<unsigned char>(value[position]);
        std::size_t width = 1;
        if (lead >= 0xC2U && lead <= 0xDFU)
        {
            width = 2;
        }
        else if (lead >= 0xE0U && lead <= 0xEFU)
        {
            width = 3;
        }
        else if (lead >= 0xF0U && lead <= 0xF4U)
        {
            width = 4;
        }
        else if (lead >= 0x80U && lead <= 0xBFU)
        {
            // A stray continuation byte is still safe to copy as one byte;
            // ImGui will replace it when the user edits the query.
            ++position;
            continue;
        }

        if (position + width > limit || position + width > value.size())
        {
            break;
        }

        bool valid = true;
        for (std::size_t offset = 1; offset < width; ++offset)
        {
            const unsigned char continuation = static_cast<unsigned char>(value[position + offset]);
            if (continuation < 0x80U || continuation > 0xBFU)
            {
                valid = false;
                break;
            }
        }

        if (!valid)
        {
            ++position;
            continue;
        }

        position += width;
    }

    return position;
}

bool InputSearchText(const char* label, const char* hint, std::string& value)
{
    // Keep the ImGui edit buffer independent from the std::string stored in
    // the filter map.  This avoids handing a resizable C++ object to the
    // Win32/IME input path, which is especially sensitive while a CJK
    // composition string is being committed.  2048 bytes is much larger than
    // any catalog name and still keeps the input control bounded.
    constexpr std::size_t kSearchBufferSize = 2048;
    std::array<char, kSearchBufferSize> buffer {};
    const auto copy_size = Utf8PrefixSize(value, kSearchBufferSize - 1);
    if (copy_size != 0)
    {
        std::copy_n(value.data(), copy_size, buffer.data());
    }

    const bool changed = ImGui::InputTextWithHint(
        label,
        hint,
        buffer.data(),
        buffer.size(),
        ImGuiInputTextFlags_NoUndoRedo);

    if (changed)
    {
        std::size_t text_size = 0;
        while (text_size < buffer.size() && buffer[text_size] != '\0')
        {
            ++text_size;
        }
        const std::string_view text(buffer.data(), text_size);
        value.assign(text.data(), Utf8PrefixSize(text, text.size()));
    }

    return changed;
}

bool ComboById(const char* label, const std::vector<NamedId>& values, int& id, const int popup_count = 10,
    const char* preview_override = nullptr)
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

    if (ImGui::BeginCombo(label, preview_override != nullptr ? preview_override : values[current_index].name.c_str()))
    {
        auto& filter = filters[combo_id];
        ImGui::PushID(static_cast<int>(combo_id));
        ImGui::SetNextItemWidth(-FLT_MIN);
        InputSearchText("##search", Ui(UiTextId::SearchHint), filter);
        const auto complete_utf8_size = Utf8PrefixSize(filter, filter.size());
        if (complete_utf8_size != filter.size())
        {
            filter.resize(complete_utf8_size);
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
                    const bool selected = values[index].id == id;
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

constexpr int kExplicitClearSelectionId = -1;

bool ComboFinishByIntent(
    const char* label,
    const std::vector<NamedId>& catalog_values,
    int& paint_kit,
    bool& explicit_vanilla_finish,
    const int popup_count = 10,
    const char* preview_override = nullptr)
{
    auto values = catalog_values;
    const auto no_change = std::find_if(
        values.begin(),
        values.end(),
        [](const NamedId& value) { return value.id == 0; });
    if (no_change == values.end())
    {
        values.insert(values.begin(), NamedId {0, Ui(UiTextId::NoChange)});
    }
    else
    {
        no_change->name = Ui(UiTextId::NoChange);
    }

    const auto vanilla_position = std::find_if(
        values.begin(),
        values.end(),
        [](const NamedId& value) { return value.id == 0; });
    values.insert(
        vanilla_position + 1,
        NamedId {kExplicitClearSelectionId, Ui(UiTextId::VanillaNoFinish)});

    int selected_id = explicit_vanilla_finish ? kExplicitClearSelectionId : paint_kit;
    if (!ComboById(label, values, selected_id, popup_count, preview_override))
    {
        return false;
    }

    explicit_vanilla_finish = selected_id == kExplicitClearSelectionId;
    paint_kit = explicit_vanilla_finish ? 0 : selected_id;
    return true;
}

bool ComboKeychainByIntent(
    const char* label,
    const std::vector<NamedId>& catalog_values,
    int& keychain_kit,
    bool& explicit_clear,
    const int popup_count = 10)
{
    auto values = catalog_values;
    const auto no_change = std::find_if(
        values.begin(),
        values.end(),
        [](const NamedId& value) { return value.id == 0; });
    if (no_change == values.end())
    {
        values.insert(values.begin(), NamedId {0, Ui(UiTextId::NoChange)});
    }
    else
    {
        no_change->name = Ui(UiTextId::NoChange);
    }

    const auto clear_position = std::find_if(
        values.begin(),
        values.end(),
        [](const NamedId& value) { return value.id == 0; });
    values.insert(
        clear_position + 1,
        NamedId {kExplicitClearSelectionId, Ui(UiTextId::NoKeychainRemove)});

    int selected_id = explicit_clear ? kExplicitClearSelectionId : keychain_kit;
    if (!ComboById(label, values, selected_id, popup_count))
    {
        return false;
    }

    explicit_clear = selected_id == kExplicitClearSelectionId;
    keychain_kit = explicit_clear ? 0 : selected_id;
    return true;
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

std::wstring Wide(const std::string& value)
{
    if (value.empty())
    {
        return {};
    }
    const auto count = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0)
    {
        return std::wstring(value.begin(), value.end());
    }
    std::wstring output(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), output.data(), count);
    return output;
}

class WindowController
{
public:
    WindowController(
        AppState& state,
        std::atomic_bool& stop_requested,
        const DWORD external_runtime_process_id)
        : state_(state)
        , stop_requested_(stop_requested)
        , config_path_(ResolveConfigPath())
        , external_runtime_process_id_(external_runtime_process_id)
    {
        state_.SetWindowVisible(false);
        working_config_ = state_.SnapshotConfig();
        if (working_config_.profiles.empty())
        {
            working_config_ = ConfigStore::CreateDefault();
            state_.ReplaceConfig(working_config_);
        }

        working_config_dirty_ = false;
        RefreshWorkshopMaps();
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
            Logger::Info(L"External UI D3D11 device created using hardware acceleration.");
        }
        else
        {
            result = D3D11CreateDeviceAndSwapChain(
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
                Logger::Warn(L"External UI hardware D3D11 creation failed; using the WARP software fallback.");
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
        FlushStickerLivePreview();

        ImGui::Render();
        constexpr float clear_color[4] {0.08f, 0.08f, 0.08f, 1.0f};
        device_context_->OMSetRenderTargets(1, &render_target_view_, nullptr);
        device_context_->ClearRenderTargetView(render_target_view_, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        const auto present_result = swap_chain_->Present(1, 0);
        if (FAILED(present_result))
        {
            if (!present_failure_logged_)
            {
                present_failure_logged_ = true;
                std::wstringstream stream;
                stream << L"External UI DXGI Present failed with HRESULT 0x"
                       << std::hex << std::uppercase
                       << static_cast<unsigned long>(present_result);
                Logger::Warn(stream.str());
            }
            return;
        }
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
                    auto& profile = CurrentProfile();
                    DrawStickersEditor(
                        catalog,
                        profile.stickers,
                        profile.base_item,
                        profile.paint_kit,
                        "weapon_stickers",
                        selected_weapon_sticker_);
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

            if (ImGui::BeginTabItem(Ui(UiTextId::TabMaps)))
            {
                DrawMapRemapEditor(status);
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem(Ui(UiTextId::TabDemoRewrite)))
            {
                DrawDemoSkinRewriteEditor(catalog);
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

        ImGui::Separator();
        DrawActions();

        ImGui::Separator();
        if (ImGui::CollapsingHeader(Ui(UiTextId::RuntimeDiagnostics)))
        {
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
            profile.seed = NormalizeCosmeticSeed(profile.seed);
            PushWorkingConfig();
        }

        if (ImGui::InputInt(Ui(UiTextId::StatTrak), &profile.stat_trak))
        {
            PushWorkingConfig();
        }

        if (ImGui::SliderFloat(
                Ui(UiTextId::Wear),
                &profile.wear,
                kMinimumCosmeticWear,
                kMaximumCosmeticWear,
                "%.7f"))
        {
            PushWorkingConfig();
        }

        const auto weapon_paints = nskinzcs2::CatalogBuilder::BuildCompatibleWeaponPaintList(catalog, profile.base_item);
        const auto paint_compatibility = nskinzcs2::CatalogBuilder::CheckWeaponPaintCompatibility(
            &catalog, profile.base_item, profile.paint_kit);
        const auto blocked_paint = paint_compatibility != nskinzcs2::WeaponPaintCompatibility::Compatible;
        // Preserve imported/staged invalid values and identify them honestly.
        // Do not silently show '-' or convert an incompatible paint to vanilla.
        std::string blocked_preview;
        if (blocked_paint)
        {
            const auto found = std::find_if(catalog.weapon_paint_kits.begin(), catalog.weapon_paint_kits.end(),
                [&](const NamedId& paint) { return paint.id == profile.paint_kit; });
            blocked_preview = std::string(Ui(UiTextId::WeaponPaintBlocked)) + " " +
                (found != catalog.weapon_paint_kits.end() ? found->name : std::string {}) +
                " (" + std::to_string(profile.paint_kit) + ")";
        }
        if (ComboFinishByIntent(
                Ui(UiTextId::PaintKit),
                weapon_paints,
                profile.paint_kit,
                profile.explicit_vanilla_finish,
                12,
                blocked_paint ? blocked_preview.c_str() : nullptr))
        {
            PushWorkingConfig();
        }
        ImGui::TextDisabled(Ui(UiTextId::CompatibleWeaponPaintCount), static_cast<int>(std::count_if(
            weapon_paints.begin(), weapon_paints.end(), [](const NamedId& paint) { return paint.id > 0; })));
        if (!catalog.weapon_paint_compatibility.contains(profile.base_item) ||
            catalog.weapon_paint_compatibility.at(profile.base_item).empty())
        {
            ImGui::TextWrapped("%s", Ui(UiTextId::WeaponPaintCompatibilityUnknown));
        }
        else if (blocked_paint)
        {
            ImGui::TextWrapped("%s", Ui(UiTextId::WeaponPaintIncompatible));
        }

        if (ComboById(Ui(UiTextId::Quality), catalog.qualities, profile.quality, 8))
        {
            PushWorkingConfig();
        }

        char custom_name_buffer[64] {};
        CopyToBuffer(profile.custom_name, custom_name_buffer, sizeof(custom_name_buffer));
        if (ImGui::InputTextWithHint(
                Ui(UiTextId::NameTag),
                nskinzcs2::model::kDefaultCosmeticCustomName.data(),
                custom_name_buffer,
                sizeof(custom_name_buffer)))
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
        const bool runtime_finish_only_active =
            status.knife_runtime_path == L"Knife finish-only runtime path active";
        const bool runtime_model_swap_active =
            status.knife_runtime_path == L"Knife model swap active";
        const bool runtime_model_swap_degraded =
            status.knife_runtime_path == L"Knife model swap degraded";
        const bool runtime_degraded = runtime_model_swap_degraded && !status.knife_runtime_reason.empty();
        const bool runtime_model_development =
            status.knife_runtime_path == L"Independent knife model (experimental)";

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
            if (profile.mode == KnifeProfileMode::FinishOnly)
            {
                profile.model_definition = 0;
            }
            PushWorkingConfig();
        }

        if (profile.mode == KnifeProfileMode::ModelAndFinish)
        {
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
            if (ComboById(Ui(UiTextId::KnifeModel), knife_model_items, profile.model_definition, 10))
            {
                // The mode selector is authoritative. Selecting/clearing a
                // model must never silently switch finish-only/model modes.
                PushWorkingConfig();
            }
            ImGui::TextWrapped("%s", Ui(UiTextId::KnifeTargetVanillaHint));
        }

        const auto knife_paints =
            profile.mode == KnifeProfileMode::ModelAndFinish && profile.model_definition > 0
                ? nskinzcs2::CatalogBuilder::BuildCompatibleWeaponPaintList(
                    catalog, profile.model_definition)
                : catalog.weapon_paint_kits;
        const auto knife_paint_compatibility =
            profile.mode == KnifeProfileMode::ModelAndFinish && profile.model_definition > 0
                ? nskinzcs2::CatalogBuilder::CheckWeaponPaintCompatibility(
                    &catalog, profile.model_definition, profile.paint_kit)
                : nskinzcs2::WeaponPaintCompatibility::Compatible;
        const auto blocked_knife_paint =
            knife_paint_compatibility != nskinzcs2::WeaponPaintCompatibility::Compatible;
        std::string blocked_knife_preview;
        if (blocked_knife_paint)
        {
            const auto found = std::find_if(
                catalog.weapon_paint_kits.begin(),
                catalog.weapon_paint_kits.end(),
                [&](const NamedId& paint) { return paint.id == profile.paint_kit; });
            blocked_knife_preview = std::string(Ui(UiTextId::WeaponPaintBlocked)) + " " +
                (found != catalog.weapon_paint_kits.end() ? found->name : std::string {}) +
                " (" + std::to_string(profile.paint_kit) + ")";
        }
        if (ComboFinishByIntent(
                Ui(UiTextId::PaintKit),
                knife_paints,
                profile.paint_kit,
                profile.explicit_vanilla_finish,
                12,
                blocked_knife_paint ? blocked_knife_preview.c_str() : nullptr))
        {
            PushWorkingConfig();
        }
        if (profile.mode == KnifeProfileMode::ModelAndFinish &&
            profile.model_definition > 0 &&
            blocked_knife_paint)
        {
            ImGui::TextWrapped("%s", Ui(UiTextId::WeaponPaintIncompatible));
        }

        if (ComboById(Ui(UiTextId::Quality), catalog.qualities, profile.quality, 8))
        {
            PushWorkingConfig();
        }

        if (ImGui::InputInt(Ui(UiTextId::Seed), &profile.seed))
        {
            profile.seed = NormalizeCosmeticSeed(profile.seed);
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
        if (ImGui::InputTextWithHint(
                Ui(UiTextId::NameTag),
                nskinzcs2::model::kDefaultCosmeticCustomName.data(),
                custom_name_buffer,
                sizeof(custom_name_buffer)))
        {
            profile.custom_name = custom_name_buffer;
            PushWorkingConfig();
        }

        const bool model_and_finish_requested = profile.mode == KnifeProfileMode::ModelAndFinish;
        const bool missing_model_definition = model_and_finish_requested && profile.model_definition == 0;
        if (missing_model_definition)
        {
            ImGui::TextWrapped("%s", Ui(UiTextId::KnifeModelRequired));
        }
        else if (model_and_finish_requested && runtime_model_development)
        {
            ImGui::TextColored(ImVec4(0.94f, 0.68f, 0.25f, 1.0f), "%s", Ui(UiTextId::KnifeModelDevelopmentPending));
            if (!status.knife_runtime_reason.empty())
                ImGui::TextWrapped("%s", Narrow(status.knife_runtime_reason).c_str());
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
                    profile.explicit_vanilla_finish = false;
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
                    profile.explicit_vanilla_finish = false;
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
        if (ComboFinishByIntent(
                Ui(UiTextId::PaintKit),
                glove_paint_list,
                profile.paint_kit,
                profile.explicit_vanilla_finish,
                12))
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
            profile.seed = NormalizeCosmeticSeed(profile.seed);
            PushWorkingConfig();
        }

        if (ImGui::SliderFloat(
                Ui(UiTextId::Wear),
                &profile.wear,
                kMinimumCosmeticWear,
                kMaximumCosmeticWear,
                "%.7f"))
        {
            PushWorkingConfig();
        }

        char custom_name_buffer[64] {};
        CopyToBuffer(profile.custom_name, custom_name_buffer, sizeof(custom_name_buffer));
        if (ImGui::InputTextWithHint(
                Ui(UiTextId::NameTag),
                nskinzcs2::model::kDefaultCosmeticCustomName.data(),
                custom_name_buffer,
                sizeof(custom_name_buffer)))
        {
            profile.custom_name = custom_name_buffer;
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

        if (ComboKeychainByIntent(
                Ui(UiTextId::Keychain),
                catalog.keychain_kits,
                profile.keychain_kit,
                profile.explicit_clear,
                12))
        {
            PushWorkingConfig();
        }

        if (profile.explicit_clear)
        {
            ImGui::TextColored(
                ImVec4(0.94f, 0.42f, 0.25f, 1.0f),
                "%s",
                Ui(UiTextId::KeychainClearDescription));
        }

        ImGui::Spacing();
        ImGui::TextWrapped("%s", Ui(UiTextId::KeychainRuntimeDescription));
    }

    void RefreshWorkshopMaps()
    {
        const auto selected_id = working_config_.map_remap.workshop_id;
        const auto selected_map = working_config_.map_remap.target_map_name;
        workshop_scan_ = nskinzcs2::runtime::DiscoverSubscribedWorkshopMaps(external_runtime_process_id_);
        selected_workshop_map_ = -1;
        for (int index = 0; index < static_cast<int>(workshop_scan_.entries.size()); ++index)
        {
            const auto& entry = workshop_scan_.entries[static_cast<std::size_t>(index)];
            if (entry.workshop_id == selected_id && entry.map_name == selected_map)
            {
                selected_workshop_map_ = index;
                if (working_config_.map_remap.target_skybox_resource_prefix !=
                    entry.skybox_resource_prefix)
                {
                    working_config_.map_remap.target_skybox_resource_prefix =
                        entry.skybox_resource_prefix;
                    PushWorkingConfig();
                    Logger::Info(
                        L"Refreshed the selected Workshop map 3D skybox resource prefix: " +
                        (entry.skybox_resource_prefix.empty()
                            ? std::wstring(L"<not uniquely detected>")
                            : Wide(entry.skybox_resource_prefix)) + L".");
                }
                break;
            }
        }

        // Older builds exposed separately compiled skybox worlds in the map
        // list. Repair that saved choice only when its Workshop item now has
        // exactly one playable target, so packages with real variants still
        // require an explicit user selection.
        if (selected_workshop_map_ < 0 && IsLikelyAuxiliaryWorkshopMap(selected_map))
        {
            int unique_playable_index = -1;
            for (int index = 0; index < static_cast<int>(workshop_scan_.entries.size()); ++index)
            {
                if (workshop_scan_.entries[static_cast<std::size_t>(index)].workshop_id != selected_id)
                {
                    continue;
                }
                if (unique_playable_index >= 0)
                {
                    unique_playable_index = -2;
                    break;
                }
                unique_playable_index = index;
            }

            if (unique_playable_index >= 0)
            {
                const auto& replacement =
                    workshop_scan_.entries[static_cast<std::size_t>(unique_playable_index)];
                selected_workshop_map_ = unique_playable_index;
                working_config_.map_remap.workshop_title = replacement.title;
                working_config_.map_remap.addon_name = replacement.addon_name;
                working_config_.map_remap.target_map_name = replacement.map_name;
                working_config_.map_remap.target_skybox_resource_prefix =
                    replacement.skybox_resource_prefix;
                working_config_.map_remap.radar_overlay_request_virtual_path.clear();
                working_config_.map_remap.radar_overlay_virtual_path.clear();
                working_config_.map_remap.radar_overlay_root_path.clear();
                working_config_.map_remap.radar_overlay_file_path.clear();
                PushWorkingConfig();
                workshop_scan_.diagnostics.push_back(
                    L"Replaced saved auxiliary map '" + Wide(selected_map) +
                    L"' with playable map '" + Wide(replacement.map_name) + L"'.");
                Logger::Info(
                    L"Migrated the saved auxiliary workshop target from " +
                    Wide(selected_map) + L" to " + Wide(replacement.map_name) + L".");
            }
        }

        Logger::Info(
            L"Workshop map scan completed: " +
            std::to_wstring(workshop_scan_.entries.size()) +
            L" CS2 map entries detected.");
    }

    void DrawMapRemapEditor(const nskinzcs2::runtime::Status& status)
    {
        auto& profile = working_config_.map_remap;
        if (ImGui::Button(Ui(UiTextId::MapRescan)))
        {
            RefreshWorkshopMaps();
        }
        ImGui::SameLine();
        ImGui::Text(
            Ui(UiTextId::MapDetectedFormat),
            static_cast<int>(workshop_scan_.entries.size()));

        for (const auto& diagnostic : workshop_scan_.diagnostics)
        {
            ImGui::TextDisabled("%s", Narrow(diagnostic).c_str());
        }

        if (workshop_scan_.entries.empty())
        {
            ImGui::TextWrapped("%s", Ui(UiTextId::MapNoEntries));
        }

        ImGui::Separator();
        ImGui::Columns(2, "MapRemapColumns", false);
        ImGui::BeginChild("WorkshopMapList", ImVec2(0.0f, 300.0f), true);
        for (int index = 0; index < static_cast<int>(workshop_scan_.entries.size()); ++index)
        {
            const auto& entry = workshop_scan_.entries[static_cast<std::size_t>(index)];
            std::string label = entry.title + "  [" + entry.map_name + "]##" +
                entry.workshop_id + "/" + entry.map_name;
            if (ImGui::Selectable(label.c_str(), selected_workshop_map_ == index))
            {
                selected_workshop_map_ = index;
            }
        }
        ImGui::EndChild();

        ImGui::NextColumn();
        if (ImGui::Checkbox(Ui(UiTextId::MapEnable), &profile.enabled))
        {
            PushWorkingConfig();
        }
        if (ImGui::Checkbox(Ui(UiTextId::MapUseSourceRadar), &profile.use_source_radar))
        {
            PushWorkingConfig();
        }
        ImGui::TextWrapped("%s", Ui(UiTextId::MapUseSourceRadarHelp));
        if (profile.use_source_radar)
        {
            bool radar_settings_released = false;
            const auto invalidate_prepared_radar = [&]()
            {
                profile.radar_overlay_request_virtual_path.clear();
                profile.radar_overlay_virtual_path.clear();
                profile.radar_overlay_root_path.clear();
                profile.radar_overlay_file_path.clear();
                PushWorkingConfig();
            };
            if (ImGui::SliderInt(
                    Ui(UiTextId::MapRadarBlackTolerance),
                    &profile.radar_black_tolerance,
                    0,
                    96,
                    "%d"))
            {
                invalidate_prepared_radar();
            }
            radar_settings_released |= ImGui::IsItemDeactivatedAfterEdit();
            ImGui::TextWrapped("%s", Ui(UiTextId::MapRadarBlackToleranceHelp));
            if (ImGui::SliderInt(
                    Ui(UiTextId::MapRadarDarkDetailRadius),
                    &profile.radar_dark_detail_radius,
                    0,
                    64,
                    "%d px"))
            {
                invalidate_prepared_radar();
            }
            radar_settings_released |= ImGui::IsItemDeactivatedAfterEdit();
            ImGui::TextWrapped("%s", Ui(UiTextId::MapRadarDarkDetailRadiusHelp));
            if (ImGui::SliderInt(
                    Ui(UiTextId::MapRadarBackgroundAlpha),
                    &profile.radar_background_alpha_percent,
                    0,
                    100,
                    "%d%%"))
            {
                invalidate_prepared_radar();
            }
            radar_settings_released |= ImGui::IsItemDeactivatedAfterEdit();
            ImGui::TextWrapped("%s", Ui(UiTextId::MapRadarBackgroundAlphaHelp));
            if (ImGui::Button(Ui(UiTextId::MapRadarDefaults), ImVec2(-1.0f, 0.0f)))
            {
                profile.radar_black_tolerance = 12;
                profile.radar_dark_detail_radius = 12;
                profile.radar_background_alpha_percent = 0;
                invalidate_prepared_radar();
                radar_settings_released = true;
            }
            if (radar_settings_released)
            {
                PublishWorkingConfigUpdate(L"radar-control");
            }
        }

        if (profile.source_map_name.empty())
        {
            const auto detected_source = !status.level_name.empty()
                ? Narrow(status.level_name)
                : Narrow(status.map_remap_source);
            if (!detected_source.empty())
            {
                profile.source_map_name = detected_source;
                PushWorkingConfig();
                Logger::Info(
                    L"Auto-filled the source demo map from runtime state: " +
                    Wide(detected_source) + L".");
            }
        }

        char source_map_buffer[256] {};
        strncpy_s(source_map_buffer, profile.source_map_name.c_str(), _TRUNCATE);
        if (ImGui::InputText(Ui(UiTextId::MapSourceName), source_map_buffer, sizeof(source_map_buffer)))
        {
            profile.source_map_name = source_map_buffer;
            PushWorkingConfig();
        }
        ImGui::TextWrapped("%s", Ui(UiTextId::MapSourceHelp));
        ImGui::Spacing();

        if (selected_workshop_map_ >= 0 &&
            selected_workshop_map_ < static_cast<int>(workshop_scan_.entries.size()))
        {
            const auto& selected = workshop_scan_.entries[static_cast<std::size_t>(selected_workshop_map_)];
            ImGui::TextWrapped(
                Ui(UiTextId::MapTargetFormat),
                selected.map_name.c_str(),
                selected.addon_name.c_str());
            ImGui::Text(Ui(UiTextId::MapWorkshopFormat), selected.workshop_id.c_str());
            if (ImGui::Button(Ui(UiTextId::MapUseSelected), ImVec2(-1.0f, 30.0f)))
            {
                profile.enabled = true;
                profile.workshop_id = selected.workshop_id;
                profile.workshop_title = selected.title;
                profile.addon_name = selected.addon_name;
                profile.target_map_name = selected.map_name;
                profile.target_skybox_resource_prefix = selected.skybox_resource_prefix;
                profile.radar_overlay_request_virtual_path.clear();
                profile.radar_overlay_virtual_path.clear();
                profile.radar_overlay_root_path.clear();
                profile.radar_overlay_file_path.clear();
                PushWorkingConfig();
                Logger::Info(
                    L"Staged workshop map " +
                    Wide(selected.title) +
                    L" (" + Wide(selected.workshop_id) + L"/" + Wide(selected.map_name) + L").");
            }
        }
        else
        {
            ImGui::TextWrapped("%s", Ui(UiTextId::MapSelectHelp));
        }

        if (!profile.target_map_name.empty())
        {
            ImGui::Separator();
            ImGui::TextWrapped(
                Ui(UiTextId::MapTargetFormat),
                profile.target_map_name.c_str(),
                profile.addon_name.c_str());
            ImGui::Text(Ui(UiTextId::MapWorkshopFormat), profile.workshop_id.c_str());
        }
        ImGui::Columns(1);
        ImGui::Separator();
        ImGui::TextWrapped("%s", Ui(UiTextId::MapSecondLoadHelp));

        if (status.map_remap_enabled || status.map_remap_hooks_installed)
        {
            const auto source = status.map_remap_source.empty()
                ? std::string("<DemoHeader>")
                : Narrow(status.map_remap_source);
            ImGui::TextWrapped(
                Ui(UiTextId::MapRuntimeFormat),
                status.map_remap_hooks_installed ? Ui(UiTextId::Yes) : Ui(UiTextId::No),
                source.c_str(),
                Narrow(status.map_remap_target).c_str(),
                static_cast<unsigned long long>(status.map_remap_replacement_count));
            if (!status.map_remap_issue.empty())
            {
                ImGui::TextColored(
                    ImVec4(0.94f, 0.42f, 0.25f, 1.0f),
                    "%s",
                    Narrow(status.map_remap_issue).c_str());
            }
        }
    }

    void RefreshDemoSkinRewriteResult()
    {
        if (!demo_skin_rewrite_future_.valid() ||
            demo_skin_rewrite_future_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        {
            return;
        }
        try
        {
            demo_skin_rewrite_result_ = demo_skin_rewrite_future_.get();
        }
        catch (const std::exception& exception)
        {
            demo_skin_rewrite_result_ = {};
            demo_skin_rewrite_result_.detail =
                L"Temporary demo rewrite worker failed: " + Wide(exception.what());
        }
        catch (...)
        {
            demo_skin_rewrite_result_ = {};
            demo_skin_rewrite_result_.detail = L"Temporary demo rewrite worker failed unexpectedly.";
        }

        if (!demo_skin_rewrite_result_.success)
        {
            return;
        }
        if (!nskinzcs2::ui::PublishExternalDemoCommand(
                external_runtime_process_id_,
                config_path_.parent_path(),
                demo_skin_rewrite_result_.playdemo_command))
        {
            demo_skin_rewrite_result_.detail +=
                L" The file was generated, but the playdemo command bridge was unavailable; press Load generated demo again after the runtime is loaded.";
            Logger::Warn(demo_skin_rewrite_result_.detail);
            return;
        }
        demo_skin_rewrite_result_.detail += L" The playdemo command was queued in CS2.";
        Logger::Info(demo_skin_rewrite_result_.detail);
    }

    void RefreshCapturedDemoSkinSource()
    {
        const auto observed = nskinzcs2::runtime::ReadCurrentDemoPathSidecar(
            config_path_.parent_path());
        if (!observed || observed->empty() || *observed == demo_skin_last_observed_path_)
        {
            return;
        }
        demo_skin_last_observed_path_ = *observed;
        if (!nskinzcs2::runtime::IsGeneratedDemoSkinPath(*observed))
        {
            demo_skin_source_path_ = *observed;
            Logger::Info(L"Temporary demo UI captured original source: " + observed->wstring() + L".");
        }
    }

    void DrawDemoSkinRewriteEditor(const Catalog& catalog)
    {
        RefreshCapturedDemoSkinSource();
        RefreshDemoSkinRewriteResult();

        ImGui::TextWrapped("%s", Ui(UiTextId::DemoRewriteDescription));
        ImGui::Separator();

        char source_buffer[2048] {};
        const auto encoded_source = Narrow(demo_skin_source_path_.wstring());
        strncpy_s(source_buffer, encoded_source.c_str(), _TRUNCATE);
        if (ImGui::InputText(
                Ui(UiTextId::DemoRewriteSource),
                source_buffer,
                sizeof(source_buffer)))
        {
            demo_skin_source_path_ = std::filesystem::path(Wide(source_buffer));
        }
        if (demo_skin_source_path_.empty())
        {
            ImGui::TextWrapped("%s", Ui(UiTextId::DemoRewriteSourceWaiting));
        }

        const auto plan = nskinzcs2::runtime::BuildDefaultDemoSkinRewritePlan(working_config_, &catalog);
        ImGui::Text(
            Ui(UiTextId::DemoRewriteProfilesFormat),
            static_cast<int>(plan.passes.size()),
            plan.weapon_item_count,
            plan.sticker_slot_request_count,
            plan.knife_item_count,
            plan.glove_item_count);
        if (!plan.validation_issue.empty())
        {
            ImGui::TextColored(
                ImVec4(0.94f, 0.42f, 0.25f, 1.0f),
                "%s",
                Narrow(plan.validation_issue).c_str());
        }

        const auto busy = demo_skin_rewrite_future_.valid();
        ImGui::BeginDisabled(
            busy || demo_skin_source_path_.empty() || plan.passes.empty() || !plan.validation_issue.empty());
        if (ImGui::Button(Ui(UiTextId::DemoRewriteGenerateLoad), ImVec2(-1.0f, 34.0f)))
        {
            const auto config = working_config_;
            const auto rewrite_catalog = catalog;
            const auto source = demo_skin_source_path_;
            const auto process_id = external_runtime_process_id_;
            demo_skin_rewrite_result_ = {};
            demo_skin_rewrite_result_.detail = L"Rewrite started.";
            demo_skin_rewrite_future_ = std::async(
                std::launch::async,
                [config, rewrite_catalog, source, process_id]()
                {
                    return nskinzcs2::runtime::GenerateTemporarySkinnedDemo(
                        config,
                        &rewrite_catalog,
                        source,
                        process_id);
                });
        }
        ImGui::EndDisabled();

        if (busy)
        {
            ImGui::TextWrapped("%s", Ui(UiTextId::DemoRewriteInProgress));
        }
        if (!demo_skin_rewrite_result_.detail.empty())
        {
            ImGui::TextWrapped(
                Ui(UiTextId::DemoRewriteResult),
                Narrow(demo_skin_rewrite_result_.detail).c_str());
        }
        if (demo_skin_rewrite_result_.success &&
            !demo_skin_rewrite_result_.playdemo_command.empty())
        {
            if (ImGui::Button(Ui(UiTextId::DemoRewriteLoadAgain), ImVec2(-1.0f, 30.0f)))
            {
                if (!nskinzcs2::ui::PublishExternalDemoCommand(
                        external_runtime_process_id_,
                        config_path_.parent_path(),
                        demo_skin_rewrite_result_.playdemo_command))
                {
                    demo_skin_rewrite_result_.detail =
                        L"The generated demo is intact, but the runtime playdemo command bridge is unavailable.";
                }
                else
                {
                    demo_skin_rewrite_result_.detail = L"The generated demo playdemo command was queued again.";
                }
            }
        }
    }

    void QueueStickerLivePreview()
    {
        PushWorkingConfig();
        if (!sticker_live_preview_enabled_ || external_runtime_process_id_ == 0)
        {
            return;
        }

        if (!sticker_live_preview_pending_)
        {
            sticker_live_preview_due_ =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(140);
        }
        sticker_live_preview_pending_ = true;
    }

    void FlushStickerLivePreview()
    {
        if (!sticker_live_preview_pending_)
        {
            return;
        }
        if (!sticker_live_preview_enabled_ || external_runtime_process_id_ == 0)
        {
            sticker_live_preview_pending_ = false;
            return;
        }
        if (std::chrono::steady_clock::now() < sticker_live_preview_due_)
        {
            return;
        }

        // Clear before publishing so a failed bridge does not retry every
        // render frame. A later edit schedules a fresh attempt.
        sticker_live_preview_pending_ = false;
        PublishWorkingConfigUpdate(L"sticker-live-preview");
    }

    void DrawStickerPlacementPreview(
        std::array<nskinzcs2::model::StickerProfile, 5>& stickers,
        int& selected_sticker,
        const int weapon_definition,
        const bool legacy_model,
        const std::string& weapon_name)
    {
        selected_sticker = std::clamp(selected_sticker, 0, 4);
        ImGui::TextUnformatted(Ui(UiTextId::StickerPlacementPreview));
        const auto available_width = std::max(280.0f, ImGui::GetContentRegionAvail().x);
        const auto canvas_height = std::clamp(available_width * 0.53f, 245.0f, 315.0f);
        const ImVec2 canvas_size(available_width, canvas_height);
        const auto origin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##sticker_placement_canvas", canvas_size);
        const auto canvas_max = ImVec2(origin.x + canvas_size.x, origin.y + canvas_size.y);
        const auto preview_bounds = ResolveStickerWeaponPreviewBounds(
            ImVec2(origin.x + 10.0f, origin.y + 30.0f),
            ImVec2(canvas_max.x - 10.0f, canvas_max.y - 36.0f),
            weapon_definition,
            legacy_model);
        const auto preview_width = std::max(1.0f, preview_bounds.maximum.x - preview_bounds.minimum.x);
        const auto preview_height = std::max(1.0f, preview_bounds.maximum.y - preview_bounds.minimum.y);

        std::array<ImVec2, 5> preset_centers {};
        std::array<ImVec2, 5> marker_centers {};
        std::array<ImVec2, 5> effective_offsets {};
        std::array<ImVec2, 5> stored_offsets {};
        const auto offset_to_canvas = [&](const std::size_t slot, const ImVec2 offset)
        {
            const auto point = nskinzcs2::ui::sticker_preview::EffectiveOffsetToPreview(
                weapon_definition,
                slot,
                legacy_model,
                offset.x,
                offset.y);
            return ImVec2(
                preview_bounds.minimum.x + point.x * preview_width,
                preview_bounds.minimum.y + point.y * preview_height);
        };
        const auto update_centers = [&]
        {
            for (std::size_t slot = 0; slot < stickers.size(); ++slot)
            {
                const auto preset = nskinzcs2::ui::sticker_preview::EffectivePreset(
                    weapon_definition, slot, legacy_model);
                preset_centers[slot] = offset_to_canvas(
                    slot, ImVec2(preset.offset_x, preset.offset_y));
                const auto& sticker = stickers[slot];
                const auto default_stored =
                    nskinzcs2::ui::sticker_preview::DefaultStoredOffset(
                        weapon_definition,
                        slot,
                        legacy_model);
                stored_offsets[slot] = sticker.custom_position
                    ? ImVec2(
                        nskinzcs2::model::NormalizeStickerOffset(sticker.offset_x),
                        nskinzcs2::model::NormalizeStickerOffset(sticker.offset_y))
                    : ImVec2(default_stored.x, default_stored.y);
                const auto effective =
                    nskinzcs2::ui::sticker_preview::StoredToMaterialOffset(
                        weapon_definition,
                        slot,
                        stored_offsets[slot].x,
                        stored_offsets[slot].y,
                        legacy_model);
                effective_offsets[slot] = ImVec2(effective.x, effective.y);
                marker_centers[slot] = offset_to_canvas(slot, effective_offsets[slot]);
            }
        };
        update_centers();

        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            const auto mouse = ImGui::GetIO().MousePos;
            float nearest_distance_squared = 30.0f * 30.0f;
            int nearest_slot = -1;
            for (int slot = 0; slot < static_cast<int>(marker_centers.size()); ++slot)
            {
                const auto delta_x = mouse.x - marker_centers[static_cast<std::size_t>(slot)].x;
                const auto delta_y = mouse.y - marker_centers[static_cast<std::size_t>(slot)].y;
                const auto distance_squared = delta_x * delta_x + delta_y * delta_y;
                if (distance_squared <= nearest_distance_squared)
                {
                    nearest_distance_squared = distance_squared;
                    nearest_slot = slot;
                }
            }
            sticker_preview_dragging_ = nearest_slot >= 0;
            if (nearest_slot >= 0)
            {
                selected_sticker = nearest_slot;
            }
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            sticker_preview_dragging_ = false;
        }
        if (sticker_preview_dragging_ && ImGui::IsItemActive() &&
            ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f))
        {
            auto& sticker = stickers[static_cast<std::size_t>(selected_sticker)];
            const auto mouse = ImGui::GetIO().MousePos;
            const auto normalized_mouse_x = (mouse.x - preview_bounds.minimum.x) / preview_width;
            const auto normalized_mouse_y = (mouse.y - preview_bounds.minimum.y) / preview_height;
            const auto current_effective = effective_offsets[static_cast<std::size_t>(selected_sticker)];
            const nskinzcs2::ui::sticker_preview::PreviewPoint inverse_hint {
                current_effective.x,
                current_effective.y,
            };
            const auto material_offset = nskinzcs2::ui::sticker_preview::PreviewToEffectiveOffset(
                weapon_definition,
                static_cast<std::size_t>(selected_sticker),
                legacy_model,
                normalized_mouse_x,
                normalized_mouse_y,
                &inverse_hint);
            const auto offset = nskinzcs2::ui::sticker_preview::MaterialToStoredOffset(
                weapon_definition,
                static_cast<std::size_t>(selected_sticker),
                material_offset.x,
                material_offset.y,
                legacy_model);
            const auto new_x = nskinzcs2::model::NormalizeStickerOffset(
                offset.x);
            const auto new_y = nskinzcs2::model::NormalizeStickerOffset(
                offset.y);
            if (!sticker.custom_position ||
                std::fabs(sticker.offset_x - new_x) > 0.00001f ||
                std::fabs(sticker.offset_y - new_y) > 0.00001f)
            {
                sticker.custom_position = true;
                sticker.offset_x = new_x;
                sticker.offset_y = new_y;
                QueueStickerLivePreview();
                update_centers();
            }
        }

        auto* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilledMultiColor(
            origin,
            canvas_max,
            IM_COL32(34, 38, 45, 255),
            IM_COL32(29, 33, 40, 255),
            IM_COL32(22, 25, 31, 255),
            IM_COL32(27, 30, 36, 255));
        draw->AddRect(origin, canvas_max, IM_COL32(105, 112, 122, 255));
        for (int division = 1; division < 5; ++division)
        {
            const auto x = origin.x + canvas_size.x * static_cast<float>(division) / 5.0f;
            draw->AddLine(
                ImVec2(x, origin.y + 28.0f),
                ImVec2(x, canvas_max.y - 34.0f),
                IM_COL32(53, 58, 66, 150));
        }
        for (int division = 1; division < 4; ++division)
        {
            const auto y = origin.y + 28.0f + (canvas_size.y - 62.0f) *
                static_cast<float>(division) / 4.0f;
            draw->AddLine(
                ImVec2(origin.x, y),
                ImVec2(canvas_max.x, y),
                IM_COL32(53, 58, 66, 150));
        }

        DrawStickerWeaponPreviewSilhouette(draw, preview_bounds);

        const auto title = weapon_name + "  [" + std::to_string(weapon_definition) + "]";
        draw->AddText(
            ImVec2(origin.x + 9.0f, origin.y + 7.0f),
            IM_COL32(235, 238, 243, 255),
            title.c_str());
        const auto selected_offset = stored_offsets[static_cast<std::size_t>(selected_sticker)];
        char coordinate_text[128] {};
        std::snprintf(
            coordinate_text,
            sizeof(coordinate_text),
            "#%d   dX %.5f   dY %.5f",
            selected_sticker + 1,
            selected_offset.x,
            selected_offset.y);
        const auto coordinate_size = ImGui::CalcTextSize(coordinate_text);
        draw->AddText(
            ImVec2(canvas_max.x - coordinate_size.x - 9.0f, origin.y + 7.0f),
            IM_COL32(255, 210, 54, 255),
            coordinate_text);

        for (std::size_t slot = 0; slot < stickers.size(); ++slot)
        {
            draw->AddCircle(
                preset_centers[slot],
                4.0f,
                IM_COL32(155, 164, 176, 210),
                0,
                1.3f);
            const auto delta_x = marker_centers[slot].x - preset_centers[slot].x;
            const auto delta_y = marker_centers[slot].y - preset_centers[slot].y;
            if (delta_x * delta_x + delta_y * delta_y > 1.0f)
            {
                draw->AddLine(
                    preset_centers[slot],
                    marker_centers[slot],
                    slot == static_cast<std::size_t>(selected_sticker)
                        ? IM_COL32(255, 210, 54, 210)
                        : IM_COL32(210, 215, 222, 125),
                    1.2f);
            }
        }

        const auto draw_sticker = [&](const std::size_t slot)
        {
            const auto& sticker = stickers[slot];
            const bool selected_slot = slot == static_cast<std::size_t>(selected_sticker);
            const bool configured = sticker.kit_id > 0;
            const auto preset = nskinzcs2::ui::sticker_preview::EffectivePreset(
                weapon_definition, slot, legacy_model);
            const auto authored_scale = std::max(1.0f, preset.authored_scale);
            // g_vStickerNScale is an inverse UV scale. The visible sticker
            // island spans roughly +/-1/scale around its center; applying the
            // exact VMDL Jacobian makes overlaps and perspective match the gun.
            const auto half_uv_extent =
                std::clamp(sticker.scale, 0.1f, 5.0f) / authored_scale;
            const auto radians = sticker.rotation * 3.14159265358979323846f / 180.0f;
            const auto cosine = std::cos(radians);
            const auto sine = std::sin(radians);
            const std::array<ImVec2, 4> local {
                ImVec2(-half_uv_extent, -half_uv_extent),
                ImVec2(half_uv_extent, -half_uv_extent),
                ImVec2(half_uv_extent, half_uv_extent),
                ImVec2(-half_uv_extent, half_uv_extent),
            };
            std::array<ImVec2, 4> points {};
            for (std::size_t index = 0; index < points.size(); ++index)
            {
                const auto rotated_x = local[index].x * cosine - local[index].y * sine;
                const auto rotated_y = local[index].x * sine + local[index].y * cosine;
                points[index] = offset_to_canvas(
                    slot,
                    ImVec2(
                        effective_offsets[slot].x + rotated_x,
                        effective_offsets[slot].y + rotated_y));
            }
            const ImU32 color = selected_slot
                ? IM_COL32(255, 210, 54, 235)
                : configured
                    ? IM_COL32(245, 247, 250, 220)
                    : IM_COL32(150, 157, 168, 52);
            const ImU32 outline = selected_slot
                ? IM_COL32(255, 226, 112, 255)
                : configured
                    ? IM_COL32(255, 255, 255, 255)
                    : IM_COL32(155, 162, 173, 155);
            draw->AddQuadFilled(points[0], points[1], points[2], points[3], color);
            draw->AddQuad(points[0], points[1], points[2], points[3], outline, selected_slot ? 2.8f : 1.8f);
            const auto label = std::to_string(slot + 1);
            const auto text_size = ImGui::CalcTextSize(label.c_str());
            draw->AddText(
                ImVec2(
                    marker_centers[slot].x - text_size.x * 0.5f,
                    marker_centers[slot].y - text_size.y * 0.5f),
                selected_slot || configured ? IM_COL32(20, 23, 28, 255) : IM_COL32(225, 228, 234, 210),
                label.c_str());
        };
        for (std::size_t slot = 0; slot < stickers.size(); ++slot)
        {
            if (slot != static_cast<std::size_t>(selected_sticker))
            {
                draw_sticker(slot);
            }
        }
        draw_sticker(static_cast<std::size_t>(selected_sticker));

        const auto legend_y = canvas_max.y - 25.0f;
        for (std::size_t slot = 0; slot < stickers.size(); ++slot)
        {
            const bool selected_slot = slot == static_cast<std::size_t>(selected_sticker);
            const bool configured = stickers[slot].kit_id > 0;
            const auto label = "#" + std::to_string(slot + 1);
            const auto text_size = ImGui::CalcTextSize(label.c_str());
            const auto center_x = origin.x + canvas_size.x *
                (static_cast<float>(slot) + 0.5f) / static_cast<float>(stickers.size());
            draw->AddCircleFilled(
                ImVec2(center_x - text_size.x * 0.5f - 7.0f, legend_y + text_size.y * 0.5f),
                3.0f,
                selected_slot
                    ? IM_COL32(255, 210, 54, 255)
                    : configured
                        ? IM_COL32(245, 247, 250, 255)
                        : IM_COL32(130, 137, 148, 180));
            draw->AddText(
                ImVec2(center_x - text_size.x * 0.5f, legend_y),
                selected_slot
                    ? IM_COL32(255, 210, 54, 255)
                    : configured
                        ? IM_COL32(245, 247, 250, 255)
                        : IM_COL32(130, 137, 148, 210),
                label.c_str());
        }

        const auto selected_anchor = preset_centers[static_cast<std::size_t>(selected_sticker)];
        draw->AddText(
            ImVec2(selected_anchor.x + 6.0f, selected_anchor.y + 5.0f),
            IM_COL32(181, 189, 200, 230),
            Ui(UiTextId::StickerPresetOrigin));
        ImGui::TextWrapped("%s", Ui(UiTextId::StickerPlacementHelp));
    }

    void DrawStickersEditor(
        const Catalog& catalog,
        std::array<nskinzcs2::model::StickerProfile, 5>& stickers,
        const int weapon_definition,
        const int paint_kit,
        const char* scope_id,
        int& selected_sticker)
    {
        const auto legacy_entry = catalog.weapon_paint_uses_legacy_model.find(paint_kit);
        const bool legacy_model = legacy_entry == catalog.weapon_paint_uses_legacy_model.end()
            ? true
            : legacy_entry->second;
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
            if (sticker.kit_id > 0 &&
                !nskinzcs2::ui::sticker_preview::HasNativePreset(
                    weapon_definition,
                    static_cast<std::size_t>(selected_sticker),
                    legacy_model))
            {
                const auto stored = nskinzcs2::ui::sticker_preview::DefaultStoredOffset(
                    weapon_definition,
                    static_cast<std::size_t>(selected_sticker),
                    legacy_model);
                sticker.custom_position = true;
                sticker.offset_x = stored.x;
                sticker.offset_y = stored.y;
            }
            QueueStickerLivePreview();
        }

        if (ImGui::SliderFloat(Ui(UiTextId::StickerWear), &sticker.wear, 0.0000001f, 1.0f, "%.7f"))
        {
            QueueStickerLivePreview();
        }

        if (ImGui::SliderFloat(Ui(UiTextId::Scale), &sticker.scale, 0.1f, 5.0f, "%.3f"))
        {
            QueueStickerLivePreview();
        }

        if (ImGui::SliderFloat(Ui(UiTextId::Rotation), &sticker.rotation, 0.0f, 360.0f, "%.1f"))
        {
            QueueStickerLivePreview();
        }

        if (ImGui::Checkbox(Ui(UiTextId::StickerCustomPosition), &sticker.custom_position))
        {
            if (sticker.custom_position)
            {
                const auto preset = nskinzcs2::ui::sticker_preview::DefaultStoredOffset(
                    weapon_definition,
                    static_cast<std::size_t>(selected_sticker),
                    legacy_model);
                sticker.offset_x = preset.x;
                sticker.offset_y = preset.y;
            }
            else
            {
                sticker.offset_x = 0.0f;
                sticker.offset_y = 0.0f;
            }
            QueueStickerLivePreview();
        }

        ImGui::BeginDisabled(!sticker.custom_position);
        if (ImGui::DragFloat(
                Ui(UiTextId::StickerOffsetX),
                &sticker.offset_x,
                0.0001f,
                0.0f,
                0.0f,
                "%.5f"))
        {
            QueueStickerLivePreview();
        }
        if (ImGui::DragFloat(
                Ui(UiTextId::StickerOffsetY),
                &sticker.offset_y,
                0.0001f,
                0.0f,
                0.0f,
                "%.5f"))
        {
            QueueStickerLivePreview();
        }
        ImGui::EndDisabled();

        if (ImGui::Button(Ui(UiTextId::StickerResetPresetPosition), ImVec2(-1.0f, 0.0f)))
        {
            const auto slot = static_cast<std::size_t>(selected_sticker);
            if (nskinzcs2::ui::sticker_preview::HasNativePreset(
                    weapon_definition,
                    slot,
                    legacy_model))
            {
                sticker.custom_position = false;
                sticker.offset_x = 0.0f;
                sticker.offset_y = 0.0f;
            }
            else
            {
                const auto preset = nskinzcs2::ui::sticker_preview::DefaultStoredOffset(
                    weapon_definition,
                    slot,
                    legacy_model);
                sticker.custom_position = true;
                sticker.offset_x = preset.x;
                sticker.offset_y = preset.y;
            }
            QueueStickerLivePreview();
        }

        if (ImGui::Checkbox(Ui(UiTextId::StickerLivePreview), &sticker_live_preview_enabled_))
        {
            if (sticker_live_preview_enabled_)
            {
                QueueStickerLivePreview();
            }
            else
            {
                sticker_live_preview_pending_ = false;
            }
        }
        ImGui::TextWrapped("%s", Ui(UiTextId::StickerLivePreviewHelp));
        if (sticker_live_preview_pending_)
        {
            ImGui::TextDisabled("%s", Ui(UiTextId::StickerLivePreviewPending));
        }

        DrawStickerPlacementPreview(
            stickers,
            selected_sticker,
            weapon_definition,
            legacy_model,
            LookupName(catalog.base_items, weapon_definition));
        ImGui::TextWrapped("%s", Ui(UiTextId::StickerExistingSlotReplacementHelp));

        ImGui::Columns(1);
        ImGui::PopID();
    }

    void DrawActions()
    {
        const float width = ImGui::GetContentRegionAvail().x / 3.0f;
        const auto catalog = state_.SnapshotCatalog();
        if (ImGui::Button(Ui(UiTextId::Update), ImVec2(width - 4.0f, 30.0f)))
        {
            PublishWorkingConfigUpdate(L"Update");
        }

        ImGui::SameLine();
        if (ImGui::Button(Ui(UiTextId::Save), ImVec2(width - 4.0f, 30.0f)))
        {
            std::wstring validation_issue;
            if (!TryValidateKnifeProfilesForApply(working_config_, catalog, validation_issue) ||
                !TryValidateGloveProfilesForApply(working_config_, catalog, validation_issue) ||
                !TryValidateKeychainProfilesForApply(working_config_, catalog, validation_issue) ||
                !TryValidateMapRemapForApply(working_config_, validation_issue))
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

    bool PublishWorkingConfigUpdate(const wchar_t* source)
    {
        const auto catalog = state_.SnapshotCatalog();
        const auto runtime_status = state_.SnapshotStatus();
        std::wstring validation_issue;
        if (!TryValidateKnifeProfilesForApply(working_config_, catalog, validation_issue) ||
            !TryValidateGloveProfilesForApply(working_config_, catalog, validation_issue) ||
            !TryValidateKeychainProfilesForApply(working_config_, catalog, validation_issue) ||
            !TryValidateMapRemapForApply(working_config_, validation_issue))
        {
            Logger::Warn(L"UI rejected staged config update: " + validation_issue);
            return false;
        }

        const bool external_update = external_runtime_process_id_ != 0;
        const bool update_published = !external_update ||
            nskinzcs2::ui::PublishExternalConfigUpdate(
                external_runtime_process_id_,
                config_path_,
                working_config_);
        if (!update_published)
        {
            Logger::Warn(
                L"UI could not publish the staged config to the injected runtime; "
                L"the editor remains dirty.");
            return false;
        }

        if (!external_update)
        {
            Logger::Warn(L"Unexpected in-process UI update path used.");
        }
        state_.ReplaceConfig(working_config_);
        if (!external_update &&
            ShouldUseLightweightDemoKeychainRefresh(runtime_status, working_config_))
        {
            state_.RequestDemoKeychainRefresh();
        }
        else if (!external_update)
        {
            state_.RequestManualRefresh();
        }
        sticker_live_preview_pending_ = false;
        working_config_dirty_ = false;
        Logger::Info(
            std::wstring(external_update ? L"External UI" : L"UI") +
            L" saved and published staged config via " +
            (source != nullptr ? source : L"Update") + L".");
        return true;
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
            if (external_runtime_process_id_ != 0)
            {
                auto config = state_.SnapshotConfig();
                if (nskinzcs2::ui::PublishExternalConfigUpdate(
                    external_runtime_process_id_,
                    config_path_,
                    config))
                {
                    state_.ReplaceConfig(config);
                }
            }
            else
            {
                state_.RequestManualRefresh();
            }
            Logger::Info(L"Runtime Update requested by local regression automation through the isolated UI.");
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
    DWORD external_runtime_process_id_ = 0;
    ConfigDocument working_config_;
    bool working_config_dirty_ = false;
    int selected_profile_ = 0;
    int selected_knife_profile_ = 0;
    int selected_glove_profile_ = 0;
    int selected_keychain_profile_ = 0;
    int selected_workshop_map_ = -1;
    int selected_weapon_sticker_ = 0;
    bool sticker_live_preview_enabled_ = true;
    bool sticker_live_preview_pending_ = false;
    bool sticker_preview_dragging_ = false;
    std::chrono::steady_clock::time_point sticker_live_preview_due_ {};
    WorkshopMapScanResult workshop_scan_;
    std::filesystem::path demo_skin_source_path_;
    std::filesystem::path demo_skin_last_observed_path_;
    std::future<nskinzcs2::runtime::DemoSkinRewriteResult> demo_skin_rewrite_future_;
    nskinzcs2::runtime::DemoSkinRewriteResult demo_skin_rewrite_result_;
    bool last_insert_state_ = false;
    bool imgui_initialized_ = false;
    bool resources_created_ = false;
    bool window_class_registered_ = false;
    bool first_frame_started_ = false;
    bool first_dx11_frame_completed_ = false;
    bool first_win32_frame_completed_ = false;
    bool first_imgui_frame_started_ = false;
    bool first_frame_presented_ = false;
    bool present_failure_logged_ = false;

    HWND hwnd_ = nullptr;
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* device_context_ = nullptr;
    IDXGISwapChain* swap_chain_ = nullptr;
    ID3D11RenderTargetView* render_target_view_ = nullptr;
};
}

nskinzcs2::MainWindow::MainWindow(
    AppState& state,
    const DWORD external_runtime_process_id)
    : state_(state)
    , external_runtime_process_id_(external_runtime_process_id)
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
        WindowController controller(
            state_,
            stop_requested_,
            external_runtime_process_id_);
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
