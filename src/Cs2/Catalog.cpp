#include "Cs2/Catalog.hpp"
#include "Core/BuildConfig.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
using nskinzcs2::model::Catalog;
using nskinzcs2::model::NamedId;
using nskinzcs2::model::VisualInfo;

struct IdNameSeed
{
    int id;
    const char* name;
};

struct VisualSeed
{
    int id;
    const char* name;
    const char* world_model;
    const char* view_model;
    const char* killfeed_icon;
    bool is_knife;
    bool is_glove;
};

std::string NormalizeCatalogModelPath(std::string value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](const unsigned char ch)
        {
            return static_cast<char>(std::tolower(ch));
        });
    std::replace(value.begin(), value.end(), '\\', '/');
    return value;
}

bool EndsWithInsensitive(const std::string_view value, const std::string_view suffix) noexcept
{
    if (suffix.size() > value.size())
    {
        return false;
    }

    return std::equal(
        suffix.rbegin(),
        suffix.rend(),
        value.rbegin(),
        [](const char lhs, const char rhs)
        {
            return std::tolower(static_cast<unsigned char>(lhs)) ==
                std::tolower(static_cast<unsigned char>(rhs));
        });
}

std::string BuildCatalogModelResourcePath(const std::string_view model_path)
{
    auto normalized = NormalizeCatalogModelPath(std::string {model_path});
    if (normalized.empty())
    {
        return {};
    }

    if (EndsWithInsensitive(normalized, ".vmdl_c"))
    {
        return normalized;
    }

    if (EndsWithInsensitive(normalized, ".vmdl"))
    {
        normalized.append("_c");
        return normalized;
    }

    if (EndsWithInsensitive(normalized, ".mdl"))
    {
        normalized.erase(normalized.size() - 4);
    }

    normalized.append(".vmdl_c");
    return normalized;
}

std::string BuildCatalogKnifeFamily(const std::string_view model_path)
{
    auto normalized = NormalizeCatalogModelPath(std::string {model_path});
    if (normalized.empty())
    {
        return {};
    }

    const auto separator = normalized.find_last_of('/');
    auto family =
        separator == std::string::npos
            ? normalized
            : normalized.substr(separator + 1);

    if (EndsWithInsensitive(family, ".vmdl_c"))
    {
        family.erase(family.size() - 7);
    }
    else if (EndsWithInsensitive(family, ".vmdl"))
    {
        family.erase(family.size() - 5);
    }
    else if (EndsWithInsensitive(family, ".mdl"))
    {
        family.erase(family.size() - 4);
    }

    if (family.size() > 2 &&
        (family[0] == 'v' || family[0] == 'w') &&
        family[1] == '_')
    {
        family.erase(0, 2);
    }

    return family;
}

void FinalizeVisualInfo(VisualInfo& visual)
{
    visual.resolved_world_model_resource_path =
        BuildCatalogModelResourcePath(visual.world_model);
    visual.resolved_view_model_resource_path =
        BuildCatalogModelResourcePath(visual.view_model);

    if (!visual.is_knife)
    {
        visual.knife_family.clear();
        visual.knife_identity_resolved = false;
        return;
    }

    const auto& preferred_model_path =
        !visual.view_model.empty()
            ? visual.view_model
            : visual.world_model;
    visual.knife_family = BuildCatalogKnifeFamily(preferred_model_path);
    visual.knife_identity_resolved =
        !visual.knife_family.empty() ||
        !visual.resolved_world_model_resource_path.empty() ||
        !visual.resolved_view_model_resource_path.empty();
}

void FinalizeKeychainVisualInfo(nskinzcs2::model::KeychainVisualInfo& visual)
{
    visual.model_path = NormalizeCatalogModelPath(std::move(visual.model_path));
    visual.resolved_model_resource_path =
        BuildCatalogModelResourcePath(visual.model_path);
}

constexpr std::array<IdNameSeed, 36> kBaseItems = {
    IdNameSeed {42, "Knife"},
    IdNameSeed {5028, "Glove"},
    IdNameSeed {7, "AK-47"},
    IdNameSeed {8, "AUG"},
    IdNameSeed {9, "AWP"},
    IdNameSeed {63, "CZ75-Auto"},
    IdNameSeed {1, "Desert Eagle"},
    IdNameSeed {2, "Dual Berettas"},
    IdNameSeed {10, "FAMAS"},
    IdNameSeed {3, "Five-SeveN"},
    IdNameSeed {11, "G3SG1"},
    IdNameSeed {13, "Galil AR"},
    IdNameSeed {4, "Glock-18"},
    IdNameSeed {14, "M249"},
    IdNameSeed {60, "M4A1-S"},
    IdNameSeed {16, "M4A4"},
    IdNameSeed {17, "MAC-10"},
    IdNameSeed {27, "MAG-7"},
    IdNameSeed {23, "MP5-SD"},
    IdNameSeed {33, "MP7"},
    IdNameSeed {34, "MP9"},
    IdNameSeed {28, "Negev"},
    IdNameSeed {35, "Nova"},
    IdNameSeed {32, "P2000"},
    IdNameSeed {36, "P250"},
    IdNameSeed {19, "P90"},
    IdNameSeed {26, "PP-Bizon"},
    IdNameSeed {64, "R8 Revolver"},
    IdNameSeed {29, "Sawed-Off"},
    IdNameSeed {38, "SCAR-20"},
    IdNameSeed {39, "SG 553"},
    IdNameSeed {40, "SSG 08"},
    IdNameSeed {30, "Tec-9"},
    IdNameSeed {24, "UMP-45"},
    IdNameSeed {61, "USP-S"},
    IdNameSeed {25, "XM1014"},
};

constexpr std::array<IdNameSeed, 20> kKnifeItems = {
    IdNameSeed {0, "Default"},
    IdNameSeed {500, "Bayonet"},
    IdNameSeed {503, "Classic Knife"},
    IdNameSeed {505, "Flip Knife"},
    IdNameSeed {506, "Gut Knife"},
    IdNameSeed {507, "Karambit"},
    IdNameSeed {508, "M9 Bayonet"},
    IdNameSeed {509, "Huntsman Knife"},
    IdNameSeed {512, "Falchion Knife"},
    IdNameSeed {514, "Bowie Knife"},
    IdNameSeed {515, "Butterfly Knife"},
    IdNameSeed {516, "Shadow Daggers"},
    IdNameSeed {517, "Paracord Knife"},
    IdNameSeed {518, "Survival Knife"},
    IdNameSeed {519, "Ursus Knife"},
    IdNameSeed {520, "Navaja Knife"},
    IdNameSeed {521, "Nomad Knife"},
    IdNameSeed {522, "Stiletto Knife"},
    IdNameSeed {523, "Talon Knife"},
    IdNameSeed {525, "Skeleton Knife"},
};

constexpr std::array<IdNameSeed, 11> kGloveItems = {
    IdNameSeed {0, "Default"},
    IdNameSeed {5027, "Bloodhound"},
    IdNameSeed {5028, "Default (Terrorists)"},
    IdNameSeed {5029, "Default (Counter-Terrorists)"},
    IdNameSeed {5030, "Sport"},
    IdNameSeed {5031, "Driver"},
    IdNameSeed {5032, "Hand Wraps"},
    IdNameSeed {5033, "Moto"},
    IdNameSeed {5034, "Specialist"},
    IdNameSeed {5035, "Hydra"},
    IdNameSeed {4725, "Broken Fang"},
};

