#include "Core/Config.hpp"
#include "Core/Log.hpp"

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
}

static void to_json(json& j, const StickerProfile& value)
{
    j = json {
        {"kit_id", value.kit_id},
        {"wear", value.wear},
        {"scale", value.scale},
        {"rotation", value.rotation},
    };
}

static void from_json(const json& j, StickerProfile& value)
{
    value.kit_id = j.value("kit_id", 0);
    value.wear = j.value("wear", 0.0000001f);
    value.scale = j.value("scale", 1.0f);
    value.rotation = j.value("rotation", 0.0f);
}

static void to_json(json& j, const CosmeticProfile& value)
{
    j = json {
        {"name", value.name},
        {"enabled", value.enabled},
        {"xuid", value.xuid},
        {"base_item", value.base_item},
        {"override_item", value.override_item},
        {"paint_kit", value.paint_kit},
        {"quality", value.quality},
        {"seed", value.seed},
        {"stat_trak", value.stat_trak},
        {"wear", value.wear},
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
    value.quality = j.value("quality", 0);
    value.seed = j.value("seed", 0);
    value.stat_trak = j.value("stat_trak", 0);
    value.wear = j.value("wear", 0.0000001f);
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
        {"quality", value.quality},
        {"seed", value.seed},
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
    value.quality = j.value("quality", 0);
    value.seed = j.value("seed", 0);
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
        {"quality", value.quality},
        {"seed", value.seed},
        {"wear", value.wear},
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
    value.quality = j.value("quality", 0);
    value.seed = j.value("seed", 0);
    value.wear = j.value("wear", 0.0000001f);
}

static void to_json(json& j, const ConfigDocument& value)
{
    j = json {
        {"version", 3},
        {"profiles", value.profiles},
        {"knife_profiles", value.knife_profiles},
        {"glove_profiles", value.glove_profiles},
        {"experimental_allow_hud_alias_synthetic_proxy_model_write", value.experimental_allow_hud_alias_synthetic_proxy_model_write},
    };
}

static void from_json(const json& j, ConfigDocument& value)
{
    value.version = j.value("version", 3);
    value.profiles = j.value("profiles", std::vector<CosmeticProfile> {});
    value.knife_profiles = j.value("knife_profiles", std::vector<KnifeProfile> {});
    value.glove_profiles = j.value("glove_profiles", std::vector<GloveProfile> {});
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

        parsed.version = 3;
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
    normalized.version = 3;
    normalized.knife_profiles = model::DeduplicateProfilesByXuidKeepLast(normalized.knife_profiles);
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
