#include "Cs2/CatalogStore.hpp"

#include "json.hpp"

#include <Windows.h>

#include <fstream>
#include <string>

namespace
{
using json = nlohmann::json;
using nskinzcs2::model::Catalog;
using nskinzcs2::model::KeychainVisualInfo;
using nskinzcs2::model::NamedId;
using nskinzcs2::model::VisualInfo;

json SaveNamedIds(const std::vector<NamedId>& values)
{
    auto output = json::array();
    for (const auto& value : values)
    {
        output.push_back({{"id", value.id}, {"name", value.name}});
    }
    return output;
}

std::vector<NamedId> LoadNamedIds(const json& input)
{
    std::vector<NamedId> output;
    if (!input.is_array())
    {
        return output;
    }
    output.reserve(input.size());
    for (const auto& value : input)
    {
        if (value.is_object())
        {
            output.push_back(NamedId {
                value.value("id", 0),
                value.value("name", std::string {})});
        }
    }
    return output;
}

json SaveCatalog(const Catalog& catalog)
{
    json output;
    output["version"] = 1;
    output["base_items"] = SaveNamedIds(catalog.base_items);
    output["knife_items"] = SaveNamedIds(catalog.knife_items);
    output["glove_items"] = SaveNamedIds(catalog.glove_items);
    output["weapon_paint_kits"] = SaveNamedIds(catalog.weapon_paint_kits);
    output["weapon_paint_model_metadata"] = json::array();
    for (const auto& [paint, legacy] : catalog.weapon_paint_uses_legacy_model)
    {
        output["weapon_paint_model_metadata"].push_back({{"paint", paint}, {"legacy", legacy}});
    }
    output["glove_paint_kits"] = SaveNamedIds(catalog.glove_paint_kits);
    output["weapon_paint_compatibility"] = json::array();
    for (const auto& [definition, paints] : catalog.weapon_paint_compatibility)
    {
        output["weapon_paint_compatibility"].push_back({{"definition", definition}, {"paints", paints}});
    }
    output["sticker_kits"] = SaveNamedIds(catalog.sticker_kits);
    output["keychain_kits"] = SaveNamedIds(catalog.keychain_kits);
    output["qualities"] = SaveNamedIds(catalog.qualities);
    output["runtime_populated"] = catalog.runtime_populated;
    output["warnings"] = catalog.warnings;

    output["glove_paint_compatibility"] = json::array();
    for (const auto& [paint, definition] : catalog.glove_paint_compatibility)
    {
        output["glove_paint_compatibility"].push_back(
            {{"paint", paint}, {"definition", definition}});
    }

    output["visuals"] = json::array();
    for (const auto& [definition, visual] : catalog.visuals)
    {
        output["visuals"].push_back({
            {"definition", definition},
            {"world_model", visual.world_model},
            {"view_model", visual.view_model},
            {"resolved_world_model_resource_path", visual.resolved_world_model_resource_path},
            {"resolved_view_model_resource_path", visual.resolved_view_model_resource_path},
            {"knife_family", visual.knife_family},
            {"killfeed_icon", visual.killfeed_icon},
            {"knife_identity_resolved", visual.knife_identity_resolved},
            {"is_knife", visual.is_knife},
            {"is_glove", visual.is_glove},
        });
    }

    output["keychain_visuals"] = json::array();
    for (const auto& [definition, visual] : catalog.keychain_visuals)
    {
        output["keychain_visuals"].push_back({
            {"definition", definition},
            {"model_path", visual.model_path},
            {"resolved_model_resource_path", visual.resolved_model_resource_path},
        });
    }
    return output;
}

Catalog LoadCatalog(const json& input)
{
    Catalog catalog;
    catalog.base_items = LoadNamedIds(input.value("base_items", json::array()));
    catalog.knife_items = LoadNamedIds(input.value("knife_items", json::array()));
    catalog.glove_items = LoadNamedIds(input.value("glove_items", json::array()));
    catalog.weapon_paint_kits = LoadNamedIds(input.value("weapon_paint_kits", json::array()));
    for (const auto& entry : input.value("weapon_paint_model_metadata", json::array()))
    {
        if (entry.is_object() && entry.contains("legacy") && entry["legacy"].is_boolean())
        {
            const auto paint = entry.value("paint", 0);
            if (paint > 0) { catalog.weapon_paint_uses_legacy_model[paint] = entry["legacy"].get<bool>(); }
        }
    }
    catalog.glove_paint_kits = LoadNamedIds(input.value("glove_paint_kits", json::array()));
    // An old snapshot has no compatibility map and stays Unknown, not unrestricted.
    for (const auto& entry : input.value("weapon_paint_compatibility", json::array()))
    {
        if (!entry.is_object() || !entry.contains("paints") || !entry["paints"].is_array()) { continue; }
        const auto definition = entry.value("definition", 0);
        if (definition <= 0) { continue; }
        auto& paints = catalog.weapon_paint_compatibility[definition];
        for (const auto& paint : entry["paints"])
        {
            if (paint.is_number_integer() && paint.get<int>() > 0) { paints.push_back(paint.get<int>()); }
        }
        std::sort(paints.begin(), paints.end());
        paints.erase(std::unique(paints.begin(), paints.end()), paints.end());
    }
    catalog.sticker_kits = LoadNamedIds(input.value("sticker_kits", json::array()));
    catalog.keychain_kits = LoadNamedIds(input.value("keychain_kits", json::array()));
    catalog.qualities = LoadNamedIds(input.value("qualities", json::array()));
    catalog.runtime_populated = input.value("runtime_populated", false);
    catalog.warnings = input.value("warnings", std::vector<std::string> {});

    for (const auto& entry : input.value("glove_paint_compatibility", json::array()))
    {
        if (entry.is_object())
        {
            catalog.glove_paint_compatibility.emplace(
                entry.value("paint", 0),
                entry.value("definition", 0));
        }
    }

    for (const auto& entry : input.value("visuals", json::array()))
    {
        if (!entry.is_object())
        {
            continue;
        }
        VisualInfo visual;
        visual.world_model = entry.value("world_model", std::string {});
        visual.view_model = entry.value("view_model", std::string {});
        visual.resolved_world_model_resource_path =
            entry.value("resolved_world_model_resource_path", std::string {});
        visual.resolved_view_model_resource_path =
            entry.value("resolved_view_model_resource_path", std::string {});
        visual.knife_family = entry.value("knife_family", std::string {});
        visual.killfeed_icon = entry.value("killfeed_icon", std::string {});
        visual.knife_identity_resolved = entry.value("knife_identity_resolved", false);
        visual.is_knife = entry.value("is_knife", false);
        visual.is_glove = entry.value("is_glove", false);
        catalog.visuals.emplace(entry.value("definition", 0), std::move(visual));
    }

    for (const auto& entry : input.value("keychain_visuals", json::array()))
    {
        if (!entry.is_object())
        {
            continue;
        }
        KeychainVisualInfo visual;
        visual.model_path = entry.value("model_path", std::string {});
        visual.resolved_model_resource_path =
            entry.value("resolved_model_resource_path", std::string {});
        catalog.keychain_visuals.emplace(entry.value("definition", 0), std::move(visual));
    }
    return catalog;
}
}

bool nskinzcs2::CatalogStore::Save(
    const std::filesystem::path& path,
    const model::Catalog& catalog)
{
    try
    {
        const auto temporary_path = path.wstring() + L".tmp";
        {
            std::ofstream stream(temporary_path, std::ios::binary | std::ios::trunc);
            if (!stream)
            {
                return false;
            }
            const auto text = SaveCatalog(catalog).dump();
            stream.write(text.data(), static_cast<std::streamsize>(text.size()));
            if (!stream)
            {
                return false;
            }
        }

        return MoveFileExW(
                   temporary_path.c_str(),
                   path.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    }
    catch (...)
    {
        return false;
    }
}

std::optional<nskinzcs2::model::Catalog> nskinzcs2::CatalogStore::Load(
    const std::filesystem::path& path)
{
    try
    {
        std::ifstream stream(path, std::ios::binary);
        if (!stream)
        {
            return std::nullopt;
        }
        const auto input = json::parse(stream, nullptr, false);
        if (input.is_discarded() || !input.is_object() || input.value("version", 0) != 1)
        {
            return std::nullopt;
        }
        return LoadCatalog(input);
    }
    catch (...)
    {
        return std::nullopt;
    }
}