struct PaintCompatibilitySeed
{
    int paint_kit_id;
    int glove_item_id;
};

constexpr std::array<IdNameSeed, 11> kWeaponPaintKits = {
    IdNameSeed {0, "Default"},
    IdNameSeed {12, "Crimson Web"},
    IdNameSeed {38, "Fade"},
    IdNameSeed {44, "Case Hardened"},
    IdNameSeed {180, "Fire Serpent"},
    IdNameSeed {344, "Dragon Lore"},
    IdNameSeed {455, "Asiimov"},
    IdNameSeed {600, "Neo-Noir"},
    IdNameSeed {711, "Printstream"},
    IdNameSeed {838, "Doppler"},
    IdNameSeed {1146, "Gamma Doppler"},
};

constexpr std::array<IdNameSeed, 6> kGlovePaintKits = {
    IdNameSeed {0, "Default"},
    IdNameSeed {10006, "Crimson Kimono"},
    IdNameSeed {10018, "Pandora's Box"},
    IdNameSeed {10024, "Vice"},
    IdNameSeed {10037, "Omega"},
    IdNameSeed {10044, "Nocts"},
};

constexpr std::array<PaintCompatibilitySeed, 6> kGlovePaintCompatibilitySeeds = {{
    {0, 0},
    {10006, 5027},
    {10018, 5030},
    {10024, 5033},
    {10037, 5030},
    {10044, 5031},
}};

constexpr std::array<IdNameSeed, 8> kStickerKits = {
    IdNameSeed {0,
#ifdef NSKINZCS2_UI_LANG_ZH_CN
        "\xE6\x97\xA0"
#else
        "None"
#endif
    },
    IdNameSeed {3, "Crown (Foil)"},
    IdNameSeed {4, "Howling Dawn"},
    IdNameSeed {188, "Ninjas in Pyjamas (Holo) Cologne 2014"},
    IdNameSeed {6001, "Team Dignitas (Holo) Cologne 2014"},
    IdNameSeed {6237, "Battle Scarred (Holo)"},
    IdNameSeed {6293, "The Lurker"},
    IdNameSeed {6331, "Miami Tier6"},
};

constexpr std::array<IdNameSeed, 11> kQualities = {
    IdNameSeed {0, "Default"},
    IdNameSeed {1, "Genuine"},
    IdNameSeed {2, "Vintage"},
    IdNameSeed {3, "Unusual"},
    IdNameSeed {5, "Community"},
    IdNameSeed {6, "Developer"},
    IdNameSeed {7, "Self-Made"},
    IdNameSeed {8, "Customized"},
    IdNameSeed {9, "Strange"},
    IdNameSeed {10, "Completed"},
    IdNameSeed {12, "Tournament"},
};

constexpr std::array<IdNameSeed, 1> kKeychainKits = {
    IdNameSeed {0,
#ifdef NSKINZCS2_UI_LANG_ZH_CN
        "\xE6\x97\xA0"
#else
        "None"
#endif
    },
};

constexpr std::array<VisualSeed, 31> kVisualSeeds = {{
    {42, "Knife", "models/weapons/w_knife_default_ct.mdl", "models/weapons/v_knife_default_ct.mdl", "knife_default_ct", true, false},
    {59, "Knife (T)", "models/weapons/w_knife_default_t.mdl", "models/weapons/v_knife_default_t.mdl", "knife_t", true, false},
    {500, "Bayonet", "models/weapons/w_knife_bayonet.mdl", "models/weapons/v_knife_bayonet.mdl", "bayonet", true, false},
    {503, "Classic Knife", "models/weapons/w_knife_css.mdl", "models/weapons/v_knife_css.mdl", "knife_css", true, false},
    {505, "Flip Knife", "models/weapons/w_knife_flip.mdl", "models/weapons/v_knife_flip.mdl", "knife_flip", true, false},
    {506, "Gut Knife", "models/weapons/w_knife_gut.mdl", "models/weapons/v_knife_gut.mdl", "knife_gut", true, false},
    {507, "Karambit", "models/weapons/w_knife_karam.mdl", "models/weapons/v_knife_karam.mdl", "knife_karambit", true, false},
    {508, "M9 Bayonet", "models/weapons/w_knife_m9_bay.mdl", "models/weapons/v_knife_m9_bay.mdl", "knife_m9_bayonet", true, false},
    {509, "Huntsman Knife", "models/weapons/w_knife_tactical.mdl", "models/weapons/v_knife_tactical.mdl", "knife_tactical", true, false},
    {512, "Falchion Knife", "models/weapons/w_knife_falchion_advanced.mdl", "models/weapons/v_knife_falchion_advanced.mdl", "knife_falchion", true, false},
    {514, "Bowie Knife", "models/weapons/w_knife_survival_bowie.mdl", "models/weapons/v_knife_survival_bowie.mdl", "knife_survival_bowie", true, false},
    {515, "Butterfly Knife", "models/weapons/w_knife_butterfly.mdl", "models/weapons/v_knife_butterfly.mdl", "knife_butterfly", true, false},
    {516, "Shadow Daggers", "models/weapons/w_knife_push.mdl", "models/weapons/v_knife_push.mdl", "knife_push", true, false},
    {517, "Paracord Knife", "models/weapons/w_knife_cord.mdl", "models/weapons/v_knife_cord.mdl", "knife_cord", true, false},
    {518, "Survival Knife", "models/weapons/w_knife_canis.mdl", "models/weapons/v_knife_canis.mdl", "knife_canis", true, false},
    {519, "Ursus Knife", "models/weapons/w_knife_ursus.mdl", "models/weapons/v_knife_ursus.mdl", "knife_ursus", true, false},
    {520, "Navaja Knife", "models/weapons/w_knife_gypsy_jackknife.mdl", "models/weapons/v_knife_gypsy_jackknife.mdl", "knife_gypsy_jackknife", true, false},
    {521, "Nomad Knife", "models/weapons/w_knife_outdoor.mdl", "models/weapons/v_knife_outdoor.mdl", "knife_outdoor", true, false},
    {522, "Stiletto Knife", "models/weapons/w_knife_stiletto.mdl", "models/weapons/v_knife_stiletto.mdl", "knife_stiletto", true, false},
    {523, "Talon Knife", "models/weapons/w_knife_widowmaker.mdl", "models/weapons/v_knife_widowmaker.mdl", "knife_widowmaker", true, false},
    {525, "Skeleton Knife", "models/weapons/w_knife_skeleton.mdl", "models/weapons/v_knife_skeleton.mdl", "knife_skeleton", true, false},
    {5027, "Bloodhound", "models/weapons/w_models/arms/w_glove_bloodhound.mdl", "models/weapons/v_models/arms/glove_bloodhound/v_glove_bloodhound.mdl", "", false, true},
    {5028, "Default (Terrorists)", "models/weapons/w_models/arms/w_glove_fingerless.mdl", "models/weapons/v_models/arms/glove_fingerless/v_glove_fingerless.mdl", "", false, true},
    {5029, "Default (Counter-Terrorists)", "models/weapons/w_models/arms/w_glove_hardknuckle.mdl", "models/weapons/v_models/arms/glove_hardknuckle/v_glove_hardknuckle.mdl", "", false, true},
    {5030, "Sport", "models/weapons/w_models/arms/w_glove_sporty.mdl", "models/weapons/v_models/arms/glove_sporty/v_glove_sporty.mdl", "", false, true},
    {5031, "Driver", "models/weapons/w_models/arms/w_glove_slick.mdl", "models/weapons/v_models/arms/glove_slick/v_glove_slick.mdl", "", false, true},
    {5032, "Hand Wraps", "models/weapons/w_models/arms/w_glove_handwrap_leathery.mdl", "models/weapons/v_models/arms/glove_handwrap_leathery/v_glove_handwrap_leathery.mdl", "", false, true},
    {5033, "Moto", "models/weapons/w_models/arms/w_glove_motorcycle.mdl", "models/weapons/v_models/arms/glove_motorcycle/v_glove_motorcycle.mdl", "", false, true},
    {5034, "Specialist", "models/weapons/w_models/arms/w_glove_specialist.mdl", "models/weapons/v_models/arms/glove_specialist/v_glove_specialist.mdl", "", false, true},
    {5035, "Hydra", "models/weapons/w_models/arms/w_glove_bloodhound_hydra.mdl", "models/weapons/v_models/arms/glove_bloodhound_hydra/v_glove_bloodhound_hydra.mdl", "", false, true},
    {4725, "Broken Fang", "models/weapons/w_models/arms/w_glove_bloodhound_brokenfang.mdl", "models/weapons/v_models/arms/glove_bloodhound/v_glove_bloodhound_brokenfang.mdl", "", false, true},
}};

