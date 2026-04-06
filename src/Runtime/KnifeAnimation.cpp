#include "Runtime/KnifeAnimation.hpp"

#include <array>
#include <cmath>
#include <cstddef>

namespace
{
constexpr bool kKnifeAttackClassifierCalibrated = true;

constexpr std::array<int, 1> kKnifeLightAttackGameplayStates {{1}};

constexpr std::array<int, 1> kKnifeHeavyAttackGameplayStates {{2}};

enum class KnifeAttackClass
{
    None,
    Light,
    Heavy,
    Unknown,
};

template <std::size_t N>
bool ContainsGameplayState(const std::array<int, N>& values, const int gameplay_state) noexcept
{
    for (const auto value : values)
    {
        if (value == gameplay_state)
        {
            return true;
        }
    }

    return false;
}

KnifeAttackClass ClassifyKnifeAttackState(const int gameplay_state) noexcept
{
    if (gameplay_state == 0)
    {
        return KnifeAttackClass::None;
    }

    if (ContainsGameplayState(kKnifeLightAttackGameplayStates, gameplay_state))
    {
        return KnifeAttackClass::Light;
    }

    if (ContainsGameplayState(kKnifeHeavyAttackGameplayStates, gameplay_state))
    {
        return KnifeAttackClass::Heavy;
    }

    return KnifeAttackClass::Unknown;
}

bool TimestampAdvanced(const float previous_timestamp, const float current_timestamp) noexcept
{
    return current_timestamp > previous_timestamp + 0.000001f;
}
}

namespace nskinzcs2::runtime
{
bool KnifeAnimationInspectActive(const KnifeAnimationStateSnapshot& snapshot) noexcept
{
    return snapshot.inspect_pending ||
        snapshot.is_looking_at_weapon ||
        snapshot.is_holding_look_at_weapon;
}

bool KnifeAnimationAttackClassifierReady() noexcept
{
    return kKnifeAttackClassifierCalibrated &&
        !kKnifeLightAttackGameplayStates.empty() &&
        !kKnifeHeavyAttackGameplayStates.empty();
}

bool KnifeAnimationGameplayStateKnown(const KnifeAnimationStateSnapshot& snapshot) noexcept
{
    if (!KnifeAnimationAttackClassifierReady())
    {
        return true;
    }

    if (!snapshot.valid || KnifeAnimationInspectActive(snapshot))
    {
        return true;
    }

    return ClassifyKnifeAttackState(snapshot.gameplay_state) != KnifeAttackClass::Unknown;
}

KnifeAnimationTrigger DetectKnifeAnimationTrigger(const KnifeAnimationTriggerInputs& inputs) noexcept
{
    if (!inputs.current.valid)
    {
        return KnifeAnimationTrigger::None;
    }

    if (!inputs.has_previous || !inputs.previous.valid)
    {
        return KnifeAnimationTrigger::Deploy;
    }

    if (inputs.previous.xuid != inputs.current.xuid ||
        inputs.previous.apply_signature != inputs.current.apply_signature ||
        inputs.previous.world_entity_index != inputs.current.world_entity_index ||
        inputs.previous.first_person_entity_index != inputs.current.first_person_entity_index ||
        inputs.previous.source_definition_index != inputs.current.source_definition_index ||
        inputs.previous.target_definition_index != inputs.current.target_definition_index)
    {
        return KnifeAnimationTrigger::Deploy;
    }

    const auto previous_inspect = KnifeAnimationInspectActive(inputs.previous);
    const auto current_inspect = KnifeAnimationInspectActive(inputs.current);
    if (!previous_inspect && current_inspect)
    {
        return KnifeAnimationTrigger::Inspect;
    }

    if (!KnifeAnimationAttackClassifierReady())
    {
        return KnifeAnimationTrigger::None;
    }

    const auto previous_attack_class = ClassifyKnifeAttackState(inputs.previous.gameplay_state);
    const auto current_attack_class = ClassifyKnifeAttackState(inputs.current.gameplay_state);
    if (current_attack_class == KnifeAttackClass::Light &&
        (previous_attack_class != KnifeAttackClass::Light ||
         TimestampAdvanced(
             inputs.previous.gameplay_state_timestamp,
             inputs.current.gameplay_state_timestamp)))
    {
        return KnifeAnimationTrigger::LightAttack;
    }

    if (current_attack_class == KnifeAttackClass::Heavy &&
        (previous_attack_class != KnifeAttackClass::Heavy ||
         TimestampAdvanced(
             inputs.previous.gameplay_state_timestamp,
             inputs.current.gameplay_state_timestamp)))
    {
        return KnifeAnimationTrigger::HeavyAttack;
    }

    return KnifeAnimationTrigger::None;
}

const wchar_t* KnifeAnimationTriggerText(const KnifeAnimationTrigger trigger) noexcept
{
    switch (trigger)
    {
    case KnifeAnimationTrigger::Deploy:
        return L"deploy";
    case KnifeAnimationTrigger::Inspect:
        return L"inspect";
    case KnifeAnimationTrigger::LightAttack:
        return L"light attack";
    case KnifeAnimationTrigger::HeavyAttack:
        return L"heavy attack";
    case KnifeAnimationTrigger::None:
    default:
        return L"none";
    }
}
}
