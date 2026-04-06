#pragma once

#include "Core/AppState.hpp"

#include <filesystem>
#include <optional>

namespace nskinzcs2
{
class ConfigStore
{
public:
    static model::ConfigDocument CreateDefault();
    static model::ConfigDocument Load(const std::filesystem::path& path);
    static bool Save(const std::filesystem::path& path, const model::ConfigDocument& document);
    static model::CosmeticProfile* FindMatchingProfile(
        model::ConfigDocument& document,
        std::uint64_t xuid,
        int base_item);
    static const model::CosmeticProfile* FindMatchingProfile(
        const model::ConfigDocument& document,
        std::uint64_t xuid,
        int base_item);
    static model::GloveProfile* FindMatchingGloveProfile(
        model::ConfigDocument& document,
        std::uint64_t xuid);
    static const model::GloveProfile* FindMatchingGloveProfile(
        const model::ConfigDocument& document,
        std::uint64_t xuid);
    static model::KnifeProfile* FindMatchingKnifeProfile(
        model::ConfigDocument& document,
        std::uint64_t xuid);
    static const model::KnifeProfile* FindMatchingKnifeProfile(
        const model::ConfigDocument& document,
        std::uint64_t xuid);
};
}