template <std::size_t N>
std::vector<NamedId> CopyArray(const std::array<IdNameSeed, N>& values)
{
    std::vector<NamedId> result;
    result.reserve(values.size());
    for (const auto& value : values)
    {
        result.push_back(NamedId {value.id, value.name});
    }

    return result;
}

struct KeyValueNode
{
    std::string key;
    std::string value;
    std::vector<KeyValueNode> children;

    [[nodiscard]] const KeyValueNode* FindChild(const std::string_view child_key) const noexcept
    {
        const auto iterator = std::find_if(
            children.begin(),
            children.end(),
            [child_key](const KeyValueNode& child)
            {
                return child.key == child_key;
            });
        return iterator == children.end() ? nullptr : &*iterator;
    }

    [[nodiscard]] std::string_view FindValue(const std::string_view child_key) const noexcept
    {
        const auto* child = FindChild(child_key);
        if (child == nullptr || !child->children.empty())
        {
            return {};
        }

        return child->value;
    }
};

class KeyValueTokenizer
{
public:
    enum class TokenKind
    {
        String,
        OpenBrace,
        CloseBrace,
        End,
    };

    struct Token
    {
        TokenKind kind = TokenKind::End;
        std::string text;
    };

    explicit KeyValueTokenizer(const std::string_view text)
        : text_(text)
    {
    }

    [[nodiscard]] Token Next()
    {
        while (true)
        {
            SkipIgnored();
            if (position_ >= text_.size())
            {
                return {};
            }

            const auto current = text_[position_];
            if (current == '{')
            {
                ++position_;
                return {TokenKind::OpenBrace, {}};
            }

            if (current == '}')
            {
                ++position_;
                return {TokenKind::CloseBrace, {}};
            }

            if (current == '"')
            {
                return {TokenKind::String, ReadQuotedString()};
            }

            auto text = ReadBareString();
            if (text.size() >= 3 && text.front() == '[' && text[1] == '$' && text.back() == ']')
            {
                continue;
            }

            return {TokenKind::String, std::move(text)};
        }
    }

private:
    void SkipIgnored()
    {
        while (position_ < text_.size())
        {
            const auto current = text_[position_];
            if (std::isspace(static_cast<unsigned char>(current)) != 0)
            {
                ++position_;
                continue;
            }

            if (current == '/' && position_ + 1 < text_.size() && text_[position_ + 1] == '/')
            {
                position_ += 2;
                while (position_ < text_.size() && text_[position_] != '\n')
                {
                    ++position_;
                }
                continue;
            }

            break;
        }
    }

    [[nodiscard]] std::string ReadQuotedString()
    {
        std::string value;
        ++position_;
        while (position_ < text_.size())
        {
            const auto current = text_[position_++];
            if (current == '"')
            {
                break;
            }

            if (current == '\\' && position_ < text_.size())
            {
                value.push_back(text_[position_++]);
                continue;
            }

            value.push_back(current);
        }

        return value;
    }

    [[nodiscard]] std::string ReadBareString()
    {
        const auto start = position_;
        while (position_ < text_.size())
        {
            const auto current = text_[position_];
            if (std::isspace(static_cast<unsigned char>(current)) != 0 || current == '{' || current == '}')
            {
                break;
            }

            ++position_;
        }

        return std::string(text_.substr(start, position_ - start));
    }

    std::string_view text_;
    std::size_t position_ = 0;
};

std::optional<KeyValueNode> ParseKeyValueBlock(KeyValueTokenizer& tokenizer, std::string key)
{
    KeyValueNode node;
    node.key = std::move(key);

    while (true)
    {
        auto token = tokenizer.Next();
        if (token.kind == KeyValueTokenizer::TokenKind::End)
        {
            return std::nullopt;
        }

        if (token.kind == KeyValueTokenizer::TokenKind::CloseBrace)
        {
            break;
        }

        if (token.kind != KeyValueTokenizer::TokenKind::String)
        {
            return std::nullopt;
        }

        auto value_or_block = tokenizer.Next();
        if (value_or_block.kind == KeyValueTokenizer::TokenKind::OpenBrace)
        {
            auto child = ParseKeyValueBlock(tokenizer, std::move(token.text));
            if (!child.has_value())
            {
                return std::nullopt;
            }

            node.children.push_back(std::move(*child));
            continue;
        }

        if (value_or_block.kind != KeyValueTokenizer::TokenKind::String)
        {
            return std::nullopt;
        }

        node.children.push_back(KeyValueNode {
            std::move(token.text),
            std::move(value_or_block.text),
            {},
        });
    }

    return node;
}

std::optional<KeyValueNode> ParseKeyValueText(const std::string_view text)
{
    KeyValueTokenizer tokenizer(text);
    auto root_key = tokenizer.Next();
    if (root_key.kind != KeyValueTokenizer::TokenKind::String)
    {
        return std::nullopt;
    }

    auto open_brace = tokenizer.Next();
    if (open_brace.kind != KeyValueTokenizer::TokenKind::OpenBrace)
    {
        return std::nullopt;
    }

    return ParseKeyValueBlock(tokenizer, std::move(root_key.text));
}

