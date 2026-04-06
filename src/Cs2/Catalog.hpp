#pragma once

#include "Core/AppState.hpp"

#include <filesystem>
#include <optional>
#include <string_view>

namespace nskinzcs2
{
class CatalogBuilder
{
public:
    static model::Catalog BuildFallbackCatalog();
    static std::optional<model::Catalog> BuildCatalogFromGameText(
        std::string_view items_game_text,
        std::string_view localization_text);
    static std::optional<model::Catalog> BuildCatalogFromGameFiles(
        const std::filesystem::path& items_game_path,
        const std::filesystem::path& localization_path);
    static bool HasDuplicateIds(const std::vector<model::NamedId>& values);
};
}
