#include "Core/Config.hpp"
#include "Core/Log.hpp"
#include "UI/StickerPreviewPlacement.generated.hpp"

#include "json.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>

using json = nlohmann::json;

namespace nskinzcs2::model
{
namespace
{
std::string ToString(const GloveProfileMode mode)
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

GloveProfileMode GloveProfileModeFromString(const std::string& value)
{
    return value == "model_and_finish"
        ? GloveProfileMode::ModelAndFinish
        : GloveProfileMode::FinishOnly;
}

std::string ToString(const KnifeProfileMode mode)
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

KnifeProfileMode KnifeProfileModeFromString(const std::string& value)
{
    return value == "model_and_finish"
        ? KnifeProfileMode::ModelAndFinish
        : KnifeProfileMode::FinishOnly;
}

bool IsLegacyKnifeBaseItem(const int base_item) noexcept
{
    static constexpr std::array<int, 21> kKnifeDefinitionIndices {
        42,
        59,
        500,
        503,
        505,
        506,
        507,
        508,
        509,
        512,
        514,
        515,
        516,
        517,
        518,
        519,
        520,
        521,
        522,
        523,
        525,
    };

    return std::find(kKnifeDefinitionIndices.begin(), kKnifeDefinitionIndices.end(), base_item) !=
        kKnifeDefinitionIndices.end();
}

template <typename Profile>
std::vector<Profile> DeduplicateProfilesByXuidKeepLast(const std::vector<Profile>& profiles)
{
    std::unordered_map<std::uint64_t, std::size_t> last_index_by_xuid;
    last_index_by_xuid.reserve(profiles.size());

    for (std::size_t index = 0; index < profiles.size(); ++index)
    {
        last_index_by_xuid[profiles[index].xuid] = index;
    }

    std::vector<Profile> deduplicated;
    deduplicated.reserve(last_index_by_xuid.size());
    for (std::size_t index = 0; index < profiles.size(); ++index)
    {
        const auto it = last_index_by_xuid.find(profiles[index].xuid);
        if (it != last_index_by_xuid.end() && it->second == index)
        {
            deduplicated.push_back(profiles[index]);
        }
    }

    return deduplicated;
}

std::vector<KeychainProfile> DeduplicateKeychainProfilesKeepLast(
    const std::vector<KeychainProfile>& profiles)
{
    std::unordered_map<std::uint64_t, std::size_t> last_index_by_binding_key;
    last_index_by_binding_key.reserve(profiles.size());

    auto binding_key_for =
        [](const KeychainProfile& profile) noexcept
        {
            return (profile.xuid << 32ull) ^ static_cast<std::uint32_t>(profile.base_item);
        };

    for (std::size_t index = 0; index < profiles.size(); ++index)
    {
        last_index_by_binding_key[binding_key_for(profiles[index])] = index;
    }

    std::vector<KeychainProfile> deduplicated;
    deduplicated.reserve(last_index_by_binding_key.size());
    for (std::size_t index = 0; index < profiles.size(); ++index)
    {
        const auto key = binding_key_for(profiles[index]);
        const auto it = last_index_by_binding_key.find(key);
        if (it != last_index_by_binding_key.end() && it->second == index)
        {
            deduplicated.push_back(profiles[index]);
        }
    }

    return deduplicated;
}

KnifeProfile ConvertLegacyKnifeProfile(const CosmeticProfile& profile)
{
    KnifeProfile knife_profile;
    knife_profile.name = profile.name.empty() ? std::string("Default Knife") : profile.name;
    knife_profile.enabled = profile.enabled;
    knife_profile.xuid = profile.xuid;
    knife_profile.mode =
        profile.override_item != 0
            ? KnifeProfileMode::ModelAndFinish
            : KnifeProfileMode::FinishOnly;
    knife_profile.model_definition = profile.override_item;
    knife_profile.paint_kit = profile.paint_kit;
    knife_profile.explicit_vanilla_finish = profile.explicit_vanilla_finish;
    knife_profile.quality = profile.quality;
    knife_profile.seed = profile.seed;
    knife_profile.stat_trak = profile.stat_trak;
    knife_profile.wear = profile.wear;
    knife_profile.custom_name = profile.custom_name;
    return knife_profile;
}

void NormalizeKnifeProfiles(
    ConfigDocument& document,
    const json& root)
{
    const auto has_explicit_knife_profiles =
        root.find("knife_profiles") != root.end() && root["knife_profiles"].is_array();

    std::vector<CosmeticProfile> retained_profiles;
    retained_profiles.reserve(document.profiles.size());

    std::vector<KnifeProfile> legacy_knife_profiles;
    legacy_knife_profiles.reserve(document.profiles.size());
    for (const auto& profile : document.profiles)
    {
        if (!IsLegacyKnifeBaseItem(profile.base_item))
        {
            retained_profiles.push_back(profile);
            continue;
        }

        if (profile.xuid != 0)
        {
            legacy_knife_profiles.push_back(ConvertLegacyKnifeProfile(profile));
        }
    }

    document.profiles = std::move(retained_profiles);
    document.knife_profiles = DeduplicateProfilesByXuidKeepLast(document.knife_profiles);

    if (!has_explicit_knife_profiles)
    {
        document.knife_profiles = DeduplicateProfilesByXuidKeepLast(legacy_knife_profiles);
        return;
    }

    if (legacy_knife_profiles.empty())
    {
        return;
    }

    std::unordered_map<std::uint64_t, bool> explicit_xuids;
    explicit_xuids.reserve(document.knife_profiles.size());
    for (const auto& profile : document.knife_profiles)
    {
        explicit_xuids[profile.xuid] = true;
    }

    std::vector<KnifeProfile> merged_knife_profiles = document.knife_profiles;
    merged_knife_profiles.reserve(document.knife_profiles.size() + legacy_knife_profiles.size());

    for (const auto& profile : legacy_knife_profiles)
    {
        if (explicit_xuids.find(profile.xuid) != explicit_xuids.end())
        {
            std::wstringstream warning;
            warning << L"Config migration ignored legacy knife profile for XUID "
                << profile.xuid
                << L": knife_profiles entry takes precedence.";
            nskinzcs2::Logger::Warn(warning.str());
            continue;
        }

        merged_knife_profiles.push_back(profile);
    }

    document.knife_profiles = DeduplicateProfilesByXuidKeepLast(merged_knife_profiles);
}

void MigrateAbsoluteStickerOffsetsToDeltas(ConfigDocument& document)
{
    for (auto& profile : document.profiles)
    {
        for (std::size_t slot = 0; slot < profile.stickers.size(); ++slot)
        {
            auto& sticker = profile.stickers[slot];
            if (!sticker.custom_position)
            {
                continue;
            }

            // Version 7 stored effective material coordinates. Convert them
            // to user deltas from the correct authored (or borrowed) home.
            const auto home = nskinzcs2::ui::sticker_preview::MaterialBaseOffset(
                profile.base_item, slot, true);
            sticker.offset_x = NormalizeStickerOffset(sticker.offset_x - home.x);
            sticker.offset_y = NormalizeStickerOffset(sticker.offset_y - home.y);
        }
    }
}

void MigrateVersionEightFifthStickerOffsets(ConfigDocument& document)
{
    for (auto& profile : document.profiles)
    {
        constexpr std::size_t slot = 4;
        auto& sticker = profile.stickers[slot];
        if (!sticker.custom_position)
        {
            continue;
        }

        const auto placement = ResolveStickerAnchor(profile.base_item, slot, true);
        if (!placement.BorrowsAnotherSlot())
        {
            continue;
        }

        // Version 8 had no schema/anchor support. A synthesized fifth slot was
        // therefore stored as an effective absolute UV coordinate. Preserve
        // its visible position while moving to the borrowed-home delta model.
        const auto home = nskinzcs2::ui::sticker_preview::MaterialBaseOffset(
            profile.base_item, slot, true);
        sticker.offset_x = NormalizeStickerOffset(sticker.offset_x - home.x);
        sticker.offset_y = NormalizeStickerOffset(sticker.offset_y - home.y);
    }
}
}

static void to_json(json& j, const StickerProfile& value)
{
    j = json {
        {"kit_id", value.kit_id},
        {"wear", value.wear},
        {"scale", value.scale},
        {"rotation", value.rotation},
        {"custom_position", value.custom_position},
        {"offset_x", NormalizeStickerOffset(value.offset_x)},
        {"offset_y", NormalizeStickerOffset(value.offset_y)},
    };
}

static void from_json(const json& j, StickerProfile& value)
{
    value.kit_id = j.value("kit_id", 0);
    value.wear = j.value("wear", 0.0000001f);
    value.scale = j.value("scale", 1.0f);
    value.rotation = j.value("rotation", 0.0f);
    value.custom_position = j.value("custom_position", false);
    value.offset_x = NormalizeStickerOffset(j.value("offset_x", 0.0f));
    value.offset_y = NormalizeStickerOffset(j.value("offset_y", 0.0f));
}

static void to_json(json& j, const CosmeticProfile& value)
{
    const auto serialized_wear =
        IsLegacyKnifeBaseItem(value.base_item) || value.base_item == 5028
            ? value.wear
            : NormalizeCosmeticWear(value.wear);
    j = json {
        {"name", value.name},
        {"enabled", value.enabled},
        {"xuid", value.xuid},
        {"base_item", value.base_item},
        {"override_item", value.override_item},
        {"paint_kit", value.paint_kit},
        {"explicit_vanilla_finish", value.explicit_vanilla_finish},
        {"quality", value.quality},
        {"seed", NormalizeCosmeticSeed(value.seed)},
        {"stat_trak", value.stat_trak},
        {"wear", serialized_wear},
        {"custom_name", value.custom_name},
        {"stickers", value.stickers},
    };
}

static void from_json(const json& j, CosmeticProfile& value)
{
    value.name = j.value("name", std::string("Default"));
    value.enabled = j.value("enabled", false);
    value.xuid = j.value("xuid", static_cast<std::uint64_t>(0));
    value.base_item = j.value("base_item", 7);
    value.override_item = j.value("override_item", 0);
    value.paint_kit = j.value("paint_kit", 0);
    value.explicit_vanilla_finish = j.value("explicit_vanilla_finish", false);
    value.quality = j.value("quality", 0);
    value.seed = NormalizeCosmeticSeed(j.value("seed", 0));
    value.stat_trak = j.value("stat_trak", 0);
    const auto parsed_wear = j.value("wear", kMinimumCosmeticWear);
    value.wear =
        IsLegacyKnifeBaseItem(value.base_item) || value.base_item == 5028
            ? parsed_wear
            : NormalizeCosmeticWear(parsed_wear);
    value.custom_name = j.value("custom_name", std::string());

    if (const auto stickers = j.find("stickers"); stickers != j.end() && stickers->is_array())
    {
        const auto parsed = stickers->get<std::vector<StickerProfile>>();
        const auto count = std::min(parsed.size(), value.stickers.size());
        for (std::size_t index = 0; index < count; ++index)
        {
            value.stickers[index] = parsed[index];
        }
    }
}

static void to_json(json& j, const KnifeProfile& value)
{
    j = json {
        {"name", value.name},
        {"enabled", value.enabled},
        {"xuid", value.xuid},
        {"mode", ToString(value.mode)},
        {"model_definition", value.model_definition},
        {"paint_kit", value.paint_kit},
        {"explicit_vanilla_finish", value.explicit_vanilla_finish},
        {"quality", value.quality},
        {"seed", NormalizeCosmeticSeed(value.seed)},
        {"stat_trak", value.stat_trak},
        {"wear", value.wear},
        {"custom_name", value.custom_name},
    };
}

static void from_json(const json& j, KnifeProfile& value)
{
    value.name = j.value("name", std::string("Default Knife"));
    value.enabled = j.value("enabled", false);
    value.xuid = j.value("xuid", static_cast<std::uint64_t>(0));
    value.mode = KnifeProfileModeFromString(j.value("mode", std::string("finish_only")));
    value.model_definition = j.value("model_definition", 0);
    value.paint_kit = j.value("paint_kit", 0);
    value.explicit_vanilla_finish = j.value("explicit_vanilla_finish", false);
    value.quality = j.value("quality", 0);
    value.seed = NormalizeCosmeticSeed(j.value("seed", 0));
    value.stat_trak = j.value("stat_trak", 0);
    value.wear = j.value("wear", 0.0000001f);
    value.custom_name = j.value("custom_name", std::string());
}

static void to_json(json& j, const GloveProfile& value)
{
    j = json {
        {"name", value.name},
        {"enabled", value.enabled},
        {"xuid", value.xuid},
        {"mode", ToString(value.mode)},
        {"model_definition", value.model_definition},
        {"paint_kit", value.paint_kit},
        {"explicit_vanilla_finish", value.explicit_vanilla_finish},
        {"quality", value.quality},
        {"seed", NormalizeCosmeticSeed(value.seed)},
        {"wear", NormalizeCosmeticWear(value.wear)},
        {"custom_name", value.custom_name},
    };
}

static void from_json(const json& j, GloveProfile& value)
{
    value.name = j.value("name", std::string("Default Gloves"));
    value.enabled = j.value("enabled", false);
    value.xuid = j.value("xuid", static_cast<std::uint64_t>(0));
    value.mode = GloveProfileModeFromString(j.value("mode", std::string("finish_only")));
    value.model_definition = j.value("model_definition", 0);
    value.paint_kit = j.value("paint_kit", 0);
    value.explicit_vanilla_finish = j.value("explicit_vanilla_finish", false);
    value.quality = j.value("quality", 0);
    value.seed = NormalizeCosmeticSeed(j.value("seed", 0));
    value.wear = NormalizeCosmeticWear(j.value("wear", kMinimumCosmeticWear));
    value.custom_name = j.value("custom_name", std::string());
}

static void to_json(json& j, const KeychainProfile& value)
{
    j = json {
        {"name", value.name},
        {"enabled", value.enabled},
        {"xuid", value.xuid},
        {"base_item", value.base_item},
        {"keychain_kit", value.keychain_kit},
        {"explicit_clear", value.explicit_clear},
    };
}

static void from_json(const json& j, KeychainProfile& value)
{
    value.name = j.value("name", std::string("Default Keychain"));
    value.enabled = j.value("enabled", false);
    value.xuid = j.value("xuid", static_cast<std::uint64_t>(0));
    value.base_item = j.value("base_item", 7);
    value.keychain_kit = j.value("keychain_kit", 0);
    value.explicit_clear = j.value("explicit_clear", false);
}

static void to_json(json& j, const MapRemapProfile& value)
{
    j = json {
        {"enabled", value.enabled},
        {"use_source_radar", value.use_source_radar},
        {"radar_black_tolerance", value.radar_black_tolerance},
        {"radar_dark_detail_radius", value.radar_dark_detail_radius},
        {"radar_background_alpha_percent", value.radar_background_alpha_percent},
        {"source_map_name", value.source_map_name},
        {"workshop_id", value.workshop_id},
        {"workshop_title", value.workshop_title},
        {"addon_name", value.addon_name},
        {"target_map_name", value.target_map_name},
        {"target_skybox_resource_prefix", value.target_skybox_resource_prefix},
        {"radar_overlay_request_virtual_path", value.radar_overlay_request_virtual_path},
        {"radar_overlay_virtual_path", value.radar_overlay_virtual_path},
        {"radar_overlay_root_path", value.radar_overlay_root_path},
        {"radar_overlay_file_path", value.radar_overlay_file_path},
    };
}

static void from_json(const json& j, MapRemapProfile& value)
{
    value.enabled = j.value("enabled", false);
    value.use_source_radar = j.value("use_source_radar", true);
    value.radar_black_tolerance = std::clamp(j.value("radar_black_tolerance", 12), 0, 96);
    value.radar_dark_detail_radius = std::clamp(j.value("radar_dark_detail_radius", 12), 0, 64);
    value.radar_background_alpha_percent =
        std::clamp(j.value("radar_background_alpha_percent", 0), 0, 100);
    value.source_map_name = j.value("source_map_name", std::string {});
    value.workshop_id = j.value("workshop_id", std::string {});
    value.workshop_title = j.value("workshop_title", std::string {});
    value.addon_name = j.value("addon_name", std::string {});
    value.target_map_name = j.value("target_map_name", std::string {});
    value.target_skybox_resource_prefix =
        j.value("target_skybox_resource_prefix", std::string {});
    value.radar_overlay_request_virtual_path =
        j.value("radar_overlay_request_virtual_path", std::string {});
    value.radar_overlay_virtual_path = j.value("radar_overlay_virtual_path", std::string {});
    value.radar_overlay_root_path = j.value("radar_overlay_root_path", std::string {});
    value.radar_overlay_file_path = j.value("radar_overlay_file_path", std::string {});
}

static void to_json(json& j, const ConfigDocument& value)
{
    j = json {
        {"version", 9},
        {"profiles", value.profiles},
        {"knife_profiles", value.knife_profiles},
        {"glove_profiles", value.glove_profiles},
        {"keychain_profiles", value.keychain_profiles},
        {"map_remap", value.map_remap},
        {"experimental_allow_hud_alias_synthetic_proxy_model_write", value.experimental_allow_hud_alias_synthetic_proxy_model_write},
    };
}

static void from_json(const json& j, ConfigDocument& value)
{
    value.version = j.value("version", 6);
    value.profiles = j.value("profiles", std::vector<CosmeticProfile> {});
    value.knife_profiles = j.value("knife_profiles", std::vector<KnifeProfile> {});
    value.glove_profiles = j.value("glove_profiles", std::vector<GloveProfile> {});
    value.keychain_profiles = j.value("keychain_profiles", std::vector<KeychainProfile> {});
    value.map_remap = j.value("map_remap", MapRemapProfile {});
    value.experimental_allow_hud_alias_synthetic_proxy_model_write =
        j.value("experimental_allow_hud_alias_synthetic_proxy_model_write", false);
}
}