std::optional<std::string> ReadTextFile(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream.good())
    {
        return std::nullopt;
    }

    std::string text {
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()
    };
    if (text.size() >= 2)
    {
        const auto first = static_cast<unsigned char>(text[0]);
        const auto second = static_cast<unsigned char>(text[1]);
        if ((first == 0xFF && second == 0xFE) || (first == 0xFE && second == 0xFF))
        {
            if (((text.size() - 2) % 2) != 0)
            {
                return std::nullopt;
            }

            const bool little_endian = first == 0xFF;
            auto read_code_unit = [&](const std::size_t index) noexcept
            {
                const auto low = static_cast<unsigned char>(text[index]);
                const auto high = static_cast<unsigned char>(text[index + 1]);
                return little_endian
                    ? static_cast<char16_t>(low | (high << 8))
                    : static_cast<char16_t>(high | (low << 8));
            };

            auto append_utf8 = [](std::string& target, const char32_t codepoint)
            {
                if (codepoint <= 0x7F)
                {
                    target.push_back(static_cast<char>(codepoint));
                    return;
                }

                if (codepoint <= 0x7FF)
                {
                    target.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
                    target.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
                    return;
                }

                if (codepoint <= 0xFFFF)
                {
                    target.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
                    target.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
                    target.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
                    return;
                }

                target.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
                target.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
                target.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
                target.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
            };

            std::string utf8_text;
            utf8_text.reserve(text.size());
            for (std::size_t index = 2; index + 1 < text.size(); index += 2)
            {
                const auto first_unit = read_code_unit(index);
                char32_t codepoint = first_unit;
                if (first_unit >= 0xD800 && first_unit <= 0xDBFF)
                {
                    if (index + 3 < text.size())
                    {
                        const auto second_unit = read_code_unit(index + 2);
                        if (second_unit >= 0xDC00 && second_unit <= 0xDFFF)
                        {
                            codepoint =
                                0x10000u +
                                ((static_cast<char32_t>(first_unit) - 0xD800u) << 10u) +
                                (static_cast<char32_t>(second_unit) - 0xDC00u);
                            index += 2;
                        }
                        else
                        {
                            codepoint = 0xFFFD;
                        }
                    }
                    else
                    {
                        codepoint = 0xFFFD;
                    }
                }
                else if (first_unit >= 0xDC00 && first_unit <= 0xDFFF)
                {
                    codepoint = 0xFFFD;
                }

                append_utf8(utf8_text, codepoint);
            }

            return utf8_text;
        }
    }

    if (text.size() >= 3 &&
        static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF)
    {
        text.erase(0, 3);
    }

    return text;
}

std::size_t SkipTrivia(const std::string_view text, std::size_t position)
{
    while (position < text.size())
    {
        const auto current = text[position];
        if (std::isspace(static_cast<unsigned char>(current)) != 0)
        {
            ++position;
            continue;
        }

        if (current == '/' && position + 1 < text.size() && text[position + 1] == '/')
        {
            position += 2;
            while (position < text.size() && text[position] != '\n')
            {
                ++position;
            }
            continue;
        }

        if (current == '[' && position + 2 < text.size() && text[position + 1] == '$')
        {
            while (position < text.size() && text[position] != ']')
            {
                ++position;
            }
            if (position < text.size())
            {
                ++position;
            }
            continue;
        }

        break;
    }

    return position;
}

std::size_t FindMatchingBrace(const std::string_view text, const std::size_t open_brace_position)
{
    int depth = 0;
    bool in_string = false;

    for (std::size_t position = open_brace_position; position < text.size(); ++position)
    {
        const auto current = text[position];
        if (in_string)
        {
            if (current == '\\' && position + 1 < text.size())
            {
                ++position;
                continue;
            }

            if (current == '"')
            {
                in_string = false;
            }
            continue;
        }

        if (current == '/' && position + 1 < text.size() && text[position + 1] == '/')
        {
            ++position;
            while (position < text.size() && text[position] != '\n')
            {
                ++position;
            }
            continue;
        }

        if (current == '"')
        {
            in_string = true;
            continue;
        }

        if (current == '{')
        {
            ++depth;
            continue;
        }

        if (current == '}')
        {
            --depth;
            if (depth == 0)
            {
                return position;
            }
        }
    }

    return std::string_view::npos;
}

std::optional<std::string> ExtractNamedSectionText(const std::string_view text, const std::string_view section_name)
{
    const std::string needle = "\"" + std::string(section_name) + "\"";
    std::size_t search_position = 0;
    while (true)
    {
        const auto key_position = text.find(needle, search_position);
        if (key_position == std::string_view::npos)
        {
            return std::nullopt;
        }

        auto brace_position = SkipTrivia(text, key_position + needle.size());
        if (brace_position >= text.size() || text[brace_position] != '{')
        {
            search_position = key_position + needle.size();
            continue;
        }

        const auto end_position = FindMatchingBrace(text, brace_position);
        if (end_position == std::string_view::npos)
        {
            return std::nullopt;
        }

        return std::string(text.substr(key_position, end_position - key_position + 1));
    }
}

std::vector<std::string> ExtractNamedSectionTexts(const std::string_view text, const std::string_view section_name)
{
    std::vector<std::string> sections;
    const std::string needle = "\"" + std::string(section_name) + "\"";
    std::size_t search_position = 0;
    while (true)
    {
        const auto key_position = text.find(needle, search_position);
        if (key_position == std::string_view::npos)
        {
            break;
        }

        auto brace_position = SkipTrivia(text, key_position + needle.size());
        if (brace_position >= text.size() || text[brace_position] != '{')
        {
            search_position = key_position + needle.size();
            continue;
        }

        const auto end_position = FindMatchingBrace(text, brace_position);
        if (end_position == std::string_view::npos)
        {
            break;
        }

        sections.emplace_back(text.substr(key_position, end_position - key_position + 1));
        search_position = end_position + 1;
    }

    return sections;
}

bool StartsWith(const std::string_view value, const std::string_view prefix) noexcept;

std::string_view TrimView(std::string_view value) noexcept
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0)
    {
        value.remove_prefix(1);
    }

    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0)
    {
        value.remove_suffix(1);
    }

    return value;
}

std::vector<std::string> ExtractQuotedStringsFromLine(const std::string_view line)
{
    std::vector<std::string> values;
    for (std::size_t index = 0; index < line.size(); ++index)
    {
        if (line[index] != '"')
        {
            continue;
        }

        ++index;
        std::string value;
        while (index < line.size())
        {
            const auto current = line[index++];
            if (current == '\\' && index < line.size())
            {
                value.push_back(line[index++]);
                continue;
            }

            if (current == '"')
            {
                break;
            }

            value.push_back(current);
        }

        values.push_back(std::move(value));
    }

    return values;
}

void CountBracesOutsideQuotes(const std::string_view line, int& open_braces, int& close_braces) noexcept
{
    open_braces = 0;
    close_braces = 0;
    bool in_string = false;
    for (std::size_t index = 0; index < line.size(); ++index)
    {
        const auto current = line[index];
        if (in_string)
        {
            if (current == '\\' && index + 1 < line.size())
            {
                ++index;
                continue;
            }

            if (current == '"')
            {
                in_string = false;
            }
            continue;
        }

        if (current == '"')
        {
            in_string = true;
            continue;
        }

        if (current == '/' && index + 1 < line.size() && line[index + 1] == '/')
        {
            break;
        }

        if (current == '{')
        {
            ++open_braces;
        }
        else if (current == '}')
        {
            ++close_braces;
        }
    }
}

bool LineMatchesCurrentPlatform(const std::string_view line) noexcept
{
    const auto marker = line.find("[$");
    if (marker == std::string_view::npos)
    {
        return true;
    }

    return line.find("[$WIN32]", marker) != std::string_view::npos;
}

using FlatSectionEntries = std::unordered_map<std::string, std::unordered_map<std::string, std::string>>;
using FlatSectionEntry = FlatSectionEntries::mapped_type;

std::string_view FindFlatValue(const FlatSectionEntry& entry, const std::string_view key) noexcept
{
    if (const auto iterator = entry.find(std::string(key)); iterator != entry.end())
    {
        return iterator->second;
    }

    return {};
}

