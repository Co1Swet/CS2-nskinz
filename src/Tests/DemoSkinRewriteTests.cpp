#include "Runtime/DemoSkinRewrite.hpp"

#include "json.hpp"

#include <cmath>
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

    const auto plan = runtime::BuildDefaultDemoSkinRewritePlan(config);
    Check(plan.passes.size() == 1, "knife and glove for one owner must share one rewrite pass");
    Check(plan.knife_item_count == 2, "one knife profile must materialize both T and CT defaults");
    Check(plan.glove_item_count == 2, "one glove profile must materialize both T and CT defaults");
    Check(plan.passes[0].xuid == knife.xuid, "rewrite pass must preserve the profile owner XUID");
    Check(plan.passes[0].item_json.size() == 4, "one owner must receive four default slot records");

    const auto items = IndexItems(plan.passes[0]);
    Check(items.size() == 4, "the four source definition records must be unique");
    for (const auto source : {59, 42})
    {
        const auto& item = items.at(source);
        Check(item.at("item_id64") == "0", "default knife rewrite must use item_id64 zero");
        Check(item.at("replacement_definition_index") == 507, "knife target definition must be serialized");
        Check(item.at("paint_kit") == 570, "knife paint kit must be serialized");
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
        Check(item.at("pattern_seed") == 123.0, "glove seed must be serialized");
        Check(std::abs(item.at("wear").get<double>() - 0.14) < 0.000001,
            "glove wear must be serialized");
    }
    Check(items.at(5028).at("team") == "T", "default T glove definition 5028 must stay on team T");
    Check(items.at(5029).at("team") == "CT", "default CT glove definition 5029 must stay on team CT");

    auto second_owner = knife;
    second_owner.xuid += 1;
    config.knife_profiles.push_back(second_owner);
    auto ignored_finish_only = glove;
    ignored_finish_only.xuid += 2;
    ignored_finish_only.mode = model::GloveProfileMode::FinishOnly;
    config.glove_profiles.push_back(ignored_finish_only);
    const auto grouped = runtime::BuildDefaultDemoSkinRewritePlan(config);
    Check(grouped.passes.size() == 2, "different owners must be rewritten in separate authenticated passes");
    Check(grouped.knife_item_count == 4, "two knife owners must create two team records each");
    Check(grouped.glove_item_count == 2, "finish-only gloves must stay on the existing runtime path");

    Check(
        runtime::IsGeneratedDemoSkinPath(L"C:\\demo\\nskinz_skin_match_123.dem"),
        "generated demo marker must be detected case-insensitively by filename");
    Check(
        !runtime::IsGeneratedDemoSkinPath(L"C:\\demo\\match.dem"),
        "ordinary source demos must not be treated as generated demos");
}
