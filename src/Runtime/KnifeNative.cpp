#include "Runtime/KnifeNative.hpp"

#include <Windows.h>
#include <algorithm>
#include <limits>

namespace nskinzcs2::runtime
{
const std::array<KnifeResourceEvidenceWindow, 7>& KnifeNativeEvidenceWindows() noexcept
{
    // Exact installed 2000899 instruction windows, including the native model
    // notification and recollection's THREE-argument skeleton getter call.
    static constexpr std::uint8_t set_model[] {
        0x40,0x53,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9,0x4C,0x8B,0xC2,
        0x48,0x8B,0x0D,0xDD,0xCC,0xA8,0x01,0x48,0x8D,0x54,0x24,0x40,
        0x48,0x8B,0x01,0xFF,0x50,0x60,0x48,0x8B,0x54,0x24,0x40,
        0x48,0x8B,0xCB,0xE8,0x15,0,0,0,0x48,0x83,0xC4,0x20,0x5B,0xC3 };
    static constexpr std::uint8_t notification[] {
        0x48,0x8B,0xD3,0x48,0x8B,0xCF,0xE8,0xCD,0xEF,0xFF,0xFF,
        0x48,0x8B,0x07,0x48,0x8B,0xCF,0xFF,0x90,0x80,0x07,0,0 };
    static constexpr std::uint8_t recollect[] {
        0x48,0x89,0x4C,0x24,0x08,0x55,0x53,0x48,0x8B,0xEC,0x48,0x83,0xEC,0x78,
        0x48,0x83,0xB9,0x50,0x04,0,0,0,0x48,0x8B,0xD9,0x0F,0x84,0xC6,0x02,0,0 };
    static constexpr std::uint8_t skeleton_call[] {
        0x45,0x8B,0xC6,0x48,0x8D,0x55,0x20,0x49,0x8B,0xCC,0xE8,0xF1,0x17,0xD5,0 };
    static constexpr std::uint8_t graph_call[] {
        0x48,0x8B,0x8B,0x50,0x04,0,0,0x48,0x8D,0x55,0xA8,0xE8,0x75,0xE5,0x95,0 };
    static constexpr std::uint8_t world_notify[] {
        0x40,0x53,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9,0xE8,0xF2,0x3A,0x13,0,
        0x8B,0x93,0x60,0x17,0,0 };
    static constexpr std::uint8_t remove_hud[] {0x48,0x83,0xC4,0x20,0x5B,0xE9,0x1F,0xBE,0x1E,0};
    static const std::array<KnifeResourceEvidenceWindow, 7> windows {{
        {kKnifeSetModelRva, set_model}, {0x934E48, notification},
        {kKnifeRecollectRva, recollect}, {0x9041E0, skeleton_call}, {0x90431B, graph_call},
        {0x7F6A70, world_notify}, {0x7F6AD7, remove_hud} }};
    return windows;
}

bool VerifyKnifeNativeEvidence(std::uintptr_t client, std::size_t size, KnifeGraphReader& reader) noexcept
{
    if (!client || size > std::numeric_limits<std::uintptr_t>::max() - client) return false;
    for (const auto& window : KnifeNativeEvidenceWindows())
    {
        std::array<std::uint8_t, 64> bytes {};
        if (window.rva > size || window.bytes.size() > size - window.rva || window.bytes.size() > bytes.size() ||
            !reader.Read(client + window.rva, bytes.data(), window.bytes.size()) ||
            !std::equal(window.bytes.begin(), window.bytes.end(), bytes.begin())) return false;
    }
    return true;
}

bool KnifeNativeHost::InvokeSetModel(std::uintptr_t entry, std::uintptr_t entity, const char* path) noexcept
{
    if (!entry || !entity || !path) return false;
    __try { reinterpret_cast<void(__fastcall*)(void*, const char*)>(entry)(reinterpret_cast<void*>(entity), path); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool KnifeNativeHost::InvokeRecollect(std::uintptr_t entry, std::uintptr_t controller) noexcept
{
    if (!entry || !controller) return false;
    __try { reinterpret_cast<void(__fastcall*)(void*)>(entry)(reinterpret_cast<void*>(controller)); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool KnifeNativeHost::CompareExchangeSymbol(std::uintptr_t address, std::uintptr_t expected, std::uintptr_t desired) noexcept
{
    if (!address || address % alignof(void*) || !expected || !desired) return false;
    __try
    {
        return InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(address),
            reinterpret_cast<void*>(desired), reinterpret_cast<void*>(expected)) == reinterpret_cast<void*>(expected);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool KnifeNativePairAccess::ContextValid() const noexcept
{
    if (!context_.binding.Valid() || !FindKnifeModelAsset(context_.source_definition) ||
        !FindKnifeModelAsset(context_.target_definition) || !context_.symbols.weapon_type_name ||
        !context_.symbols.source_type || !context_.symbols.target_type || !context_.slot_symbol ||
        !context_.hud_body_controller || context_.hud_body_controller == context_.binding.body_animation_controller)
        return false;
    const std::array fields {context_.resources.world, context_.resources.hud, context_.resources.arms};
    for (std::size_t i = 0; i < fields.size(); ++i)
    {
        // Schema 2000899: hModel +0xA0, ModelName +0xA8. Host validates that
        // each captured field still belongs to its entity's current scene node.
        if (!fields[i] || fields[i] > std::numeric_limits<std::uintptr_t>::max() - 16 ||
            context_.model_names[i] != fields[i] + 8) return false;
        for (std::size_t j = 0; j < i; ++j) if (fields[i] == fields[j]) return false;
    }
    return true;
}

bool KnifeNativePairAccess::EvidenceVerified() noexcept
{
    return ContextValid() && VerifyKnifeNativeEvidence(context_.client, context_.client_size, host_) &&
        VerifyKnifeResourceEvidence(context_.client, context_.client_size, host_);
}

bool KnifeNativePairAccess::Alive(const KnifeModelBinding& binding) noexcept
{
    std::uintptr_t graph = 0;
    return ContextValid() && binding == context_.binding && host_.BindingAlive(context_) &&
        ReadKnifeBodyGraph(binding.body_animation_controller, true, host_, graph) && graph == binding.graph_instance;
}

bool KnifeNativePairAccess::RebindPreparedHud(const KnifeNativeContext& next) noexcept
{
    const auto& a = context_.binding;
    const auto& b = next.binding;
    if (!touched_[0] || touched_[1] || !owned_models_[0] || retire_failed_ || retire_attempted_[0] ||
        a.scene_epoch != b.scene_epoch || a.xuid != b.xuid || a.pawn != b.pawn || a.world != b.world || a.arms != b.arms ||
        a.weapon_graph_controller != b.weapon_graph_controller || a.body_animation_controller != b.body_animation_controller ||
        a.hud_weapon == b.hud_weapon || context_.source_definition != next.source_definition || context_.target_definition != next.target_definition ||
        context_.resources.world != next.resources.world || context_.resources.arms != next.resources.arms ||
        context_.model_names[0] != next.model_names[0] || context_.model_names[2] != next.model_names[2] ||
        context_.symbols.weapon_type_name != next.symbols.weapon_type_name || context_.symbols.source_type != next.symbols.source_type ||
        context_.symbols.target_type != next.symbols.target_type || context_.slot_symbol != next.slot_symbol ||
        context_.client != next.client || context_.client_size != next.client_size || !host_.WorldBindingAlive(context_)) return false;
    int item_definition = 0;
    if (!item_definition_touched_ || !host_.ReadWorldItemDefinition(context_, item_definition) ||
        item_definition != context_.target_definition) return false;
    const auto old_hud = host_.BladePresence(context_, KnifeBladeRole::Hud);
    if (old_hud != KnifeBladePresence::Gone && old_hud != KnifeBladePresence::Replaced) return false;
    KnifeNativePairAccess candidate(next, host_);
    KnifePairState state;
    if (!candidate.EvidenceVerified() || !candidate.SafePoint(b) || !candidate.Read(b, state) ||
        state.world_definition != next.target_definition || state.world_model != owned_models_[0] ||
        !state.attachments_valid) return false;
    // No raw handles are replayed. Keep the world restore ledger and replace
    // only the now-proven HUD/graph context before touching that new HUD.
    context_ = next;
    if (state.hud_definition == next.target_definition)
    {
        // A successor that inherited OUR prepared world model also belongs to
        // this restore ledger, even though it needs no second HUD SetModel.
        touched_[1] = true;
        owned_models_[1] = state.hud_model;
    }
    return true;
}

bool KnifeNativePairAccess::SafePoint(const KnifeModelBinding& binding) noexcept
{ return Alive(binding) && host_.AtSafePoint(binding); }

bool KnifeNativePairAccess::ModelNames(std::array<int, 3>& definitions) noexcept
{
    definitions = {};
    for (std::size_t i = 0; i < context_.model_names.size(); ++i)
    {
        if (!ModelName(i, definitions[i])) return false;
        if (i < 2)
        {
            if (definitions[i] != context_.source_definition && definitions[i] != context_.target_definition)
                return false;
        }
        else if (definitions[i]) return false; // Arms must never become a knife model.
    }
    return true;
}

bool KnifeNativePairAccess::ModelName(std::size_t role, int& definition) noexcept
{
    definition = 0;
    if (role >= context_.model_names.size()) return false;
    std::uintptr_t name = 0, confirmed = 0;
    if (!host_.Read(context_.model_names[role], &name, sizeof(name)) || !name ||
        name > std::numeric_limits<std::uintptr_t>::max() - 256) return false;
    std::array<char, 256> text {};
    std::size_t length = 0;
    for (; length < text.size(); ++length)
    {
        if (!host_.Read(name + length, &text[length], 1)) return false;
        if (!text[length]) break;
    }
    if (!length || length == text.size() || !host_.Read(context_.model_names[role], &confirmed, sizeof(confirmed)) ||
        confirmed != name) return false;
    if (const auto* asset = FindKnifeModelAssetByPath({text.data(), length})) definition = asset->definition;
    return true;
}

bool KnifeNativePairAccess::Blade(std::size_t role, int& definition, std::uintptr_t& model) noexcept
{
    definition = 0; model = 0;
    if (role > 1) return false;
    const auto field = role ? context_.resources.hud : context_.resources.world;
    std::uintptr_t resource = 0, again = 0;
    int confirmed = 0;
    return ModelName(role, definition) && host_.Read(field, &resource, sizeof(resource)) && resource &&
        host_.Read(resource, &model, sizeof(model)) && model && model != resource &&
        host_.Read(field, &again, sizeof(again)) && again == resource &&
        ModelName(role, confirmed) && confirmed == definition;
}

KnifeGraphProbeResult KnifeNativePairAccess::Parameter(std::uintptr_t expected) noexcept
{
    if (expected != context_.symbols.source_type && expected != context_.symbols.target_type) return {};
    auto symbols = context_.symbols;
    symbols.source_type = expected;
    return ProbeKnifeWeaponType(context_.binding, symbols, true, host_);
}

bool KnifeNativePairAccess::Read(const KnifeModelBinding& binding, KnifePairState& out) noexcept
{
    out = {};
    if (!Alive(binding)) return false;
    std::array<int, 3> names {}, confirmed {};
    KnifeResourcePair resources;
    int item_definition = 0;
    if (!ModelNames(names) || !ProbeKnifeResourcePair(binding, context_.resources, true, host_, resources) ||
        !host_.ReadWorldItemDefinition(context_, item_definition) ||
        (item_definition != context_.source_definition && item_definition != context_.target_definition)) return false;
    auto parameter = Parameter(context_.symbols.source_type);
    if (parameter.status == KnifeGraphProbeStatus::SourceTypeMismatch) parameter = Parameter(context_.symbols.target_type);
    if (!parameter.SourceVerified()) return false;
    KnifeSecondarySkeleton secondary;
    const bool attached = ProbeKnifeSecondarySkeleton(binding, context_.hud_body_controller,
        context_.slot_symbol, true, host_, secondary) && host_.AttachmentsValid(binding);
    if (!ModelNames(confirmed) || confirmed != names || !Alive(binding)) return false;
    out = {names[0], names[1], resources.world_model, resources.hud_model, resources.arms_model,
        parameter.parameter.value, resources.blade_skeletons_match && resources.hud_skeleton_in_graph, attached,
        item_definition};
    return true;
}

std::uintptr_t KnifeNativePairAccess::ExistingAnimationType(const KnifeModelAsset& asset) noexcept
{
    const auto* canonical = FindKnifeModelAsset(asset.definition);
    if (!canonical || asset.model != canonical->model || asset.skeleton != canonical->skeleton ||
        asset.animation_class != canonical->animation_class) return 0;
    if (asset.definition == context_.source_definition) return context_.symbols.source_type;
    if (asset.definition == context_.target_definition) return context_.symbols.target_type;
    return 0;
}

bool KnifeNativePairAccess::ReadItemDefinition(const KnifeModelBinding& binding, int& definition) noexcept
{
    definition = 0;
    return Alive(binding) && host_.ReadWorldItemDefinition(context_, definition) &&
        (definition == context_.source_definition || definition == context_.target_definition);
}

bool KnifeNativePairAccess::CompareItemDefinition(const KnifeModelBinding& binding,
    int expected, int desired) noexcept
{
    if ((expected != context_.source_definition && expected != context_.target_definition) ||
        (desired != context_.source_definition && desired != context_.target_definition) ||
        !SafePoint(binding) || !EvidenceVerified()) return false;
    int before = 0, after = 0;
    if (!host_.ReadWorldItemDefinition(context_, before) || before != expected) return false;
    if (expected != desired && !host_.CompareExchangeWorldItemDefinition(context_, expected, desired)) return false;
    if (!host_.ReadWorldItemDefinition(context_, after) || after != desired || !Alive(binding)) return false;
    if (desired == context_.target_definition && desired != context_.source_definition)
        item_definition_touched_ = true;
    else if (desired == context_.source_definition)
        item_definition_touched_ = false;
    return true;
}

bool KnifeNativePairAccess::SetBladeModel(const KnifeModelBinding& binding, KnifeBladeRole role,
    const KnifeModelAsset& asset) noexcept
{
    if ((role != KnifeBladeRole::World && role != KnifeBladeRole::Hud) || !ExistingAnimationType(asset) ||
        !SafePoint(binding) || !EvidenceVerified()) return false;
    KnifePairState before, after;
    if (!Read(binding, before) || !before.attachments_valid) return false;
    const auto entity = role == KnifeBladeRole::World ? binding.world.address : binding.hud_weapon.address;
    // Only the engine wrapper owns/replaces strong handles and sends +0x780.
    const auto* canonical = FindKnifeModelAsset(asset.definition);
    if (!SafePoint(binding)) return false;
    const auto index = role == KnifeBladeRole::World ? 0u : 1u;
    touched_[index] = true;
    const bool called = host_.InvokeSetModel(context_.client + kKnifeSetModelRva, entity, canonical->model.data());
    // Record a successful blade change even if the call invalidated the OTHER
    // blade or graph. Otherwise partial failure could strand the world model.
    int actual_definition = 0; std::uintptr_t actual_model = 0;
    if (Blade(index, actual_definition, actual_model) && actual_definition == asset.definition)
        owned_models_[index] = actual_model;
    if (!called || !Read(binding, after) || after.arms_model != before.arms_model) return false;
    return role == KnifeBladeRole::World
        ? after.world_definition == asset.definition && after.hud_model == before.hud_model
        : after.hud_definition == asset.definition && after.world_model == before.world_model;
}

bool KnifeNativePairAccess::Recollect(const KnifeModelBinding& binding) noexcept
{
    KnifePairState before, after;
    if (!SafePoint(binding) || !EvidenceVerified() || !Read(binding, before) || !before.attachments_valid ||
        before.world_definition != before.hud_definition || !SafePoint(binding) ||
        !host_.InvokeRecollect(context_.client + kKnifeRecollectRva, binding.body_animation_controller) ||
        !Read(binding, after)) return false;
    // Return from the engine function is insufficient; prove the new skeleton
    // has actually reached the same graph, without changing arms or either blade.
    auto expected = before; expected.secondary_matches_hud = true;
    return after == expected && after.secondary_matches_hud;
}

bool KnifeNativePairAccess::CompareAnimationType(const KnifeModelBinding& binding,
    std::uintptr_t expected, std::uintptr_t desired) noexcept
{
    if ((desired != context_.symbols.source_type && desired != context_.symbols.target_type) ||
        !SafePoint(binding)) return false;
    const auto before = Parameter(expected);
    if (!before.SourceVerified() || before.parameter.node % alignof(void*) || !host_.AttachmentsValid(binding)) return false;
    // No cached address: validate type, definition/index, graph owner and value
    // immediately before a compare/exchange, then re-probe the same node after.
    if (expected != desired && (!SafePoint(binding) ||
        !host_.CompareExchangeSymbol(before.parameter.node + 0x18, expected, desired))) return false;
    const auto after = Parameter(desired);
    auto wanted = before.parameter; wanted.value = desired;
    return after.SourceVerified() && after.parameter == wanted && Alive(binding);
}

KnifeRetireResult KnifeNativePairAccess::RetireDetached() noexcept
{
    if (!touched_[0] && !touched_[1] && !item_definition_touched_ && !retire_failed_ && !retire_models_changed_)
        return retire_discarded_ ? KnifeRetireResult::Discarded : KnifeRetireResult::Restored;
    const auto epoch = host_.SceneEpoch();
    if (!epoch) return KnifeRetireResult::Waiting;
    if (epoch != context_.binding.scene_epoch)
    {
        touched_ = {}; owned_models_ = {}; item_definition_touched_ = false; // No reads of pre-seek addresses.
        retire_failed_ = false; retire_models_changed_ = false; retire_discarded_ = true;
        return KnifeRetireResult::Discarded;
    }
    if (retire_failed_) return KnifeRetireResult::Failed;
    if (!host_.AtRetirementPoint()) return KnifeRetireResult::Waiting;
    if (!EvidenceVerified()) { retire_failed_ = true; return KnifeRetireResult::Failed; }
    if (item_definition_touched_)
    {
        const auto presence = host_.BladePresence(context_, KnifeBladeRole::World);
        if (presence == KnifeBladePresence::Gone || presence == KnifeBladePresence::Replaced)
        { item_definition_touched_ = false; retire_discarded_ = true; }
        else
        {
            int definition = 0;
            if (presence != KnifeBladePresence::Live || !host_.ReadWorldItemDefinition(context_, definition))
                retire_failed_ = true;
            else if (definition == context_.source_definition)
                item_definition_touched_ = false;
            else if (definition != context_.target_definition)
            { item_definition_touched_ = false; retire_discarded_ = true; }
            else if (item_definition_retire_attempted_ || !host_.AtRetirementPoint() || host_.SceneEpoch() != epoch)
                retire_failed_ = true;
            else
            {
                item_definition_retire_attempted_ = true;
                if (!host_.CompareExchangeWorldItemDefinition(context_, context_.target_definition,
                        context_.source_definition) ||
                    !host_.ReadWorldItemDefinition(context_, definition) || definition != context_.source_definition)
                    retire_failed_ = true;
                else item_definition_touched_ = false;
            }
        }
    }
    for (std::size_t i = 0; i < 2; ++i)
    {
        if (!touched_[i]) continue;
        const auto role = i == 0 ? KnifeBladeRole::World : KnifeBladeRole::Hud;
        auto presence = host_.BladePresence(context_, role);
        if (presence == KnifeBladePresence::Gone || presence == KnifeBladePresence::Replaced)
        { touched_[i] = false; retire_discarded_ = true; continue; }
        int definition = 0; std::uintptr_t model = 0;
        if (presence != KnifeBladePresence::Live || !Blade(i, definition, model))
        { retire_failed_ = true; continue; }
        if (definition == context_.source_definition)
        { touched_[i] = false; retire_models_changed_ |= owned_models_[i] != 0; continue; }
        if (definition != context_.target_definition || (owned_models_[i] && model != owned_models_[i]))
        { touched_[i] = false; retire_discarded_ = true; continue; } // Respect a competing writer.
        if (!owned_models_[i] || retire_attempted_[i]) { retire_failed_ = true; continue; }
        if (!host_.AtRetirementPoint() || host_.SceneEpoch() != epoch) return KnifeRetireResult::Waiting;
        if (host_.BladePresence(context_, role) != KnifeBladePresence::Live)
        { retire_failed_ = true; continue; }
        retire_attempted_[i] = true;
        const auto entity = i == 0 ? context_.binding.world.address : context_.binding.hud_weapon.address;
        const bool called = host_.InvokeSetModel(context_.client + kKnifeSetModelRva, entity,
            FindKnifeModelAsset(context_.source_definition)->model.data());
        if (!called || host_.BladePresence(context_, role) != KnifeBladePresence::Live ||
            !Blade(i, definition, model) || definition != context_.source_definition)
        { retire_failed_ = true; continue; }
        touched_[i] = false; retire_models_changed_ = true;
    }
    // A remaining valid HUD registration needs explicit recollection even when
    // the old graph was replaced. The host returns a freshly owned graph or
    // nothing; no cached node pointer is replayed into the new instance.
    KnifeModelBinding current;
    if (!host_.AtRetirementPoint() || host_.SceneEpoch() != epoch) return KnifeRetireResult::Waiting;
    const auto graph_state = retire_models_changed_ && !retire_failed_ ? host_.RetirementGraph(context_, current) : KnifeRetirementGraph::Absent;
    if (graph_state == KnifeRetirementGraph::Unreadable) retire_failed_ = true;
    if (retire_models_changed_ && !retire_failed_ && graph_state == KnifeRetirementGraph::Live)
    {
        KnifeSecondarySkeleton secondary;
        if (!host_.AtRetirementPoint() || host_.SceneEpoch() != epoch ||
            !ProbeKnifeSecondarySkeleton(current, context_.hud_body_controller, context_.slot_symbol, true, host_, secondary) ||
            !host_.InvokeRecollect(context_.client + kKnifeRecollectRva, current.body_animation_controller))
            retire_failed_ = true;
        else
        {
            KnifeModelBinding confirmed;
            // Retirement can outlive the world entity. Only the still-owned HUD
            // skeleton needs to be present in this freshly owned arms graph.
            if (host_.RetirementGraph(context_, confirmed) != KnifeRetirementGraph::Live || confirmed != current ||
                !ProbeKnifeHudSkeleton(current, context_.resources.hud, true, host_)) retire_failed_ = true;
            auto symbols = context_.symbols; symbols.source_type = context_.symbols.target_type;
            const auto parameter = ProbeKnifeWeaponType(current, symbols, true, host_);
            if (!retire_failed_ && parameter.SourceVerified() && context_.symbols.source_type != context_.symbols.target_type)
            {
                // If the native updater has already selected another weapon,
                // leave its value alone; it is no longer our animation branch.
                if (!host_.AtRetirementPoint() || host_.SceneEpoch() != epoch ||
                    !host_.CompareExchangeSymbol(parameter.parameter.node + 0x18,
                    context_.symbols.target_type, context_.symbols.source_type)) retire_failed_ = true;
                else
                {
                    const auto after = ProbeKnifeWeaponType(current, context_.symbols, true, host_);
                    auto expected = parameter.parameter; expected.value = context_.symbols.source_type;
                    if (!after.SourceVerified() || after.parameter != expected ||
                        host_.RetirementGraph(context_, confirmed) != KnifeRetirementGraph::Live || confirmed != current)
                        retire_failed_ = true;
                }
            }
            else if (!parameter.SourceVerified() && parameter.status != KnifeGraphProbeStatus::SourceTypeMismatch)
                retire_failed_ = true;
        }
    }
    if (!retire_failed_) retire_models_changed_ = false;
    return retire_failed_ ? KnifeRetireResult::Failed : retire_discarded_ ? KnifeRetireResult::Discarded : KnifeRetireResult::Restored;
}

bool KnifeNativePairAccess::Maintain(const KnifeModelBinding& binding, const KnifePairState& committed,
    std::uintptr_t source, std::uintptr_t target) noexcept
{
    if (source != context_.symbols.source_type || target != context_.symbols.target_type || !SafePoint(binding)) return false;
    KnifeResourcePair resources;
    KnifeSecondarySkeleton secondary;
    // The committed loaded-model identities prove the same immutable paths.
    // No per-frame string reads, asset lookups, SetModel or resource recollect.
    int item_definition = 0;
    if (!ProbeKnifeResourcePair(binding, context_.resources, true, host_, resources) ||
        resources.world_model != committed.world_model || resources.hud_model != committed.hud_model ||
        resources.arms_model != committed.arms_model || !resources.blade_skeletons_match || !resources.hud_skeleton_in_graph ||
        !ProbeKnifeSecondarySkeleton(binding, context_.hud_body_controller, context_.slot_symbol, true, host_, secondary) ||
        !host_.AttachmentsValid(binding) || !host_.ReadWorldItemDefinition(context_, item_definition) ||
        (item_definition != context_.source_definition && item_definition != context_.target_definition)) return false;
    if (item_definition != context_.target_definition &&
        !CompareItemDefinition(binding, item_definition, context_.target_definition)) return false;
    auto parameter = Parameter(source);
    if (parameter.status == KnifeGraphProbeStatus::SourceTypeMismatch) parameter = Parameter(target);
    return parameter.SourceVerified() && CompareAnimationType(binding, parameter.parameter.value, target);
}
}