FlatSectionEntries ParseFlatSectionEntries(const std::string_view section_text)
{
    FlatSectionEntries entries;
    int depth = 0;
    std::string pending_entry_key;
    std::string current_entry_key;
    std::size_t line_start = 0;

    while (line_start <= section_text.size())
    {
        auto line_end = section_text.find('\n', line_start);
        if (line_end == std::string_view::npos)
        {
            line_end = section_text.size();
        }

        auto line = TrimView(section_text.substr(line_start, line_end - line_start));
        if (!line.empty() && !StartsWith(line, "//"))
        {
            const auto quoted_values = ExtractQuotedStringsFromLine(line);
            if (depth == 1 && quoted_values.size() == 1)
            {
                pending_entry_key = quoted_values.front();
            }
            else if (depth == 2 && !current_entry_key.empty() && quoted_values.size() >= 2)
            {
                entries[current_entry_key][quoted_values[0]] = quoted_values[1];
            }

            int open_braces = 0;
            int close_braces = 0;
            CountBracesOutsideQuotes(line, open_braces, close_braces);

            if (depth == 1 && !pending_entry_key.empty() && open_braces > 0)
            {
                current_entry_key = pending_entry_key;
                entries.try_emplace(current_entry_key);
                pending_entry_key.clear();
            }

            depth += open_braces;
            depth -= close_braces;

            if (!current_entry_key.empty() && depth < 2)
            {
                current_entry_key.clear();
            }
        }

        if (line_end == section_text.size())
        {
            break;
        }

        line_start = line_end + 1;
    }

    return entries;
}

FlatSectionEntries MergeFlatSectionEntries(const std::vector<std::string>& section_texts)
{
    FlatSectionEntries merged;
    for (const auto& section_text : section_texts)
    {
        const auto parsed = ParseFlatSectionEntries(section_text);
        for (const auto& [entry_key, entry_values] : parsed)
        {
            auto& merged_entry = merged[entry_key];
            for (const auto& [field_key, field_value] : entry_values)
            {
                merged_entry[field_key] = field_value;
            }
        }
    }

    return merged;
}

std::unordered_map<std::string, std::string> ParseLocalizationTokensSection(const std::string_view section_text)
{
    std::unordered_map<std::string, std::string> localization;
    int depth = 0;
    std::size_t line_start = 0;

    while (line_start <= section_text.size())
    {
        auto line_end = section_text.find('\n', line_start);
        if (line_end == std::string_view::npos)
        {
            line_end = section_text.size();
        }

        auto line = TrimView(section_text.substr(line_start, line_end - line_start));
        if (!line.empty() && !StartsWith(line, "//"))
        {
            const auto quoted_values = ExtractQuotedStringsFromLine(line);
            if (depth == 1 && quoted_values.size() >= 2 && LineMatchesCurrentPlatform(line))
            {
                localization[quoted_values[0]] = quoted_values[1];
            }

            int open_braces = 0;
            int close_braces = 0;
            CountBracesOutsideQuotes(line, open_braces, close_braces);
            depth += open_braces;
            depth -= close_braces;
        }

        if (line_end == section_text.size())
        {
            break;
        }

        line_start = line_end + 1;
    }

    return localization;
}

bool TryParseInt(const std::string_view value, int& out)
{
    out = 0;
    if (value.empty())
    {
        return false;
    }

    const auto* begin = value.data();
    const auto* end = begin + value.size();
    const auto result = std::from_chars(begin, end, out);
    return result.ec == std::errc() && result.ptr == end;
}