nskinzcs2::model::ConfigDocument nskinzcs2::ConfigStore::CreateDefault()
{
    model::ConfigDocument document;
    document.profiles.push_back(model::CosmeticProfile {});
    document.knife_profiles.push_back(model::KnifeProfile {});
    document.glove_profiles.push_back(model::GloveProfile {});
    document.keychain_profiles.push_back(model::KeychainProfile {});
    return document;
}

nskinzcs2::model::ConfigDocument nskinzcs2::ConfigStore::Load(const std::filesystem::path& path)
{
    std::ifstream stream(path);
    if (!stream.good())
    {
        return CreateDefault();
    }

    try
    {
        const auto root = json::parse(stream);
        auto parsed = root.get<model::ConfigDocument>();
        const auto loaded_version = parsed.version;
        model::NormalizeKnifeProfiles(parsed, root);
        if (parsed.profiles.empty())
        {
            parsed.profiles.push_back(model::CosmeticProfile {});
        }

        if (parsed.knife_profiles.empty())
        {
            parsed.knife_profiles.push_back(model::KnifeProfile {});
        }

        if (parsed.glove_profiles.empty())
        {
            parsed.glove_profiles.push_back(model::GloveProfile {});
        }

        parsed.keychain_profiles =
            model::DeduplicateKeychainProfilesKeepLast(parsed.keychain_profiles);
        if (parsed.keychain_profiles.empty())
        {
            parsed.keychain_profiles.push_back(model::KeychainProfile {});
        }

        // v6 shipped with a high road-destructive cutoff and a recovery
        // radius that was silently clamped to six. Migrate only the exact old
        // defaults; explicit user tuning remains intact.
        if (loaded_version <= 6 &&
            parsed.map_remap.radar_black_tolerance == 24 &&
            parsed.map_remap.radar_dark_detail_radius == 4)
        {
            parsed.map_remap.radar_black_tolerance = 12;
            parsed.map_remap.radar_dark_detail_radius = 12;
        }
        if (loaded_version <= 6)
        {
            parsed.map_remap.radar_overlay_request_virtual_path.clear();
            parsed.map_remap.radar_overlay_virtual_path.clear();
            parsed.map_remap.radar_overlay_root_path.clear();
            parsed.map_remap.radar_overlay_file_path.clear();
        }
        if (loaded_version <= 7)
        {
            model::MigrateAbsoluteStickerOffsetsToDeltas(parsed);
        }
        else if (loaded_version == 8)
        {
            model::MigrateVersionEightFifthStickerOffsets(parsed);
        }
        parsed.version = 9;
        return parsed;
    }
    catch (const std::exception&)
    {
        return CreateDefault();
    }
}

