#pragma once

#include <cstdint>

namespace nskinzcs2::runtime
{
enum class KnifeAnimationTrigger
{
    None,
    Deploy,
    Inspect,
    LightAttack,
    HeavyAttack,
};

struct KnifeAnimationStateSnapshot
{
    bool valid = false;
    std::uint64_t xuid = 0;
    std::uint64_t apply_signature = 0;
    int world_entity_index = -1;
    int first_person_entity_index = -1;
    int live_definition_index = 0;
    int source_definition_index = 0;
    int target_definition_index = 0;
    std::uint32_t sequence = 0;
    float sequence_start_time = 0.0f;
    int gameplay_state = 0;
    float gameplay_state_timestamp = 0.0f;
    bool networked_sequence_changed = false;
    bool sequence_finished = false;
    bool inspect_pending = false;
    bool inspect_should_loop = false;
    bool is_looking_at_weapon = false;
    bool is_holding_look_at_weapon = false;
    bool block_inspect_until_next_graph_update = false;
};

struct KnifeAnimationTriggerInputs
{
    bool has_previous = false;
    KnifeAnimationStateSnapshot previous {};
    KnifeAnimationStateSnapshot current {};
};

bool KnifeAnimationInspectActive(const KnifeAnimationStateSnapshot& snapshot) noexcept;
bool KnifeAnimationAttackClassifierReady() noexcept;
bool KnifeAnimationGameplayStateKnown(const KnifeAnimationStateSnapshot& snapshot) noexcept;
KnifeAnimationTrigger DetectKnifeAnimationTrigger(const KnifeAnimationTriggerInputs& inputs) noexcept;
const wchar_t* KnifeAnimationTriggerText(KnifeAnimationTrigger trigger) noexcept;
}