bool StartsWith(const std::string_view value, const std::string_view prefix) noexcept
{
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

int ResolveCompatibleGloveDefinitionIndex(const std::string_view paint_kit_name) noexcept
{
    if (paint_kit_name.empty())
    {
        return 0;
    }

    if (StartsWith(paint_kit_name, "bloodhound_hydra_"))
    {
        return 5035;
    }

    if (StartsWith(paint_kit_name, "bloodhound_"))
    {
        return 5027;
    }

    if (StartsWith(paint_kit_name, "operation10_"))
    {
        return 4725;
    }

    if (StartsWith(paint_kit_name, "sporty_"))
    {
        return 5030;
    }

    if (StartsWith(paint_kit_name, "slick_"))
    {
        return 5031;
    }

    if (StartsWith(paint_kit_name, "handwrap_"))
    {
        return 5032;
    }

    if (StartsWith(paint_kit_name, "motorcycle_"))
    {
        return 5033;
    }

    if (StartsWith(paint_kit_name, "specialist_"))
    {
        return 5034;
    }

    return 0;
}

bool IsConditionalToken(const std::string_view value) noexcept
{
    return value.size() >= 3 && value.front() == '[' && value[1] == '$' && value.back() == ']';
}

std::string Lowercase(const std::string_view value)
{
    std::string lower(value);
    std::transform(
        lower.begin(),
        lower.end(),
        lower.begin(),
        [](unsigned char ch)
        {
            return static_cast<char>(std::tolower(ch));
        });
    return lower;
}

std::string HumanizeIdentifier(std::string_view value)
{
    while (!value.empty() && value.front() == '#')
    {
        value.remove_prefix(1);
    }

    if (StartsWith(value, "weapon_"))
    {
        value.remove_prefix(7);
    }

    std::string result;
    result.reserve(value.size());

    bool capitalize_next = true;
    for (const auto ch : value)
    {
        if (ch == '_' || ch == '-')
        {
            if (!result.empty() && result.back() != ' ')
            {
                result.push_back(' ');
            }
            capitalize_next = true;
            continue;
        }

        if (capitalize_next && std::isalpha(static_cast<unsigned char>(ch)) != 0)
        {
            result.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
            capitalize_next = false;
            continue;
        }

        result.push_back(static_cast<char>(std::isalpha(static_cast<unsigned char>(ch)) != 0
                ? std::tolower(static_cast<unsigned char>(ch))
                : ch));
        capitalize_next = false;
    }

    return result;
}

std::string ResolveLocalizedText(
    std::string_view token,
    const std::unordered_map<std::string, std::string>& localization)
{
    if (!token.empty())
    {
        const auto normalized = token.front() == '#' ? token.substr(1) : token;
        if (const auto iterator = localization.find(std::string(normalized)); iterator != localization.end())
        {
            return iterator->second;
        }
    }

    if (!token.empty() && token.front() == '#')
    {
        token.remove_prefix(1);
    }

    return HumanizeIdentifier(token);
}

std::string TryResolveLocalizedText(
    std::string_view token,
    const std::unordered_map<std::string, std::string>& localization)
{
    if (token.empty())
    {
        return {};
    }

    if (token.front() == '#')
    {
        token.remove_prefix(1);
    }

    if (const auto iterator = localization.find(std::string(token)); iterator != localization.end())
    {
        return iterator->second;
    }

    return {};
}

std::string StripRichTextAndCollapseWhitespace(std::string_view value)
{
    std::string result;
    result.reserve(value.size());

    bool in_tag = false;
    bool previous_was_space = false;
    for (const auto ch : value)
    {
        if (ch == '<')
        {
            in_tag = true;
            continue;
        }

        if (ch == '>')
        {
            in_tag = false;
            continue;
        }

        if (in_tag)
        {
            continue;
        }

        const bool whitespace = std::isspace(static_cast<unsigned char>(ch)) != 0;
        if (whitespace)
        {
            if (!result.empty() && !previous_was_space)
            {
                result.push_back(' ');
            }
            previous_was_space = true;
            continue;
        }

        result.push_back(ch);
        previous_was_space = false;
    }

    while (!result.empty() && result.back() == ' ')
    {
        result.pop_back();
    }

    return result;
}

std::unordered_map<std::string, std::string> BuildLocalizationMap(
    const std::unordered_map<std::string, std::string>& tokens)
{
    return tokens;
}

std::unordered_map<std::string, std::string> BuildLocalizationMap(const KeyValueNode& root)
{
    std::unordered_map<std::string, std::string> localization;
    const KeyValueNode* tokens = &root;
    if (root.key != "Tokens")
    {
        tokens = root.FindChild("Tokens");
    }

    if (tokens == nullptr)
    {
        return localization;
    }

    localization.reserve(tokens->children.size());
    for (const auto& child : tokens->children)
    {
        if (!child.children.empty() || child.key.empty())
        {
            continue;
        }

        localization.emplace(child.key, child.value);
    }

    return localization;
}

void SortNamedIds(std::vector<NamedId>& values)
{
    if (values.empty())
    {
        return;
    }

    const auto default_iterator = std::find_if(
        values.begin(),
        values.end(),
        [](const NamedId& value)
        {
            return value.id == 0;
        });

    auto sort_begin = values.begin();
    if (default_iterator != values.end())
    {
        std::rotate(values.begin(), default_iterator, std::next(default_iterator));
        sort_begin = std::next(values.begin());
    }

    std::sort(
        sort_begin,
        values.end(),
        [](const NamedId& left, const NamedId& right)
        {
            const auto left_name = Lowercase(left.name);
            const auto right_name = Lowercase(right.name);
            if (left_name == right_name)
            {
                return left.id < right.id;
            }

            return left_name < right_name;
        });
}

void UpdateNamedIdsFromLookup(
    std::vector<NamedId>& values,
    const std::unordered_map<int, std::string>& names_by_id)
{
    for (auto& value : values)
    {
        if (const auto iterator = names_by_id.find(value.id); iterator != names_by_id.end() && !iterator->second.empty())
        {
            value.name = iterator->second;
        }
    }
}

bool IsSupportedBaseWeaponId(const int id)
{
    return std::any_of(
        kBaseItems.begin(),
        kBaseItems.end(),
        [id](const IdNameSeed& item)
        {
            return item.id == id && item.id != 42 && item.id != 5028;
        });
}

template <std::size_t N>
std::string_view FindSeedNameById(const std::array<IdNameSeed, N>& seeds, const int id) noexcept
{
    const auto it = std::find_if(
        seeds.begin(),
        seeds.end(),
        [id](const IdNameSeed& seed)
        {
            return seed.id == id;
        });
    return it == seeds.end() ? std::string_view {} : std::string_view {it->name};
}

std::string ResolveKnownCatalogItemDisplayName(const int id)
{
    if (const auto base_name = FindSeedNameById(kBaseItems, id); !base_name.empty())
    {
        return std::string {base_name};
    }

    if (const auto knife_name = FindSeedNameById(kKnifeItems, id); !knife_name.empty())
    {
        return std::string {knife_name};
    }

    if (const auto glove_name = FindSeedNameById(kGloveItems, id); !glove_name.empty())
    {
        return std::string {glove_name};
    }

    return {};
}

std::string ResolveItemDisplayName(
    const int id,
    const FlatSectionEntry& item,
    const std::unordered_map<std::string, std::string>& localization)
{
    if (const auto localized_name = TryResolveLocalizedText(FindFlatValue(item, "item_name"), localization);
        !localized_name.empty())
    {
        return localized_name;
    }

    if (const auto known_name = ResolveKnownCatalogItemDisplayName(id); !known_name.empty())
    {
        return known_name;
    }

    const auto raw_name = FindFlatValue(item, "name");
    return HumanizeIdentifier(raw_name);
}

std::string ResolveItemDisplayName(
    const int id,
    const KeyValueNode& item,
    const std::unordered_map<std::string, std::string>& localization)
{
    if (auto localized_name = TryResolveLocalizedText(item.FindValue("item_name"), localization);
        !localized_name.empty())
    {
        return localized_name;
    }

    if (const auto known_name = ResolveKnownCatalogItemDisplayName(id); !known_name.empty())
    {
        return known_name;
    }

    auto raw_name = item.FindValue("name");
    return HumanizeIdentifier(raw_name);
}

std::string ResolvePaintKitDisplayName(
    const FlatSectionEntry& paint_kit,
    const std::unordered_map<std::string, std::string>& localization)
{
    auto localized = TryResolveLocalizedText(FindFlatValue(paint_kit, "description_tag"), localization);
    if (!localized.empty())
    {
        return StripRichTextAndCollapseWhitespace(localized);
    }

    localized = TryResolveLocalizedText(FindFlatValue(paint_kit, "description_string"), localization);
    if (!localized.empty())
    {
        return StripRichTextAndCollapseWhitespace(localized);
    }

    return HumanizeIdentifier(FindFlatValue(paint_kit, "name"));
}

std::string ResolvePaintKitDisplayName(
    const KeyValueNode& paint_kit,
    const std::unordered_map<std::string, std::string>& localization)
{
    auto localized = TryResolveLocalizedText(paint_kit.FindValue("description_tag"), localization);
    if (!localized.empty())
    {
        return StripRichTextAndCollapseWhitespace(localized);
    }

    localized = TryResolveLocalizedText(paint_kit.FindValue("description_string"), localization);
    if (!localized.empty())
    {
        return StripRichTextAndCollapseWhitespace(localized);
    }

    return HumanizeIdentifier(paint_kit.FindValue("name"));
}

std::string ResolveQualityDisplayName(const std::string_view key)
{
    if (key == "normal")
    {
        return "Default";
    }

    if (key == "selfmade")
    {
        return "Self-Made";
    }

    return HumanizeIdentifier(key);
}
}

nskinzcs2::model::Catalog nskinzcs2::CatalogBuilder::BuildFallbackCatalog()
{
    Catalog catalog;
    catalog.base_items = CopyArray(kBaseItems);
    catalog.knife_items = CopyArray(kKnifeItems);
    catalog.glove_items = CopyArray(kGloveItems);
    catalog.weapon_paint_kits = CopyArray(kWeaponPaintKits);
    catalog.glove_paint_kits = CopyArray(kGlovePaintKits);
    for (const auto& seed : kGlovePaintCompatibilitySeeds)
    {
    catalog.glove_paint_compatibility.emplace(seed.paint_kit_id, seed.glove_item_id);
    }
    catalog.sticker_kits = CopyArray(kStickerKits);
    catalog.keychain_kits = CopyArray(kKeychainKits);
    catalog.qualities = CopyArray(kQualities);
    catalog.runtime_populated = false;
    catalog.warnings.push_back("Using built-in fallback catalog until live CS2 schema reflection is bound.");

    for (const auto& seed : kVisualSeeds)
    {
        catalog.visuals.emplace(seed.id, VisualInfo {
            seed.world_model,
            seed.view_model,
            {},
            {},
            {},
            seed.killfeed_icon,
            false,
            seed.is_knife,
            seed.is_glove,
        });
    }

    for (auto& [id, visual] : catalog.visuals)
    {
        static_cast<void>(id);
        FinalizeVisualInfo(visual);
    }

    if (HasDuplicateIds(catalog.base_items))
    {
        catalog.warnings.push_back("Duplicate base item ids detected in fallback catalog.");
    }

    if (HasDuplicateIds(catalog.knife_items))
    {
        catalog.warnings.push_back("Duplicate knife ids detected in fallback catalog.");
    }

    if (HasDuplicateIds(catalog.glove_items))
    {
        catalog.warnings.push_back("Duplicate glove ids detected in fallback catalog.");
    }

    return catalog;
}