bool nskinzcs2::ConfigStore::Save(const std::filesystem::path& path, const model::ConfigDocument& document)
{
    std::ofstream stream(path, std::ios::trunc);
    if (!stream.good())
    {
        return false;
    }

    auto normalized = document;
    normalized.version = 9;
    normalized.knife_profiles = model::DeduplicateProfilesByXuidKeepLast(normalized.knife_profiles);
    normalized.keychain_profiles =
        model::DeduplicateKeychainProfilesKeepLast(normalized.keychain_profiles);
    normalized.profiles.erase(
        std::remove_if(
            normalized.profiles.begin(),
            normalized.profiles.end(),
            [](const model::CosmeticProfile& profile)
            {
                return model::IsLegacyKnifeBaseItem(profile.base_item);
            }),
        normalized.profiles.end());

    stream << json(normalized).dump(2);
    return stream.good();
}

nskinzcs2::model::CosmeticProfile* nskinzcs2::ConfigStore::FindMatchingProfile(
    model::ConfigDocument& document,
    const std::uint64_t xuid,
    const int base_item)
{
    const auto iterator = std::find_if(
        document.profiles.begin(),
        document.profiles.end(),
        [xuid, base_item](const model::CosmeticProfile& profile)
        {
            return profile.enabled &&
                profile.base_item != 5028 &&
                !model::IsLegacyKnifeBaseItem(profile.base_item) &&
                profile.xuid == xuid &&
                profile.base_item == base_item;
        });

    return iterator == document.profiles.end() ? nullptr : &*iterator;
}

