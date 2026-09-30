#include "Runtime/DemoSkinRewrite.hpp"

#include "json.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>

namespace
{
using json = nlohmann::json;

void Check(const bool value, const char* message)
{
    if (!value)
    {
        throw std::runtime_error(message);
    }
}

std::map<int, json> IndexItems(const nskinzcs2::runtime::DemoSkinRewritePass& pass)
{
    std::map<int, json> result;
    for (const auto& encoded : pass.item_json)
    {
        auto item = json::parse(encoded);
        result.emplace(item.at("definition_index").get<int>(), std::move(item));
    }
    return result;
}
}

void RunDemoSkinRewriteTests()
{
    using namespace nskinzcs2;

    model::ConfigDocument config;
    Check(
        runtime::BuildDefaultDemoSkinRewritePlan(config).passes.empty(),
        "empty config must not create a demo rewrite pass");

    model::KnifeProfile knife;
    knife.enabled = true;
    knife.xuid = 76561198800578327ull;
    knife.mode = model::KnifeProfileMode::ModelAndFinish;
    knife.model_definition = 507;
    knife.paint_kit = 570;
    knife.custom_name = "Test Knife";
    knife.seed = 392;
    knife.wear = 0.02349f;
    config.knife_profiles.push_back(knife);

    model::GloveProfile glove;
    glove.enabled = true;
    glove.xuid = knife.xuid;
    glove.mode = model::GloveProfileMode::ModelAndFinish;
    glove.model_definition = 5031;
    glove.paint_kit = 10037;
    glove.seed = 123;
    glove.wear = 0.14f;
    config.glove_profiles.push_back(glove);

    model::CosmeticProfile weapon;
    weapon.enabled = true;
    weapon.xuid = knife.xuid;
    weapon.base_item = 7;
    weapon.paint_kit = 302;
    weapon.custom_name = "Test AK";
    weapon.seed = 77;
    weapon.wear = 0.19f;
    config.profiles.push_back(weapon);

    const auto plan = runtime::BuildDefaultDemoSkinRewritePlan(config);
    Check(plan.passes.size() == 1, "weapon, knife and glove for one owner must share one rewrite pass");
    Check(plan.weapon_item_count == 1, "one firearm profile must create one ANY-team source record");
    Check(plan.knife_item_count == 2, "one knife profile must materialize both T and CT defaults");
    Check(plan.glove_item_count == 2, "one glove profile must materialize both T and CT defaults");
    Check(plan.passes[0].xuid == knife.xuid, "rewrite pass must preserve the profile owner XUID");
    Check(plan.passes[0].item_json.size() == 5, "one owner must receive one firearm and four default slot records");

    const auto items = IndexItems(plan.passes[0]);
    Check(items.size() == 5, "all source definition records must be unique");
    Check(items.at(7).at("team") == "ANY", "firearm rewrite must use ANY team routing");
    Check(items.at(7).at("paint_kit") == 302, "firearm paint kit must be serialized");
    Check(items.at(7).at("custom_name") == "Test AK", "firearm custom name must be serialized");
    Check(items.at(7).at("quality") == weapon.quality, "firearm quality must be serialized");
    Check(items.at(7).at("pattern_seed") == 77.0, "firearm seed must be serialized");
    Check(std::abs(items.at(7).at("wear").get<double>() - 0.19) < 0.000001,
        "firearm wear must be serialized");
    Check(items.at(7).at("mesh_group_mask") == 2,
        "catalog-free default AK reconstruction must retain the conservative legacy mesh fallback");
    Check(!items.at(7).contains("replacement_definition_index"),
        "firearm temporary rewrite must preserve its source definition");
    for (const auto source : {59, 42})
    {
        const auto& item = items.at(source);
        Check(item.at("item_id64") == "0", "default knife rewrite must use item_id64 zero");
        Check(item.at("replacement_definition_index") == 507, "knife target definition must be serialized");
        Check(item.at("paint_kit") == 570, "knife paint kit must be serialized");
        Check(item.at("custom_name") == "Test Knife", "knife custom name must be serialized");
        Check(item.at("quality") == 3, "knife demo rebuild must use knife quality");
        Check(item.at("pattern_seed") == 392.0, "knife seed must be serialized");
        Check(std::abs(item.at("wear").get<double>() - 0.02349) < 0.000001,
            "knife wear must be serialized");
    }
    Check(items.at(59).at("team") == "T", "default T knife definition 59 must stay on team T");
    Check(items.at(42).at("team") == "CT", "default CT knife definition 42 must stay on team CT");

    for (const auto source : {5028, 5029})
    {
        const auto& item = items.at(source);
        Check(item.at("item_id64") == "0", "default glove rewrite must use item_id64 zero");
        Check(item.at("replacement_definition_index") == 5031, "glove target definition must be serialized");
        Check(item.at("paint_kit") == 10037, "glove paint kit must be serialized");
        Check(item.at("custom_name") == "nSkinz CS2", "default glove name must be serialized");
        Check(item.at("quality") == glove.quality, "glove quality must be serialized");
        Check(item.at("pattern_seed") == 123.0, "glove seed must be serialized");
        Check(std::abs(item.at("wear").get<double>() - 0.14) < 0.000001,
            "glove wear must be serialized");
    }
    Check(items.at(5028).at("team") == "T", "default T glove definition 5028 must stay on team T");
    Check(items.at(5029).at("team") == "CT", "default CT glove definition 5029 must stay on team CT");

    model::Catalog compatibility_catalog;
    compatibility_catalog.weapon_paint_kits = {
        {0, "-"}, {302, "AK finish"}, {570, "Knife finish"}, {999, "Wrong finish"}};
    compatibility_catalog.weapon_paint_compatibility = {{7, {302}}, {507, {570}}};
    compatibility_catalog.weapon_paint_uses_legacy_model = {{302, true}};
    compatibility_catalog.glove_paint_compatibility = {{0, 0}, {10037, 5031}};
    Check(
        runtime::BuildDefaultDemoSkinRewritePlan(config, &compatibility_catalog).validation_issue.empty(),
        "exact firearm, knife and glove compatibility pairs must pass rewrite validation");
    const auto legacy_mesh_plan = runtime::BuildDefaultDemoSkinRewritePlan(config, &compatibility_catalog);
    Check(IndexItems(legacy_mesh_plan.passes.at(0)).at(7).at("mesh_group_mask") == 2,
        "legacy AK finishes must request mesh group two in the temporary demo");
    auto modern_mesh_catalog = compatibility_catalog;
    modern_mesh_catalog.weapon_paint_uses_legacy_model[302] = false;
    const auto modern_mesh_plan = runtime::BuildDefaultDemoSkinRewritePlan(config, &modern_mesh_catalog);
    Check(IndexItems(modern_mesh_plan.passes.at(0)).at(7).at("mesh_group_mask") == 1,
        "modern AK finishes must request mesh group one in the temporary demo");
    auto non_ak_config = config;
    non_ak_config.profiles[0].base_item = 16;
    compatibility_catalog.weapon_paint_compatibility[16] = {302};
    const auto non_ak_plan = runtime::BuildDefaultDemoSkinRewritePlan(
        non_ak_config,
        &compatibility_catalog);
    Check(!IndexItems(non_ak_plan.passes.at(0)).at(16).contains("mesh_group_mask"),
        "AK mesh selection must not alter other default firearm reconstruction requests");
    auto incompatible_config = config;
    incompatible_config.profiles[0].paint_kit = 999;
    Check(
        !runtime::BuildDefaultDemoSkinRewritePlan(incompatible_config, &compatibility_catalog).validation_issue.empty(),
        "cross-weapon paint IDs must be rejected before a demo is rewritten");

    auto second_owner = knife;
    second_owner.xuid += 1;
    config.knife_profiles.push_back(second_owner);
    auto finish_only = glove;
    finish_only.xuid += 2;
    finish_only.mode = model::GloveProfileMode::FinishOnly;
    finish_only.model_definition = 0;
    config.glove_profiles.push_back(finish_only);
    const auto grouped = runtime::BuildDefaultDemoSkinRewritePlan(config);
    Check(grouped.passes.size() == 3, "different owners must be rewritten in separate authenticated passes");
    Check(grouped.knife_item_count == 4, "two knife owners must create two team records each");
    Check(grouped.glove_item_count == 4, "finish-only gloves must also create two conditional source records");
    Check(grouped.weapon_item_count == 1, "the firearm profile must remain a single source record");
    const auto finish_only_pass = std::find_if(
        grouped.passes.begin(),
        grouped.passes.end(),
        [&](const runtime::DemoSkinRewritePass& pass) { return pass.xuid == finish_only.xuid; });
    Check(finish_only_pass != grouped.passes.end(), "finish-only glove owner must have a rewrite pass");
    const auto finish_only_items = IndexItems(*finish_only_pass);
    for (const auto source : {5028, 5029})
    {
        const auto& item = finish_only_items.at(source);
        Check(item.at("preserve_source_definition") == true,
            "finish-only glove rewrites must preserve the discovered source definition");
        Check(!item.contains("replacement_definition_index"),
            "finish-only glove request must not force a model definition");
    }

    auto no_finish_change = weapon;
    no_finish_change.xuid += 4;
    no_finish_change.paint_kit = 0;
    config.profiles.push_back(no_finish_change);
    Check(
        runtime::BuildDefaultDemoSkinRewritePlan(config).weapon_item_count == 1,
        "firearm profiles without a positive target paint must stay on the existing runtime path");

    Check(
        runtime::IsGeneratedDemoSkinPath(L"C:\\demo\\nskinz_skin_match_123.dem"),
        "generated demo marker must be detected case-insensitively by filename");
    Check(
        !runtime::IsGeneratedDemoSkinPath(L"C:\\demo\\match.dem"),
        "ordinary source demos must not be treated as generated demos");

    namespace fs = std::filesystem;
    const auto manifest_demo = fs::temp_directory_path() / "nskinz_skin_manifest_test.dem";
    auto manifest_path = manifest_demo;
    manifest_path += runtime::kGeneratedDemoSkinManifestSuffix;
    {
        std::ofstream stream(manifest_path, std::ios::binary | std::ios::trunc);
        stream << R"json({
          "schema_version": 1,
          "rewritten_weapon_profiles": [
            {"xuid": "76561198800578327", "base_item": 7}
          ],
          "rewritten_glove_xuids": ["76561198800578327"]
        })json";
    }
    Check(
        runtime::IsGeneratedDemoRewrittenWeaponProfile(manifest_demo, knife.xuid, 7),
        "generated-demo manifests must identify an exact reconstructed firearm profile for one native material initialization");
    Check(
        !runtime::IsGeneratedDemoRewrittenWeaponProfile(manifest_demo, knife.xuid, 9),
        "generated-demo manifests must not identify a different weapon definition");
    Check(
        runtime::ShouldSuppressGeneratedDemoGloveProfile(manifest_demo, knife.xuid),
        "generated-demo manifests must suppress an exact reconstructed glove profile");
    Check(
        !runtime::ShouldSuppressGeneratedDemoGloveProfile(manifest_demo, knife.xuid + 1),
        "generated-demo manifests must keep unrelated glove profiles live");
    std::error_code ignored;
    fs::remove(manifest_path, ignored);
}