std::optional<nskinzcs2::model::Catalog> nskinzcs2::CatalogBuilder::BuildCatalogFromGameText(
    const std::string_view items_game_text,
    const std::string_view localization_text)
{
    const auto items_section_text = ExtractNamedSectionText(items_game_text, "items");
    const auto qualities_section_text = ExtractNamedSectionText(items_game_text, "qualities");
    const auto paint_kits_section_texts = ExtractNamedSectionTexts(items_game_text, "paint_kits");
    const auto sticker_kits_section_texts = ExtractNamedSectionTexts(items_game_text, "sticker_kits");
    const auto keychain_definitions_section_texts =
        ExtractNamedSectionTexts(items_game_text, "keychain_definitions");
    const auto tokens_section_text = ExtractNamedSectionText(localization_text, "Tokens");
    if (!items_section_text.has_value() ||
        !qualities_section_text.has_value() ||
        paint_kits_section_texts.empty() ||
        !tokens_section_text.has_value())
    {
        return std::nullopt;
    }

    auto catalog = BuildFallbackCatalog();
    catalog.runtime_populated = true;
    catalog.warnings.clear();
    catalog.warnings.push_back("Catalog loaded from local CS2 game files.");

    const auto items_section = ParseFlatSectionEntries(*items_section_text);
    const auto qualities_section = ParseFlatSectionEntries(*qualities_section_text);
    const auto paint_kits_section = MergeFlatSectionEntries(paint_kits_section_texts);
    const auto sticker_kits_section = MergeFlatSectionEntries(sticker_kits_section_texts);
    const auto keychain_definitions_section =
        MergeFlatSectionEntries(keychain_definitions_section_texts);
    const auto localization = BuildLocalizationMap(ParseLocalizationTokensSection(*tokens_section_text));

    std::unordered_map<int, std::string> base_item_names;
    std::unordered_map<std::string, int> weapon_definitions_by_name;
    std::unordered_map<std::string, std::vector<int>> paint_ids_by_name;
    std::unordered_map<int, std::string> knife_names;
    std::unordered_map<int, std::string> glove_names;
    std::unordered_map<int, NamedId> weapon_paint_kits;
    std::unordered_map<int, NamedId> glove_paint_kits;
    std::unordered_map<int, int> glove_paint_compatibility;
    std::unordered_map<int, NamedId> sticker_kits;
    std::unordered_map<int, NamedId> keychain_kits;
    std::unordered_map<int, NamedId> qualities;
    glove_paint_compatibility.emplace(0, 0);

    for (const auto& [item_key, item] : items_section)
    {
        int id = 0;
        if (!TryParseInt(item_key, id))
        {
            continue;
        }

        const auto prefab = FindFlatValue(item, "prefab");
        const auto raw_name = FindFlatValue(item, "name");
        const auto item_slot = FindFlatValue(item, "item_slot");
        const auto item_class = FindFlatValue(item, "item_class");
        const auto display_name = ResolveItemDisplayName(id, item, localization);

        const auto is_glove =
            prefab == "hands" ||
            prefab == "hands_paintable";
        const auto is_knife =
            StartsWith(raw_name, "weapon_") &&
            (prefab == "melee" ||
                prefab == "melee_unusual" ||
                item_slot == "melee" ||
                item_class == "weapon_knife");

        if (is_glove)
        {
            glove_names[id] = display_name;
        }
        else if (is_knife)
        {
            knife_names[id] = display_name;
        }
        else if (StartsWith(raw_name, "weapon_") && IsSupportedBaseWeaponId(id))
        {
            base_item_names[id] = display_name;
            weapon_definitions_by_name.emplace(raw_name, id);
        }

        const auto model_player = FindFlatValue(item, "model_player");
        const auto model_world = FindFlatValue(item, "model_world");
        if (!model_player.empty() || !model_world.empty())
        {
            auto& visual = catalog.visuals[id];
            if (!model_world.empty())
            {
                visual.world_model = std::string(model_world);
            }
            if (!model_player.empty())
            {
                visual.view_model = std::string(model_player);
            }
            visual.is_knife = is_knife || visual.is_knife;
            visual.is_glove = is_glove || visual.is_glove;
        }
    }

    UpdateNamedIdsFromLookup(catalog.base_items, base_item_names);
    UpdateNamedIdsFromLookup(catalog.knife_items, knife_names);
    UpdateNamedIdsFromLookup(catalog.glove_items, glove_names);

    for (auto& [id, visual] : catalog.visuals)
    {
        static_cast<void>(id);
        FinalizeVisualInfo(visual);
    }

    for (const auto& [quality_key, quality] : qualities_section)
    {
        int id = 0;
        if (!TryParseInt(FindFlatValue(quality, "value"), id))
        {
            continue;
        }

        qualities[id] = NamedId {id, ResolveQualityDisplayName(quality_key)};
    }

    if (!qualities.empty())
    {
        catalog.qualities.clear();
        catalog.qualities.reserve(qualities.size());
        for (const auto& [id, quality] : qualities)
        {
            catalog.qualities.push_back(quality);
        }

        std::sort(
            catalog.qualities.begin(),
            catalog.qualities.end(),
            [](const NamedId& left, const NamedId& right)
            {
                return left.id < right.id;
            });
    }

    for (const auto& [paint_kit_key, paint_kit] : paint_kits_section)
    {
        int id = 0;
        if (!TryParseInt(paint_kit_key, id))
        {
            continue;
        }

        const auto raw_name = FindFlatValue(paint_kit, "name");
        const auto name = ResolvePaintKitDisplayName(paint_kit, localization);
        const NamedId entry {id, name};

        if (id == 0)
        {
            weapon_paint_kits[id] = entry;
            glove_paint_kits[id] = entry;
            continue;
        }

        if (id == 9001 || id >= 10000)
        {
            glove_paint_kits[id] = entry;
            const auto compatible_glove_definition = ResolveCompatibleGloveDefinitionIndex(raw_name);
            if (compatible_glove_definition != 0)
            {
                glove_paint_compatibility[id] = compatible_glove_definition;
            }
            continue;
        }

        weapon_paint_kits[id] = entry;
        catalog.weapon_paint_uses_legacy_model[id] = FindFlatValue(paint_kit, "use_legacy_model") == "1";
        if (id > 0 && !raw_name.empty())
        {
            paint_ids_by_name[std::string(raw_name)].push_back(id);
        }
    }

    // Use exact game-defined combinations, not localized names, paint prefixes,
    // or model-age guesses. Collections alone miss loot-list-only variants.
    // Merge every repeated section; shared finishes may name several weapons.
    for (const auto section_name : {"item_sets", "client_loot_lists"})
    {
        for (const auto& section : ExtractNamedSectionTexts(items_game_text, section_name))
        {
            std::size_t start = 0;
            while (start < section.size())
            {
                auto end = section.find('\n', start);
                if (end == std::string::npos) { end = section.size(); }
                const auto line = TrimView(std::string_view(section).substr(start, end - start));
                start = end + 1;
                if (line.empty() || StartsWith(line, "//") || !LineMatchesCurrentPlatform(line)) { continue; }
                const auto values = ExtractQuotedStringsFromLine(line);
                if (values.size() < 2 || !StartsWith(values[0], "[")) { continue; }
                const auto close = values[0].find(']');
                if (close == std::string::npos || close <= 1) { continue; }
                const auto weapon = weapon_definitions_by_name.find(values[0].substr(close + 1));
                const auto paints = paint_ids_by_name.find(values[0].substr(1, close - 1));
                if (weapon == weapon_definitions_by_name.end() || paints == paint_ids_by_name.end()) { continue; }
                auto& allowed = catalog.weapon_paint_compatibility[weapon->second];
                allowed.insert(allowed.end(), paints->second.begin(), paints->second.end());
            }
        }
    }
    for (auto& [definition, paints] : catalog.weapon_paint_compatibility)
    {
        static_cast<void>(definition);
        std::sort(paints.begin(), paints.end());
        paints.erase(std::unique(paints.begin(), paints.end()), paints.end());
    }
    if (catalog.weapon_paint_compatibility.empty())
    {
        catalog.warnings.push_back("Weapon paint compatibility is unavailable; colored weapon finishes are blocked until local game data is loaded.");
    }

    if (!weapon_paint_kits.empty())
    {
        catalog.weapon_paint_kits.clear();
        catalog.weapon_paint_kits.reserve(weapon_paint_kits.size());
        for (const auto& [id, paint_kit] : weapon_paint_kits)
        {
            catalog.weapon_paint_kits.push_back(paint_kit);
        }
        SortNamedIds(catalog.weapon_paint_kits);
    }

    if (!glove_paint_kits.empty())
    {
        catalog.glove_paint_kits.clear();
        catalog.glove_paint_kits.reserve(glove_paint_kits.size());
        for (const auto& [id, paint_kit] : glove_paint_kits)
        {
            catalog.glove_paint_kits.push_back(paint_kit);
        }
        SortNamedIds(catalog.glove_paint_kits);
    }
    catalog.glove_paint_compatibility = std::move(glove_paint_compatibility);

    sticker_kits.emplace(
        0,
        NamedId {0, nskinzcs2::build::kUiLanguageChinese ? "\xE6\x97\xA0" : "None"});
    for (const auto& [sticker_kit_key, sticker_kit] : sticker_kits_section)
    {
        int id = 0;
        if (!TryParseInt(sticker_kit_key, id))
        {
            continue;
        }

        const auto item_name = FindFlatValue(sticker_kit, "item_name");
        const auto raw_name = FindFlatValue(sticker_kit, "name");
        if (StartsWith(item_name, "#SprayKit") || StartsWith(item_name, "#GraffitiKit"))
        {
            continue;
        }

        if (StartsWith(raw_name, "spray_") || StartsWith(raw_name, "graffiti_"))
        {
            continue;
        }

        if (item_name.empty() && raw_name.empty())
        {
            continue;
        }

        auto name = ResolveLocalizedText(item_name, localization);
        if (name.empty())
        {
            name = HumanizeIdentifier(raw_name);
        }

        sticker_kits[id] = NamedId {id, std::move(name)};
    }

    if (!sticker_kits.empty())
    {
        catalog.sticker_kits.clear();
        catalog.sticker_kits.reserve(sticker_kits.size());
        for (const auto& [id, sticker] : sticker_kits)
        {
            catalog.sticker_kits.push_back(sticker);
        }
        SortNamedIds(catalog.sticker_kits);
    }

    keychain_kits.emplace(
        0,
        NamedId {0, nskinzcs2::build::kUiLanguageChinese ? "\xE6\x97\xA0" : "None"});
    for (const auto& [keychain_key, keychain_definition] : keychain_definitions_section)
    {
        int id = 0;
        if (!TryParseInt(keychain_key, id))
        {
            continue;
        }

        if (id == 0)
        {
            continue;
        }

        const auto pedestal_display_model = FindFlatValue(keychain_definition, "pedestal_display_model");
        if (!pedestal_display_model.empty())
        {
            auto& visual = catalog.keychain_visuals[id];
            visual.model_path = std::string(pedestal_display_model);
            FinalizeKeychainVisualInfo(visual);
        }

        auto name = ResolveLocalizedText(FindFlatValue(keychain_definition, "loc_name"), localization);
        if (name.empty())
        {
            name = HumanizeIdentifier(FindFlatValue(keychain_definition, "name"));
        }

        if (name.empty())
        {
            continue;
        }

        keychain_kits[id] = NamedId {id, std::move(name)};
    }

    if (!keychain_kits.empty())
    {
        catalog.keychain_kits.clear();
        catalog.keychain_kits.reserve(keychain_kits.size());
        for (const auto& [id, keychain] : keychain_kits)
        {
            catalog.keychain_kits.push_back(keychain);
        }
        SortNamedIds(catalog.keychain_kits);
    }

    if (catalog.weapon_paint_kits.empty() ||
        catalog.sticker_kits.empty() ||
        catalog.keychain_kits.empty())
    {
        catalog.warnings.push_back("Local CS2 game files were parsed, but some catalog lists stayed empty.");
    }

    return catalog;
}