const nskinzcs2::model::CosmeticProfile* nskinzcs2::ConfigStore::FindMatchingProfile(
    const model::ConfigDocument& document,
    const std::uint64_t xuid,
    const int base_item)
{
    const auto iterator = std::find_if(
        document.profiles.begin(),
        document.profiles.end(),
        [xuid, base_item](const model::CosmeticProfile& profile)
        {
            return profile.enabled &&
                profile.base_item != 5028 &&
                !model::IsLegacyKnifeBaseItem(profile.base_item) &&
                profile.xuid == xuid &&
                profile.base_item == base_item;
        });

    return iterator == document.profiles.end() ? nullptr : &*iterator;
}

nskinzcs2::model::GloveProfile* nskinzcs2::ConfigStore::FindMatchingGloveProfile(
    model::ConfigDocument& document,
    const std::uint64_t xuid)
{
    const auto iterator = std::find_if(
        document.glove_profiles.begin(),
        document.glove_profiles.end(),
        [xuid](const model::GloveProfile& profile)
        {
            return profile.enabled && profile.xuid == xuid;
        });

    return iterator == document.glove_profiles.end() ? nullptr : &*iterator;
}

const nskinzcs2::model::GloveProfile* nskinzcs2::ConfigStore::FindMatchingGloveProfile(
    const model::ConfigDocument& document,
    const std::uint64_t xuid)
{
    const auto iterator = std::find_if(
        document.glove_profiles.begin(),
        document.glove_profiles.end(),
        [xuid](const model::GloveProfile& profile)
        {
            return profile.enabled && profile.xuid == xuid;
        });

    return iterator == document.glove_profiles.end() ? nullptr : &*iterator;
}

