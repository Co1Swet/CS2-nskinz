#pragma once

#include "Core/AppState.hpp"

#include <filesystem>
#include <optional>
#include <string_view>

namespace nskinzcs2
{
enum class WeaponPaintCompatibility
{
    Compatible,
    Incompatible,
    Unknown,
};

class CatalogBuilder
{
public:
    static model::Catalog BuildFallbackCatalog();
    static std::optional<model::Catalog> BuildCatalogFromGameText(
        std::string_view items_game_text,
        std::string_view localization_text,
        std::string_view inventory_icon_paths_text = {});
    static std::optional<model::Catalog> BuildCatalogFromGameFiles(
        const std::filesystem::path& items_game_path,
        const std::filesystem::path& localization_path,
        const std::filesystem::path& directory_vpk_path = {});
    static std::optional<std::string> ReadInventoryIconPathsFromVpk(
        const std::filesystem::path& directory_vpk_path);
    static bool HasDuplicateIds(const std::vector<model::NamedId>& values);
    static WeaponPaintCompatibility CheckWeaponPaintCompatibility(
        const model::Catalog* catalog, int definition, int paint_kit) noexcept;
    static std::vector<model::NamedId> BuildCompatibleWeaponPaintList(
        const model::Catalog& catalog, int definition);
};
}