std::optional<nskinzcs2::model::Catalog> nskinzcs2::CatalogBuilder::BuildCatalogFromGameFiles(
    const std::filesystem::path& items_game_path,
    const std::filesystem::path& localization_path)
{
    const auto items_game_text = ReadTextFile(items_game_path);
    const auto localization_text = ReadTextFile(localization_path);
    if (!items_game_text.has_value() || !localization_text.has_value())
    {
        return std::nullopt;
    }

    return BuildCatalogFromGameText(*items_game_text, *localization_text);
}

nskinzcs2::WeaponPaintCompatibility nskinzcs2::CatalogBuilder::CheckWeaponPaintCompatibility(
    const model::Catalog* catalog, const int definition, const int paint_kit) noexcept
{
    // Zero expresses either no change or explicit vanilla; both are independent
    // of the paint compatibility catalog. Never turn a rejected paint into zero.
    if (paint_kit == 0) { return WeaponPaintCompatibility::Compatible; }
    if (paint_kit < 0 || definition <= 0) { return WeaponPaintCompatibility::Incompatible; }
    if (catalog == nullptr) { return WeaponPaintCompatibility::Unknown; }
    const auto found = catalog->weapon_paint_compatibility.find(definition);
    if (found == catalog->weapon_paint_compatibility.end() || found->second.empty())
    {
        return WeaponPaintCompatibility::Unknown;
    }
    return std::binary_search(found->second.begin(), found->second.end(), paint_kit)
        ? WeaponPaintCompatibility::Compatible : WeaponPaintCompatibility::Incompatible;
}

std::vector<nskinzcs2::model::NamedId> nskinzcs2::CatalogBuilder::BuildCompatibleWeaponPaintList(
    const model::Catalog& catalog, const int definition)
{
    std::vector<model::NamedId> values;
    for (const auto& paint : catalog.weapon_paint_kits)
    {
        if (CheckWeaponPaintCompatibility(&catalog, definition, paint.id) == WeaponPaintCompatibility::Compatible)
        {
            values.push_back(paint);
        }
    }
    return values;
}

bool nskinzcs2::CatalogBuilder::HasDuplicateIds(const std::vector<model::NamedId>& values)
{
    std::map<int, int> counters;
    for (const auto& value : values)
    {
        ++counters[value.id];
    }

    return std::any_of(
        counters.begin(),
        counters.end(),
        [](const auto& pair)
        {
            return pair.second > 1;
        });
}