nskinzcs2::model::KnifeProfile* nskinzcs2::ConfigStore::FindMatchingKnifeProfile(
    model::ConfigDocument& document,
    const std::uint64_t xuid)
{
    const auto iterator = std::find_if(
        document.knife_profiles.rbegin(),
        document.knife_profiles.rend(),
        [xuid](const model::KnifeProfile& profile)
        {
            return profile.enabled && profile.xuid == xuid;
        });

    return iterator == document.knife_profiles.rend() ? nullptr : &*iterator;
}

nskinzcs2::model::KeychainProfile* nskinzcs2::ConfigStore::FindMatchingKeychainProfile(
    model::ConfigDocument& document,
    const std::uint64_t xuid,
    const int base_item)
{
    const auto iterator = std::find_if(
        document.keychain_profiles.rbegin(),
        document.keychain_profiles.rend(),
        [xuid, base_item](const model::KeychainProfile& profile)
        {
            return profile.enabled &&
                profile.xuid == xuid &&
                profile.base_item == base_item &&
                (profile.keychain_kit != 0 || profile.explicit_clear);
        });

    return iterator == document.keychain_profiles.rend() ? nullptr : &*iterator;
}

const nskinzcs2::model::KeychainProfile* nskinzcs2::ConfigStore::FindMatchingKeychainProfile(
    const model::ConfigDocument& document,
    const std::uint64_t xuid,
    const int base_item)
{
    const auto iterator = std::find_if(
        document.keychain_profiles.rbegin(),
        document.keychain_profiles.rend(),
        [xuid, base_item](const model::KeychainProfile& profile)
        {
            return profile.enabled &&
                profile.xuid == xuid &&
                profile.base_item == base_item &&
                (profile.keychain_kit != 0 || profile.explicit_clear);
        });

    return iterator == document.keychain_profiles.rend() ? nullptr : &*iterator;
}

const nskinzcs2::model::KnifeProfile* nskinzcs2::ConfigStore::FindMatchingKnifeProfile(
    const model::ConfigDocument& document,
    const std::uint64_t xuid)
{
    const auto iterator = std::find_if(
        document.knife_profiles.rbegin(),
        document.knife_profiles.rend(),
        [xuid](const model::KnifeProfile& profile)
        {
            return profile.enabled && profile.xuid == xuid;
        });

    return iterator == document.knife_profiles.rend() ? nullptr : &*iterator;
}
