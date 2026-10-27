use anyhow::{anyhow, bail, Context as _, Result};
use serde::{Deserialize, Serialize};
use source2_demo::prelude::*;
use source2_demo::proto::{
    c_demo_string_tables::ItemsT, CDemoStringTables, CSvcMsgCreateStringTable,
};
use source2_demo::writer::{
    insert_create_string_table_entry, DemoRewriter, DemoWriter, MessageRewrite, RewriteInterests,
    StringTableEntryUpdate,
};
use std::collections::{BTreeSet, HashMap, HashSet};
use std::fs::File;
use std::io::{BufReader, BufWriter, Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};
use std::sync::LazyLock;
use std::time::Instant;

mod keychains;
mod killfeed;
use keychains::{KeychainRule, RequestKeychain};

const IO_BUFFER_CAPACITY: usize = 8 * 1024 * 1024;
const IN_MEMORY_DEMO_LIMIT: u64 = 768 * 1024 * 1024;
const WEAPON_SLOT_COUNT: usize = 64;
const ECON_ATTRIBUTE_SLOT_COUNT: usize = 64;
const STICKER_SLOT_COUNT: usize = 5;
const STICKER_ATTRIBUTE_COMPONENT_COUNT: usize = 7;

static WEAPON_FIELDS: LazyLock<Vec<String>> = LazyLock::new(|| {
    (0..WEAPON_SLOT_COUNT)
        .map(|slot| format!("m_pWeaponServices.m_hMyWeapons.{slot:04}"))
        .collect()
});

struct PaintAttributeFields {
    definition: String,
    raw: String,
    initial: String,
}

fn paint_attribute_fields(prefix: &str) -> Vec<PaintAttributeFields> {
    (0..ECON_ATTRIBUTE_SLOT_COUNT)
        .map(|slot| {
            let base = format!("{prefix}.m_NetworkedDynamicAttributes.m_Attributes.{slot:04}");
            PaintAttributeFields {
                definition: format!("{base}.m_iAttributeDefinitionIndex"),
                raw: format!("{base}.m_iRawValue32"),
                initial: format!("{base}.m_flInitialValue"),
            }
        })
        .collect()
}

static WEAPON_PAINT_FIELDS: LazyLock<Vec<PaintAttributeFields>> =
    LazyLock::new(|| paint_attribute_fields("m_AttributeManager.m_Item"));
static GLOVE_PAINT_FIELDS: LazyLock<Vec<PaintAttributeFields>> =
    LazyLock::new(|| paint_attribute_fields("m_EconGloves"));

#[derive(Debug, Deserialize)]
struct Request {
    schema_version: u32,
    input_dem: PathBuf,
    output_dem: PathBuf,
    #[serde(default)]
    cs2_game_dir: Option<PathBuf>,
    passes: Vec<RequestPass>,
}

#[derive(Debug, Deserialize)]
struct RequestPass {
    steam_id64: String,
    items: Vec<RequestItem>,
    #[serde(default)]
    keychains: Vec<RequestKeychain>,
}

#[derive(Clone, Copy, Debug, Deserialize)]
#[serde(default)]
struct StickerTransform {
    wear: f32,
    scale: f32,
    rotation: f32,
    custom_position: bool,
    offset_x: f32,
    offset_y: f32,
    schema: u32,
}

impl Default for StickerTransform {
    fn default() -> Self {
        Self {
            wear: 0.0000001,
            scale: 1.0,
            rotation: 0.0,
            custom_position: false,
            offset_x: 0.0,
            offset_y: 0.0,
            schema: 0,
        }
    }
}

impl StickerTransform {
    fn normalized(self) -> Self {
        let finite_or = |value: f32, fallback: f32| {
            if value.is_finite() {
                value
            } else {
                fallback
            }
        };
        Self {
            wear: finite_or(self.wear, 0.0000001).clamp(0.0000001, 1.0),
            scale: finite_or(self.scale, 1.0).clamp(0.1, 5.0),
            rotation: finite_or(self.rotation, 0.0).clamp(0.0, 360.0),
            custom_position: self.custom_position,
            // Borrowed fifth-slot homes add a calibrated anchor shift after
            // the user's [-0.5, 0.5] delta. Do not clamp that serialized
            // material offset back into the editor slider's input range.
            offset_x: finite_or(self.offset_x, 0.0),
            offset_y: finite_or(self.offset_y, 0.0),
            schema: self.schema,
        }
    }
}

#[derive(Clone, Debug, Deserialize)]
struct RequestItem {
    definition_index: u16,
    replacement_definition_index: Option<u16>,
    #[serde(default)]
    preserve_source_definition: bool,
    paint_kit: i32,
    #[serde(default)]
    quality: i32,
    pattern_seed: f32,
    wear: f32,
    #[serde(default)]
    mesh_group_mask: Option<u64>,
    #[serde(default)]
    custom_name: String,
    #[serde(default)]
    stickers: [i32; STICKER_SLOT_COUNT],
    #[serde(default)]
    sticker_transforms: [StickerTransform; STICKER_SLOT_COUNT],
    team: String,
}

#[derive(Debug, Serialize)]
struct Response {
    ok: bool,
    error: Option<String>,
    matched_player_count: usize,
    skipped_player_count: usize,
    matched_knife_request_count: usize,
    matched_glove_request_count: usize,
    requested_sticker_slot_count: usize,
    pawn_count: usize,
    knife_count: usize,
    matched_knife_death_count: usize,
    rewritten_knife_death_count: usize,
    verified_death_event_count: usize,
    glove_count: usize,
    verified_glove_record_count: usize,
    verified_glove_created_count: usize,
    keychain_weapon_count: usize,
    verified_keychain_record_count: usize,
    verified_keychain_created_count: usize,
    weapon_count: usize,
    painted_glove_passthrough_count: usize,
    painted_weapon_passthrough_count: usize,
    sticker_weapon_count: usize,
    sticker_slot_count: usize,
    created_sticker_slot_count: usize,
    missing_sticker_slot_count: usize,
    animation_resource_count: usize,
    animation_resource_source: String,
    input_mode: String,
    input_read_ms: u64,
    discovery_ms: u64,
    rewrite_ms: u64,
    verification_ms: u64,
    total_ms: u64,
}

#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
enum Team {
    T,
    Ct,
    Any,
}

impl Team {
    fn parse(value: &str) -> Result<Self> {
        match value.to_ascii_uppercase().as_str() {
            "T" => Ok(Self::T),
            "CT" => Ok(Self::Ct),
            "ANY" => Ok(Self::Any),
            _ => bail!("unsupported team {value:?}"),
        }
    }

    fn number(self) -> Option<u8> {
        match self {
            Self::T => Some(2),
            Self::Ct => Some(3),
            Self::Any => None,
        }
    }
}

#[derive(Clone, Debug)]
struct Cosmetic {
    definition: u16,
    preserve_source_definition: bool,
    paint_kit: i32,
    quality: i32,
    seed: f32,
    wear: f32,
    mesh_group_mask: Option<u64>,
    custom_name: String,
    stickers: [i32; STICKER_SLOT_COUNT],
    sticker_transforms: [StickerTransform; STICKER_SLOT_COUNT],
}

#[derive(Default)]
struct TeamPlan {
    knife: Option<Cosmetic>,
    glove: Option<Cosmetic>,
    weapons: HashMap<u16, Cosmetic>,
}

#[derive(Default)]
struct PlayerPlan {
    by_team: HashMap<Team, TeamPlan>,
    keychains: HashMap<u16, RequestKeychain>,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct KnifeVariant {
    model: u64,
    subclass: u32,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct KnifeAsset {
    definition: u16,
    model: &'static str,
    skeleton: &'static str,
}

const KNIFE_ASSETS: &[KnifeAsset] = &[
    KnifeAsset {
        definition: 42,
        model: "weapons/models/knife/knife_default_ct/weapon_knife_default_ct.vmdl",
        skeleton: "animation/skeletons/weapons/knife_default_ct.vnmskel",
    },
    KnifeAsset {
        definition: 59,
        model: "weapons/models/knife/knife_default_t/weapon_knife_default_t.vmdl",
        skeleton: "animation/skeletons/weapons/knife_default_t.vnmskel",
    },
    KnifeAsset {
        definition: 500,
        model: "weapons/models/knife/knife_bayonet/weapon_knife_bayonet.vmdl",
        skeleton: "animation/skeletons/weapons/knife_bayonet.vnmskel",
    },
    KnifeAsset {
        definition: 503,
        model: "weapons/models/knife/knife_css/weapon_knife_css.vmdl",
        skeleton: "animation/skeletons/weapons/knife_css.vnmskel",
    },
    KnifeAsset {
        definition: 505,
        model: "weapons/models/knife/knife_flip/weapon_knife_flip.vmdl",
        skeleton: "animation/skeletons/weapons/knife_flip.vnmskel",
    },
    KnifeAsset {
        definition: 506,
        model: "weapons/models/knife/knife_gut/weapon_knife_gut.vmdl",
        skeleton: "animation/skeletons/weapons/knife_gut.vnmskel",
    },
    KnifeAsset {
        definition: 507,
        model: "weapons/models/knife/knife_karambit/weapon_knife_karambit.vmdl",
        skeleton: "animation/skeletons/weapons/knife_karambit.vnmskel",
    },
    KnifeAsset {
        definition: 508,
        model: "weapons/models/knife/knife_m9/weapon_knife_m9.vmdl",
        skeleton: "animation/skeletons/weapons/knife_m9.vnmskel",
    },
    KnifeAsset {
        definition: 509,
        model: "weapons/models/knife/knife_tactical/weapon_knife_tactical.vmdl",
        skeleton: "animation/skeletons/weapons/knife_tactical.vnmskel",
    },
    KnifeAsset {
        definition: 512,
        model: "weapons/models/knife/knife_falchion/weapon_knife_falchion.vmdl",
        skeleton: "animation/skeletons/weapons/knife_falchion.vnmskel",
    },
    KnifeAsset {
        definition: 514,
        model: "weapons/models/knife/knife_bowie/weapon_knife_bowie.vmdl",
        skeleton: "animation/skeletons/weapons/knife_bowie.vnmskel",
    },
    KnifeAsset {
        definition: 515,
        model: "weapons/models/knife/knife_butterfly/weapon_knife_butterfly.vmdl",
        skeleton: "animation/skeletons/weapons/knife_butterfly.vnmskel",
    },
    KnifeAsset {
        definition: 516,
        model: "weapons/models/knife/knife_push/weapon_knife_push.vmdl",
        skeleton: "animation/skeletons/weapons/knife_push.vnmskel",
    },
    KnifeAsset {
        definition: 517,
        model: "weapons/models/knife/knife_cord/weapon_knife_cord.vmdl",
        skeleton: "animation/skeletons/weapons/knife_cord.vnmskel",
    },
    KnifeAsset {
        definition: 518,
        model: "weapons/models/knife/knife_canis/weapon_knife_canis.vmdl",
        skeleton: "animation/skeletons/weapons/knife_canis.vnmskel",
    },
    KnifeAsset {
        definition: 519,
        model: "weapons/models/knife/knife_ursus/weapon_knife_ursus.vmdl",
        skeleton: "animation/skeletons/weapons/knife_ursus.vnmskel",
    },
    KnifeAsset {
        definition: 520,
        model: "weapons/models/knife/knife_navaja/weapon_knife_navaja.vmdl",
        skeleton: "animation/skeletons/weapons/knife_navaja.vnmskel",
    },
    KnifeAsset {
        definition: 521,
        model: "weapons/models/knife/knife_outdoor/weapon_knife_outdoor.vmdl",
        skeleton: "animation/skeletons/weapons/knife_outdoor.vnmskel",
    },
    KnifeAsset {
        definition: 522,
        model: "weapons/models/knife/knife_stiletto/weapon_knife_stiletto.vmdl",
        skeleton: "animation/skeletons/weapons/knife_stiletto.vnmskel",
    },
    KnifeAsset {
        definition: 523,
        model: "weapons/models/knife/knife_talon/weapon_knife_talon.vmdl",
        skeleton: "animation/skeletons/weapons/knife_talon.vnmskel",
    },
    KnifeAsset {
        definition: 525,
        model: "weapons/models/knife/knife_skeleton/weapon_knife_skeleton.vmdl",
        skeleton: "animation/skeletons/weapons/knife_skeleton.vnmskel",
    },
    KnifeAsset {
        definition: 526,
        model: "weapons/models/knife/knife_kukri/weapon_knife_kukri.vmdl",
        skeleton: "animation/skeletons/weapons/knife_kukri.vnmskel",
    },
];

#[derive(Default)]
struct PawnDiscovery {
    controllers: HashSet<u32>,
    weapons: HashSet<u32>,
    teams: HashSet<u8>,
    glove_definitions_by_team: HashMap<u8, u16>,
    glove_paint_attributes_by_team: HashMap<u8, PaintAttributeLayout>,
    glove_definitions: HashSet<u16>,
    glove_item_id_highs: HashSet<u32>,
    glove_item_id_lows: HashSet<u32>,
    glove_account_ids: HashSet<u32>,
    glove_qualities: HashSet<i32>,
    glove_attribute_counts: HashSet<usize>,
    glove_paint_values: [HashSet<u32>; 3],
    glove_custom_names: HashSet<String>,
    glove_body_group_choices: [HashSet<i32>; 2],
    glove_change_counters: HashSet<u8>,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
struct PaintAttributeLayout {
    count: usize,
    indices: [Option<usize>; 3],
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct MaterializedPaintAttributes {
    count: usize,
    indices: [usize; 3],
}

#[derive(Default)]
struct WeaponDiscovery {
    definition: u16,
    paint_attributes: PaintAttributeLayout,
    sticker_slots: [bool; STICKER_SLOT_COUNT],
    sticker_kits: [HashSet<u32>; STICKER_SLOT_COUNT],
    sticker_attribute_indices:
        [[HashSet<usize>; STICKER_ATTRIBUTE_COMPONENT_COUNT]; STICKER_SLOT_COUNT],
    sticker_attribute_values:
        [[HashSet<u32>; STICKER_ATTRIBUTE_COMPONENT_COUNT]; STICKER_SLOT_COUNT],
}

#[derive(Default)]
struct Discovery {
    controller_steam: HashMap<u32, u64>,
    controller_pawns: HashMap<u32, HashSet<u32>>,
    pawns: HashMap<u32, PawnDiscovery>,
    weapons: HashMap<u32, WeaponDiscovery>,
    knives: HashSet<u32>,
    anim_asset_table_present: bool,
    anim_asset_initial_count: Option<i32>,
    anim_asset_initial_keys: HashSet<String>,
}

fn integer(value: &FieldValue) -> Option<u64> {
    value.try_into().ok()
}

fn field_integer(entity: &Entity, name: &str) -> Option<u64> {
    entity.try_get_property(name).and_then(integer)
}

fn positive_u32(value: &FieldValue) -> Option<u32> {
    match value {
        // EconItemAttribute stores a 32-bit value union. Integer attributes
        // such as "sticker slot N id" are exposed by the demo serializer as
        // a float field, but their integer bit pattern must remain untouched.
        FieldValue::Float(value) => (value.to_bits() > 0).then_some(value.to_bits()),
        _ => integer(value)
            .filter(|value| *value > 0 && *value <= u32::MAX as u64)
            .map(|value| value as u32),
    }
}

fn union_u32(value: &FieldValue) -> Option<u32> {
    match value {
        FieldValue::Float(value) => Some(value.to_bits()),
        _ => integer(value)
            .filter(|value| *value <= u32::MAX as u64)
            .map(|value| value as u32),
    }
}

fn sticker_component_from_definition(definition: u64) -> Option<(usize, usize)> {
    if let Some(relative) = definition.checked_sub(113).filter(|value| *value < 24) {
        let slot = (relative / 4) as usize;
        let component = (relative % 4) as usize;
        return (slot < STICKER_SLOT_COUNT).then_some((slot, component));
    }
    if let Some(relative) = definition.checked_sub(278).filter(|value| *value < 12) {
        let slot = (relative / 2) as usize;
        let component = 4 + (relative % 2) as usize;
        return (slot < STICKER_SLOT_COUNT).then_some((slot, component));
    }
    let slot = definition.checked_sub(290)? as usize;
    (slot < STICKER_SLOT_COUNT).then_some((slot, 6))
}

fn sticker_definition(logical_slot: usize, component: usize) -> u16 {
    match component {
        0..=3 => 113 + logical_slot as u16 * 4 + component as u16,
        4..=5 => 278 + logical_slot as u16 * 2 + (component - 4) as u16,
        6 => 290 + logical_slot as u16,
        _ => unreachable!("invalid sticker component {component}"),
    }
}

fn paint_attribute_slot(definition: u64) -> Option<usize> {
    match definition {
        6 => Some(0),
        7 => Some(1),
        8 => Some(2),
        _ => None,
    }
}

fn discover_paint_attribute_layout(
    entity: &Entity,
    count_field: &str,
    fields: &[PaintAttributeFields],
    layout: &mut PaintAttributeLayout,
) {
    let active_count = field_integer(entity, count_field).unwrap_or_default() as usize;
    layout.count = layout.count.max(active_count);
    for (attribute_index, field) in fields
        .iter()
        .enumerate()
        .take(active_count.min(fields.len()))
    {
        let Some(slot) = field_integer(entity, &field.definition).and_then(paint_attribute_slot)
        else {
            continue;
        };
        layout.indices[slot].get_or_insert(attribute_index);
    }
}

fn discover_glove_state(entity: &Entity, pawn: &mut PawnDiscovery) {
    if let Some(definition) = field_integer(entity, "m_EconGloves.m_iItemDefinitionIndex")
        .map(|value| value as u16)
        .filter(|value| *value != 0)
    {
        pawn.glove_definitions.insert(definition);
    }
    for (name, values) in [
        ("m_EconGloves.m_iItemIDHigh", &mut pawn.glove_item_id_highs),
        ("m_EconGloves.m_iItemIDLow", &mut pawn.glove_item_id_lows),
        ("m_EconGloves.m_iAccountID", &mut pawn.glove_account_ids),
    ] {
        if let Some(value) = field_integer(entity, name).map(|value| value as u32) {
            values.insert(value);
        }
    }
    if let Some(value) = field_integer(entity, "m_EconGloves.m_iEntityQuality") {
        pawn.glove_qualities.insert(value as i32);
    }
    let active_count = field_integer(
        entity,
        "m_EconGloves.m_NetworkedDynamicAttributes.m_Attributes",
    )
    .unwrap_or_default() as usize;
    pawn.glove_attribute_counts.insert(active_count);
    for field in GLOVE_PAINT_FIELDS
        .iter()
        .take(active_count.min(GLOVE_PAINT_FIELDS.len()))
    {
        let Some(slot) = field_integer(entity, &field.definition).and_then(paint_attribute_slot)
        else {
            continue;
        };
        for value in [field.raw.as_str(), field.initial.as_str()]
            .iter()
            .filter_map(|name| entity.try_get_property(name))
            .filter_map(union_u32)
        {
            pawn.glove_paint_values[slot].insert(value);
        }
    }
    if let Some(FieldValue::String(name)) = entity.try_get_property("m_EconGloves.m_szCustomName") {
        pawn.glove_custom_names.insert(name.clone());
    }
    for (slot, values) in pawn.glove_body_group_choices.iter_mut().enumerate() {
        if let Some(value) = field_integer(
            entity,
            &format!(
                "CBodyComponent.m_skeletonInstance.m_modelState.m_nBodyGroupChoices.{slot:04}"
            ),
        ) {
            values.insert(value as i32);
        }
    }
    if let Some(value) = field_integer(entity, "m_nEconGlovesChanged") {
        pawn.glove_change_counters.insert(value as u8);
    }
}

fn materialize_paint_attributes(layout: PaintAttributeLayout) -> MaterializedPaintAttributes {
    let mut count = layout.count;
    let mut indices = [0usize; 3];
    for (slot, discovered) in layout.indices.into_iter().enumerate() {
        indices[slot] = match discovered {
            Some(index) => index,
            None => {
                let index = count;
                count += 1;
                index
            }
        };
        count = count.max(indices[slot] + 1);
    }
    MaterializedPaintAttributes { count, indices }
}

fn discover_weapon_attributes(entity: &Entity, weapon: &mut WeaponDiscovery) {
    let count_field = "m_AttributeManager.m_Item.m_NetworkedDynamicAttributes.m_Attributes";
    let active_count = field_integer(entity, count_field).unwrap_or_default() as usize;
    discover_paint_attribute_layout(
        entity,
        count_field,
        &WEAPON_PAINT_FIELDS,
        &mut weapon.paint_attributes,
    );
    for (attribute_index, field) in WEAPON_PAINT_FIELDS
        .iter()
        .enumerate()
        .take(active_count.min(WEAPON_PAINT_FIELDS.len()))
    {
        let Some((logical_slot, component)) =
            field_integer(entity, &field.definition).and_then(sticker_component_from_definition)
        else {
            continue;
        };
        weapon.sticker_attribute_indices[logical_slot][component].insert(attribute_index);
        for value in [field.raw.as_str(), field.initial.as_str()]
            .iter()
            .filter_map(|name| entity.try_get_property(name))
            .filter_map(union_u32)
        {
            weapon.sticker_attribute_values[logical_slot][component].insert(value);
        }
        if component != 0 {
            continue;
        }
        for kit in [field.raw.as_str(), field.initial.as_str()]
            .iter()
            .filter_map(|name| entity.try_get_property(name))
            .filter_map(positive_u32)
        {
            weapon.sticker_slots[logical_slot] = true;
            weapon.sticker_kits[logical_slot].insert(kit);
        }
    }
}

impl Observer for Discovery {
    fn interests(&self) -> Interests {
        Interests::ENTITY_STATE
            | Interests::ENTITY_EVENTS
            | Interests::STRING_TABLE_STATE
            | Interests::STRING_TABLE_ENTRIES
            | Interests::REPLAY_END
    }

    fn on_entity(
        &mut self,
        _ctx: &Context,
        event: EntityEvents,
        entity: &Entity,
    ) -> ObserverResult {
        if event == EntityEvents::Deleted {
            return Ok(());
        }
        let class = entity.class().name();
        let handle = entity.handle();
        if class.contains("PlayerController") {
            if let Some(steam) = field_integer(entity, "m_steamID") {
                if steam != 0 {
                    self.controller_steam.insert(handle, steam);
                }
            }
            for name in ["m_hPlayerPawn", "m_hPawn"] {
                if let Some(pawn) = field_integer(entity, name).map(|value| value as u32) {
                    if pawn != u32::MAX && pawn != 0xFFFF_FFFE {
                        self.controller_pawns
                            .entry(handle)
                            .or_default()
                            .insert(pawn);
                    }
                }
            }
        } else if class == "CCSPlayerPawn" {
            let pawn = self.pawns.entry(handle).or_default();
            discover_glove_state(entity, pawn);
            if let Some(team) = field_integer(entity, "m_iTeamNum") {
                let team = team as u8;
                pawn.teams.insert(team);
                if let Some(definition) =
                    field_integer(entity, "m_EconGloves.m_iItemDefinitionIndex")
                        .map(|value| value as u16)
                        .filter(|value| *value != 0)
                {
                    pawn.glove_definitions_by_team.insert(team, definition);
                }
                discover_paint_attribute_layout(
                    entity,
                    "m_EconGloves.m_NetworkedDynamicAttributes.m_Attributes",
                    &GLOVE_PAINT_FIELDS,
                    pawn.glove_paint_attributes_by_team.entry(team).or_default(),
                );
            }
            for name in ["m_hController", "m_hOriginalController"] {
                if let Some(controller) = field_integer(entity, name).map(|value| value as u32) {
                    if controller != u32::MAX && controller != 0xFFFF_FFFE {
                        pawn.controllers.insert(controller);
                    }
                }
            }
            for name in WEAPON_FIELDS.iter() {
                if let Some(weapon) = field_integer(entity, name).map(|value| value as u32) {
                    if weapon != u32::MAX && weapon != 0xFFFF_FFFE {
                        pawn.weapons.insert(weapon);
                    }
                }
            }
        } else if class == "CKnife" {
            self.knives.insert(handle);
        }
        if let Some(definition) =
            field_integer(entity, "m_AttributeManager.m_Item.m_iItemDefinitionIndex")
                .map(|value| value as u16)
                .filter(|value| *value != 0)
        {
            let weapon = self.weapons.entry(handle).or_default();
            weapon.definition = definition;
            discover_weapon_attributes(entity, weapon);
        }
        Ok(())
    }

    fn on_string_table(
        &mut self,
        _ctx: &Context,
        table: &StringTable,
        _modified: &[i32],
    ) -> ObserverResult {
        if !table.name().eq_ignore_ascii_case("AnimAssetData") {
            return Ok(());
        }
        self.anim_asset_table_present = true;
        if self.anim_asset_initial_count.is_none() {
            let mut next_index = 0;
            for row in table.iter() {
                next_index = next_index.max(row.index().saturating_add(1));
                self.anim_asset_initial_keys
                    .insert(row.key().to_ascii_lowercase());
            }
            self.anim_asset_initial_count = Some(next_index);
        }
        Ok(())
    }

    fn on_stop(&mut self, ctx: &Context) -> ObserverResult {
        for table in ctx.string_tables().iter() {
            if table.name().eq_ignore_ascii_case("AnimAssetData") {
                self.anim_asset_table_present = true;
                break;
            }
        }
        Ok(())
    }
}

fn parse_plans(request: &Request) -> Result<HashMap<u64, PlayerPlan>> {
    let mut result: HashMap<u64, PlayerPlan> = HashMap::new();
    for pass in &request.passes {
        let steam = pass
            .steam_id64
            .parse::<u64>()
            .with_context(|| format!("invalid steam_id64 {:?}", pass.steam_id64))?;
        if steam == 0 {
            continue;
        }
        let plan = result.entry(steam).or_default();
        for keychain in &pass.keychains {
            if !keychains::supported_definition(keychain.definition_index) {
                bail!("unsupported keychain weapon definition {}", keychain.definition_index);
            }
            plan.keychains.insert(keychain.definition_index, keychain.clone());
        }
        for item in &pass.items {
            if item
                .mesh_group_mask
                .is_some_and(|mask| mask != 1 && mask != 2)
            {
                bail!(
                    "unsupported weapon mesh group mask {:?} for source definition {}",
                    item.mesh_group_mask,
                    item.definition_index
                );
            }
            let team = Team::parse(&item.team)?;
            let target = item
                .replacement_definition_index
                .unwrap_or(item.definition_index);
            let cosmetic = Cosmetic {
                definition: target,
                preserve_source_definition: item.preserve_source_definition,
                paint_kit: item.paint_kit,
                quality: item.quality,
                seed: item.pattern_seed.clamp(0.0, 1000.0),
                wear: item.wear.clamp(0.0000001, 1.0),
                mesh_group_mask: item.mesh_group_mask,
                stickers: item.stickers.map(|kit| kit.max(0)),
                sticker_transforms: item.sticker_transforms.map(StickerTransform::normalized),
                custom_name: {
                    let requested = item.custom_name.trim();
                    if requested.is_empty() {
                        "nSkinz CS2".to_owned()
                    } else {
                        requested.chars().take(127).collect()
                    }
                },
            };
            let team_plan = plan.by_team.entry(team).or_default();
            match item.definition_index {
                42 | 59 => team_plan.knife = Some(cosmetic),
                5028 | 5029 => team_plan.glove = Some(cosmetic),
                other if (1..500).contains(&other) => {
                    team_plan.weapons.insert(other, cosmetic);
                }
                other => bail!("unsupported demo cosmetic source definition {other}"),
            }
        }
    }
    if result.is_empty() {
        bail!("request contains no usable player cosmetic passes");
    }
    Ok(result)
}

fn knife_asset(definition: u16) -> Option<&'static KnifeAsset> {
    KNIFE_ASSETS
        .iter()
        .find(|asset| asset.definition == definition)
}

fn murmur_hash2_ascii_ci(input: &str, seed: u32) -> u32 {
    const MULTIPLIER: u32 = 0x5BD1_E995;
    const SHIFT: u32 = 24;
    let bytes: Vec<u8> = input
        .bytes()
        .map(|byte| byte.to_ascii_lowercase())
        .collect();
    let mut hash = seed ^ bytes.len() as u32;
    let mut chunks = bytes.chunks_exact(4);
    for chunk in &mut chunks {
        let mut value = u32::from_le_bytes(chunk.try_into().unwrap());
        value = value.wrapping_mul(MULTIPLIER);
        value ^= value >> SHIFT;
        value = value.wrapping_mul(MULTIPLIER);
        hash = hash.wrapping_mul(MULTIPLIER) ^ value;
    }
    let tail = chunks.remainder();
    match tail.len() {
        3 => {
            hash ^= (tail[2] as u32) << 16;
            hash ^= (tail[1] as u32) << 8;
            hash ^= tail[0] as u32;
            hash = hash.wrapping_mul(MULTIPLIER);
        }
        2 => {
            hash ^= (tail[1] as u32) << 8;
            hash ^= tail[0] as u32;
            hash = hash.wrapping_mul(MULTIPLIER);
        }
        1 => {
            hash ^= tail[0] as u32;
            hash = hash.wrapping_mul(MULTIPLIER);
        }
        _ => {}
    }
    hash ^= hash >> 13;
    hash = hash.wrapping_mul(MULTIPLIER);
    hash ^ (hash >> 15)
}

fn murmur_hash64b_ascii_ci(input: &str, seed: u64) -> u64 {
    const MULTIPLIER: u32 = 0x5BD1_E995;
    const SHIFT: u32 = 24;
    let bytes: Vec<u8> = input
        .bytes()
        .map(|byte| byte.to_ascii_lowercase())
        .collect();
    let mut high = seed as u32 ^ bytes.len() as u32;
    let mut low = (seed >> 32) as u32;
    let mut chunks = bytes.chunks_exact(8);
    for chunk in &mut chunks {
        let mut first = u32::from_le_bytes(chunk[0..4].try_into().unwrap());
        first = first.wrapping_mul(MULTIPLIER);
        first ^= first >> SHIFT;
        first = first.wrapping_mul(MULTIPLIER);
        high = high.wrapping_mul(MULTIPLIER) ^ first;

        let mut second = u32::from_le_bytes(chunk[4..8].try_into().unwrap());
        second = second.wrapping_mul(MULTIPLIER);
        second ^= second >> SHIFT;
        second = second.wrapping_mul(MULTIPLIER);
        low = low.wrapping_mul(MULTIPLIER) ^ second;
    }
    let tail = chunks.remainder();
    let (low_tail, high_tail) = if tail.len() >= 4 {
        (&tail[4..], Some(&tail[..4]))
    } else {
        (tail, None)
    };
    if let Some(high_tail) = high_tail {
        let mut value = u32::from_le_bytes(high_tail.try_into().unwrap());
        value = value.wrapping_mul(MULTIPLIER);
        value ^= value >> SHIFT;
        value = value.wrapping_mul(MULTIPLIER);
        high = high.wrapping_mul(MULTIPLIER) ^ value;
    }
    match low_tail.len() {
        3 => {
            low ^= (low_tail[2] as u32) << 16;
            low ^= (low_tail[1] as u32) << 8;
            low ^= low_tail[0] as u32;
            low = low.wrapping_mul(MULTIPLIER);
        }
        2 => {
            low ^= (low_tail[1] as u32) << 8;
            low ^= low_tail[0] as u32;
            low = low.wrapping_mul(MULTIPLIER);
        }
        1 => {
            low ^= low_tail[0] as u32;
            low = low.wrapping_mul(MULTIPLIER);
        }
        _ => {}
    }
    high ^= low >> 18;
    high = high.wrapping_mul(MULTIPLIER);
    low ^= high >> 22;
    low = low.wrapping_mul(MULTIPLIER);
    high ^= low >> 17;
    high = high.wrapping_mul(MULTIPLIER);
    low ^= high >> 19;
    low = low.wrapping_mul(MULTIPLIER);
    ((high as u64) << 32) | low as u64
}

fn installed_variant(definition: u16) -> Option<KnifeVariant> {
    const RESOURCE_ID_SEED: u64 = 0xEDAB_CDEF;
    const STRING_TOKEN_SEED: u32 = 0x3141_5926;
    let asset = knife_asset(definition)?;
    Some(KnifeVariant {
        model: murmur_hash64b_ascii_ci(asset.model, RESOURCE_ID_SEED),
        subclass: murmur_hash2_ascii_ci(&definition.to_string(), STRING_TOKEN_SEED),
    })
}

// AnimAssetData does not contain the compiled skeleton payload. Its value is
// the shared weapon animation classification block; the row key is the path
// that makes Source 2 load the actual compiled resource from the installed
// game content. This byte sequence is identical for the stock and replacement
// knife skeleton rows observed in CS2 demos.
const WEAPON_ANIM_ASSET_METADATA: &[u8] =
    b"\x01\x00\x00\x00\x03\x00\x00\x00weapon\x00weapon_offset\x00econ\x00\x00\x00\x00\x00";
const VPK_SIGNATURE: u32 = 0x55AA_1234;

struct VpkTarget {
    compiled: String,
    extension: Vec<u8>,
    directory: Vec<u8>,
    filename: Vec<u8>,
}

fn vpk_target(compiled: &str) -> Result<VpkTarget> {
    let compiled = compiled.replace('\\', "/").to_ascii_lowercase();
    let (directory, leaf) = compiled
        .rsplit_once('/')
        .map_or(("", compiled.as_str()), |(directory, leaf)| {
            (directory, leaf)
        });
    let (filename, extension) = leaf
        .rsplit_once('.')
        .ok_or_else(|| anyhow!("installed CS2 resource path has no extension: {compiled}"))?;
    Ok(VpkTarget {
        compiled: compiled.clone(),
        extension: extension.as_bytes().to_vec(),
        directory: directory.as_bytes().to_vec(),
        filename: filename.as_bytes().to_vec(),
    })
}

fn read_vpk_tree_cstring<'a>(tree: &'a [u8], offset: &mut usize) -> Result<&'a [u8]> {
    let remaining = tree
        .get(*offset..)
        .ok_or_else(|| anyhow!("installed CS2 VPK directory tree is truncated"))?;
    let length = remaining
        .iter()
        .position(|byte| *byte == 0)
        .ok_or_else(|| anyhow!("installed CS2 VPK directory string is unterminated"))?;
    if length >= 64 * 1024 {
        bail!("VPK directory string exceeded the safety limit");
    }
    let value = &remaining[..length];
    *offset = offset
        .checked_add(length + 1)
        .ok_or_else(|| anyhow!("installed CS2 VPK directory offset overflow"))?;
    Ok(value)
}

fn vpk_find_paths(directory_vpk: &Path, targets: &[String]) -> Result<HashSet<String>> {
    if targets.is_empty() {
        return Ok(HashSet::new());
    }
    let targets = targets
        .iter()
        .map(|target| vpk_target(target))
        .collect::<Result<Vec<_>>>()?;
    let file = File::open(directory_vpk)
        .with_context(|| format!("open installed CS2 VPK {}", directory_vpk.display()))?;
    let mut reader = BufReader::with_capacity(IO_BUFFER_CAPACITY, file);
    let mut header = [0u8; 12];
    reader.read_exact(&mut header)?;
    let signature = u32::from_le_bytes(header[0..4].try_into().unwrap());
    let version = u32::from_le_bytes(header[4..8].try_into().unwrap());
    let tree_size = u32::from_le_bytes(header[8..12].try_into().unwrap()) as usize;
    if signature != VPK_SIGNATURE {
        bail!("installed CS2 VPK has unexpected signature 0x{signature:08X}");
    }
    match version {
        1 => {}
        2 => {
            let mut extended_header = [0u8; 16];
            reader.read_exact(&mut extended_header)?;
        }
        other => bail!("installed CS2 VPK uses unsupported version {other}"),
    }
    let mut tree = vec![0u8; tree_size];
    reader
        .read_exact(&mut tree)
        .context("read installed CS2 VPK directory tree")?;
    let mut offset = 0usize;
    let mut found = HashSet::new();

    loop {
        let extension = read_vpk_tree_cstring(&tree, &mut offset)?;
        if extension.is_empty() {
            break;
        }
        loop {
            let directory = read_vpk_tree_cstring(&tree, &mut offset)?;
            if directory.is_empty() {
                break;
            }
            loop {
                let filename = read_vpk_tree_cstring(&tree, &mut offset)?;
                if filename.is_empty() {
                    break;
                }
                let entry = tree
                    .get(offset..offset + 18)
                    .ok_or_else(|| anyhow!("installed CS2 VPK entry header is truncated"))?;
                offset += 18;
                let preload_size = u16::from_le_bytes(entry[4..6].try_into().unwrap()) as u64;
                let payload_size = u32::from_le_bytes(entry[12..16].try_into().unwrap()) as u64;
                let terminator = u16::from_le_bytes(entry[16..18].try_into().unwrap());
                if terminator != 0xFFFF {
                    bail!("installed CS2 VPK entry has invalid terminator 0x{terminator:04X}");
                }
                let directory = if directory == b" " {
                    &b""[..]
                } else {
                    directory
                };
                let extension = if extension == b" " {
                    &b""[..]
                } else {
                    extension
                };
                if preload_size + payload_size > 0 {
                    for target in &targets {
                        if extension.eq_ignore_ascii_case(&target.extension)
                            && directory.eq_ignore_ascii_case(&target.directory)
                            && filename.eq_ignore_ascii_case(&target.filename)
                        {
                            found.insert(target.compiled.clone());
                        }
                    }
                    if found.len() == targets.len() {
                        return Ok(found);
                    }
                }
                offset = offset
                    .checked_add(preload_size as usize)
                    .filter(|offset| *offset <= tree.len())
                    .ok_or_else(|| anyhow!("installed CS2 VPK preload data is truncated"))?;
            }
        }
    }
    Ok(found)
}

fn vpk_contains_path(directory_vpk: &Path, target: &str) -> Result<bool> {
    let target = target.replace('\\', "/").to_ascii_lowercase();
    Ok(vpk_find_paths(directory_vpk, std::slice::from_ref(&target))?.contains(&target))
}

fn require_installed_resources(cs2_game_dir: &Path, keys: &BTreeSet<String>) -> Result<()> {
    let mut unresolved = Vec::new();
    for key in keys {
        let compiled = format!("{key}_c").replace('\\', "/").to_ascii_lowercase();
        let loose = cs2_game_dir.join(Path::new(&compiled));
        if !std::fs::metadata(&loose).is_ok_and(|metadata| metadata.is_file() && metadata.len() > 0)
        {
            unresolved.push(compiled);
        }
    }
    if unresolved.is_empty() {
        return Ok(());
    }
    let directory_vpk = cs2_game_dir.join("pak01_dir.vpk");
    if !directory_vpk.is_file() {
        bail!(
            "installed CS2 resource index is missing: {}",
            directory_vpk.display()
        );
    }
    let found = vpk_find_paths(&directory_vpk, &unresolved)?;
    let missing = unresolved
        .iter()
        .filter(|resource| !found.contains(resource.as_str()))
        .cloned()
        .collect::<Vec<_>>();
    if !missing.is_empty() {
        bail!(
            "resources are not present in the installed CS2 files: {}",
            missing.join(", ")
        );
    }
    Ok(())
}

fn require_installed_resource(cs2_game_dir: &Path, key: &str) -> Result<()> {
    let mut keys = BTreeSet::new();
    keys.insert(key.to_string());
    require_installed_resources(cs2_game_dir, &keys)
}

fn fnv_identity(xuid: u64, definition: u16, salt: u64) -> (u32, u32) {
    const OFFSET: u64 = 1_469_598_103_934_665_603;
    const PRIME: u64 = 1_099_511_628_211;
    let mut hash = OFFSET;
    for value in [salt, xuid, definition as u64] {
        hash ^= value;
        hash = hash.wrapping_mul(PRIME);
    }
    let mut low = (hash ^ (hash >> 32)) as u32;
    if low == 0 || low == u32::MAX {
        low ^= 0x6E53_6B7A;
    }
    if low == 0 || low == u32::MAX {
        low = 1;
    }
    (0xC521_0000 | definition as u32, low)
}

#[derive(Clone)]
struct KnifeRule {
    steam: u64,
    cosmetic: Cosmetic,
    variant: KnifeVariant,
}

#[derive(Clone)]
struct GloveRule {
    steam: u64,
    cosmetic: Cosmetic,
    paint_attributes: MaterializedPaintAttributes,
}

#[derive(Clone)]
struct WeaponRule {
    steam: u64,
    cosmetic: Cosmetic,
    paint_attributes: MaterializedPaintAttributes,
}

#[derive(Clone)]
struct StickerRule {
    kits: [i32; STICKER_SLOT_COUNT],
    expected_attribute_values:
        [[Option<u32>; STICKER_ATTRIBUTE_COMPONENT_COUNT]; STICKER_SLOT_COUNT],
    attribute_values: HashMap<usize, f32>,
    created_attributes: Vec<MaterializedStickerAttribute>,
    attribute_count: usize,
}

#[derive(Clone, Copy)]
struct MaterializedStickerAttribute {
    index: usize,
    definition: u16,
    value: f32,
}

#[derive(Clone)]
struct AnimAsset {
    index: i32,
    key: String,
    value: Vec<u8>,
}

#[derive(Clone)]
struct RewritePlan {
    knives: HashMap<u32, KnifeRule>,
    gloves: HashMap<u32, HashMap<Team, GloveRule>>,
    weapons: HashMap<u32, WeaponRule>,
    sticker_weapons: HashMap<u32, StickerRule>,
    keychain_weapons: HashMap<u32, KeychainRule>,
    requested_sticker_slots: usize,
    created_sticker_slots: usize,
    missing_sticker_slots: usize,
    painted_glove_passthroughs: HashSet<(u32, u8)>,
    painted_weapon_passthroughs: HashSet<u32>,
    resources: Vec<AnimAsset>,
    selected_pawns: HashSet<u32>,
    matched_players: usize,
    skipped_players: usize,
    matched_knife_requests: usize,
    matched_glove_requests: usize,
}

fn player_pawns(discovery: &Discovery, steam: u64) -> HashSet<u32> {
    let controllers: HashSet<u32> = discovery
        .controller_steam
        .iter()
        .filter_map(|(handle, value)| (*value == steam).then_some(*handle))
        .collect();
    let mut pawns = HashSet::new();
    for controller in &controllers {
        if let Some(linked) = discovery.controller_pawns.get(controller) {
            pawns.extend(linked.iter().copied());
        }
    }
    for (pawn_handle, pawn) in &discovery.pawns {
        if pawn
            .controllers
            .iter()
            .any(|controller| controllers.contains(controller))
        {
            pawns.insert(*pawn_handle);
        }
    }
    pawns
}

fn choose_team(pawn: &PawnDiscovery, plan: &PlayerPlan) -> Option<Team> {
    for team in [Team::T, Team::Ct] {
        if team
            .number()
            .is_some_and(|number| pawn.teams.contains(&number))
            && plan.by_team.contains_key(&team)
        {
            return Some(team);
        }
    }
    let mut specific = plan
        .by_team
        .keys()
        .copied()
        .filter(|team| *team != Team::Any);
    let only = specific.next();
    if only.is_some() && specific.next().is_none() {
        only
    } else {
        None
    }
}

fn build_rewrite_plan(
    plans: &HashMap<u64, PlayerPlan>,
    discovery: &Discovery,
    cs2_game_dir: Option<&Path>,
) -> Result<RewritePlan> {
    let mut knives = HashMap::new();
    let mut gloves: HashMap<u32, HashMap<Team, GloveRule>> = HashMap::new();
    let mut weapons = HashMap::new();
    let mut sticker_weapons: HashMap<u32, StickerRule> = HashMap::new();
    let mut keychain_weapons: HashMap<u32, KeychainRule> = HashMap::new();
    let mut requested_sticker_slots = HashSet::new();
    let mut created_sticker_slots = HashSet::new();
    // Kept in the response/manifest schema for compatibility with older UI
    // builds. Current rewrites never bypass an existing paint: discovered
    // attribute slots are updated in place instead.
    let painted_glove_passthroughs = HashSet::new();
    let painted_weapon_passthroughs = HashSet::new();
    let mut selected_pawns = HashSet::new();
    let mut target_definitions = BTreeSet::new();
    let mut matched_players = 0;
    let mut skipped_players = 0;
    let mut matched_knife_players = HashSet::new();
    let mut matched_glove_players = HashSet::new();

    for (steam, player_plan) in plans {
        let pawns = player_pawns(discovery, *steam);
        if pawns.is_empty() {
            skipped_players += 1;
            continue;
        }
        matched_players += 1;
        for pawn_handle in pawns {
            let Some(pawn) = discovery.pawns.get(&pawn_handle) else {
                continue;
            };
            let team = choose_team(pawn, player_plan);
            let team_plan = team.and_then(|team| player_plan.by_team.get(&team));
            let any_plan = player_plan.by_team.get(&Team::Any);
            if team_plan.is_none() && any_plan.is_none() && player_plan.keychains.is_empty() {
                continue;
            }
            selected_pawns.insert(pawn_handle);
            for handle in &pawn.weapons {
                let Some(weapon) = discovery.weapons.get(handle) else { continue; };
                let Some(request) = player_plan.keychains.get(&weapon.definition) else { continue; };
                if let Some(previous) = keychain_weapons.get(handle) {
                    if previous.request.kit_id != request.kit_id {
                        bail!("conflicting keychain requests for shared weapon handle {handle}: {} versus {}", previous.request.kit_id, request.kit_id);
                    }
                }
                keychain_weapons.insert(*handle, KeychainRule { steam: *steam, request: request.clone() });
            }
            for glove_team in [Team::T, Team::Ct, Team::Any] {
                if glove_team
                    .number()
                    .is_some_and(|number| !pawn.teams.contains(&number))
                {
                    continue;
                }
                let Some(cosmetic) = player_plan
                    .by_team
                    .get(&glove_team)
                    .and_then(|team_plan| team_plan.glove.as_ref())
                else {
                    continue;
                };
                matched_glove_players.insert(*steam);
                let team_number = glove_team.number().unwrap_or_default();
                let mut cosmetic = cosmetic.clone();
                if cosmetic.preserve_source_definition {
                    cosmetic.definition = pawn
                        .glove_definitions_by_team
                        .get(&team_number)
                        .copied()
                        .unwrap_or(cosmetic.definition);
                }
                let paint_attributes = pawn
                    .glove_paint_attributes_by_team
                    .get(&team_number)
                    .copied()
                    .map(materialize_paint_attributes)
                    .unwrap_or_else(|| materialize_paint_attributes(Default::default()));
                gloves.entry(pawn_handle).or_default().insert(
                    glove_team,
                    GloveRule {
                        steam: *steam,
                        cosmetic,
                        paint_attributes,
                    },
                );
            }
            if let Some(team_plan) = team_plan {
                if let Some(cosmetic) = &team_plan.knife {
                    matched_knife_players.insert(*steam);
                    let variant = installed_variant(cosmetic.definition).ok_or_else(|| {
                        anyhow!(
                            "knife definition {} has no installed CS2 model/subclass mapping",
                            cosmetic.definition
                        )
                    })?;
                    target_definitions.insert(cosmetic.definition);
                    for weapon in &pawn.weapons {
                        if discovery.knives.contains(weapon) {
                            knives.insert(
                                *weapon,
                                KnifeRule {
                                    steam: *steam,
                                    cosmetic: cosmetic.clone(),
                                    variant,
                                },
                            );
                        }
                    }
                }
            }
            for weapon_plan in [any_plan, team_plan].into_iter().flatten() {
                for (source_definition, cosmetic) in &weapon_plan.weapons {
                    for weapon_handle in &pawn.weapons {
                        if discovery.knives.contains(weapon_handle) {
                            continue;
                        }
                        let Some(discovered) = discovery.weapons.get(weapon_handle) else {
                            continue;
                        };
                        if discovered.definition != *source_definition {
                            continue;
                        }
                        let mut paint_attributes = (cosmetic.paint_kit > 0)
                            .then(|| materialize_paint_attributes(discovered.paint_attributes));
                        let mut attribute_count = paint_attributes
                            .as_ref()
                            .map(|attributes| attributes.count)
                            .unwrap_or(discovered.paint_attributes.count);
                        for (logical_slot, kit) in cosmetic.stickers.iter().copied().enumerate() {
                            if kit <= 0 {
                                continue;
                            }
                            requested_sticker_slots.insert((*weapon_handle, logical_slot));
                            let rule = sticker_weapons.entry(*weapon_handle).or_insert_with(|| {
                                StickerRule {
                                    kits: [0; STICKER_SLOT_COUNT],
                                    expected_attribute_values: [[None;
                                        STICKER_ATTRIBUTE_COMPONENT_COUNT];
                                        STICKER_SLOT_COUNT],
                                    attribute_values: HashMap::new(),
                                    created_attributes: Vec::new(),
                                    attribute_count: discovered.paint_attributes.count,
                                }
                            });
                            attribute_count = attribute_count.max(rule.attribute_count);
                            rule.kits[logical_slot] = kit;
                            let transform = cosmetic.sticker_transforms[logical_slot];
                            let source_slot_exists = discovered.sticker_slots[logical_slot];
                            let component_count = if transform.custom_position {
                                STICKER_ATTRIBUTE_COMPONENT_COUNT
                            } else if source_slot_exists {
                                1
                            } else {
                                4
                            };
                            let value_bits = [
                                kit as u32,
                                transform.wear.to_bits(),
                                transform.scale.to_bits(),
                                transform.rotation.to_bits(),
                                transform.offset_x.to_bits(),
                                transform.offset_y.to_bits(),
                                transform.schema,
                            ];
                            for component in 0..component_count {
                                let definition = sticker_definition(logical_slot, component);
                                let bits = value_bits[component];
                                let value = f32::from_bits(bits);
                                rule.expected_attribute_values[logical_slot][component] =
                                    Some(bits);
                                let existing =
                                    &discovered.sticker_attribute_indices[logical_slot][component];
                                if !existing.is_empty() {
                                    for attribute_index in existing {
                                        rule.attribute_values.insert(*attribute_index, value);
                                    }
                                    continue;
                                }
                                if let Some(attribute) = rule
                                    .created_attributes
                                    .iter_mut()
                                    .find(|attribute| attribute.definition == definition)
                                {
                                    attribute.value = value;
                                    continue;
                                }
                                if attribute_count >= ECON_ATTRIBUTE_SLOT_COUNT {
                                    bail!(
                                        "weapon handle {weapon_handle} needs sticker slot {} component {} but its dynamic attribute vector already uses all {ECON_ATTRIBUTE_SLOT_COUNT} entries",
                                        logical_slot + 1,
                                        component + 1
                                    );
                                }
                                rule.created_attributes.push(MaterializedStickerAttribute {
                                    index: attribute_count,
                                    definition,
                                    value,
                                });
                                attribute_count += 1;
                            }
                            if !source_slot_exists {
                                created_sticker_slots.insert((*weapon_handle, logical_slot));
                            }
                            rule.attribute_count = rule.attribute_count.max(attribute_count);
                        }
                        if let Some(attributes) = paint_attributes.as_mut() {
                            attributes.count = attributes.count.max(attribute_count);
                            weapons.insert(
                                *weapon_handle,
                                WeaponRule {
                                    steam: *steam,
                                    cosmetic: cosmetic.clone(),
                                    paint_attributes: *attributes,
                                },
                            );
                        }
                    }
                }
            }
        }
    }
    if knives.is_empty()
        && gloves.is_empty()
        && weapons.is_empty()
        && painted_glove_passthroughs.is_empty()
        && painted_weapon_passthroughs.is_empty()
        && requested_sticker_slots.is_empty()
        && keychain_weapons.is_empty()
    {
        if matched_players == 0 && skipped_players > 0 {
            bail!("none of the {skipped_players} configured Steam IDs belong to this demo");
        }
        bail!("no owned knife, glove or weapon targets were resolved from the demo");
    }

    if !target_definitions.is_empty() && !discovery.anim_asset_table_present {
        bail!("source demo does not contain the AnimAssetData string table required for knife replacement");
    }
    if !target_definitions.is_empty() && discovery.anim_asset_initial_count.is_none() {
        bail!("source demo did not expose the initial AnimAssetData table required for safe row insertion");
    }
    let mut resources = Vec::new();
    let mut required_resources = BTreeSet::new();
    for definition in &target_definitions {
        let asset = knife_asset(*definition).ok_or_else(|| {
            anyhow!("knife definition {definition} has no installed CS2 asset mapping")
        })?;
        required_resources.insert(asset.model.to_string());
        required_resources.insert(asset.skeleton.to_string());
    }
    let insert_at = discovery.anim_asset_initial_count.unwrap_or_default();
    let mut installed_initial_rows = discovery.anim_asset_initial_keys.clone();
    for definition in &target_definitions {
        let target_asset = knife_asset(*definition).ok_or_else(|| {
            anyhow!("knife definition {definition} has no installed CS2 asset mapping")
        })?;
        let normalized_target = target_asset.skeleton.to_ascii_lowercase();
        if installed_initial_rows.contains(&normalized_target) {
            continue;
        }
        resources.push(AnimAsset {
            index: insert_at + resources.len() as i32,
            key: target_asset.skeleton.to_string(),
            value: WEAPON_ANIM_ASSET_METADATA.to_vec(),
        });
        installed_initial_rows.insert(normalized_target);
    }
    if !required_resources.is_empty() {
        let cs2_game_dir = cs2_game_dir.ok_or_else(|| {
            anyhow!("request is missing cs2_game_dir required for installed knife resources")
        })?;
        require_installed_resources(cs2_game_dir, &required_resources)?;
    }
    Ok(RewritePlan {
        knives,
        gloves,
        weapons,
        sticker_weapons,
        keychain_weapons,
        requested_sticker_slots: requested_sticker_slots.len(),
        created_sticker_slots: created_sticker_slots.len(),
        missing_sticker_slots: 0,
        painted_glove_passthroughs,
        painted_weapon_passthroughs,
        resources,
        selected_pawns,
        matched_players,
        skipped_players,
        matched_knife_requests: matched_knife_players.len(),
        matched_glove_requests: matched_glove_players.len(),
    })
}

fn push_attribute(
    fields: &mut Vec<(String, FieldValue)>,
    prefix: &str,
    slot: usize,
    definition: u16,
    value: f32,
) {
    let base = format!("{prefix}.m_NetworkedDynamicAttributes.m_Attributes.{slot:04}");
    fields.push((
        format!("{base}.m_iAttributeDefinitionIndex"),
        FieldValue::Unsigned16(definition),
    ));
    fields.push((format!("{base}.m_iRawValue32"), FieldValue::Float(value)));
    fields.push((format!("{base}.m_flInitialValue"), FieldValue::Float(value)));
    fields.push((
        format!("{base}.m_nRefundableCurrency"),
        FieldValue::Signed32(0),
    ));
    fields.push((format!("{base}.m_bSetBonus"), FieldValue::Boolean(false)));
}

fn knife_fields(rule: &KnifeRule) -> Vec<(String, FieldValue)> {
    let c = &rule.cosmetic;
    let account = rule.steam as u32;
    let (high, low) = fnv_identity(rule.steam, c.definition, 0x4B4E_4946_455F_4944);
    let prefix = "m_AttributeManager.m_Item";
    let mut fields = vec![
        (
            "CBodyComponent.m_skeletonInstance.m_modelState.m_hModel".into(),
            FieldValue::Unsigned64(rule.variant.model),
        ),
        (
            "m_nSubclassID".into(),
            FieldValue::Unsigned32(rule.variant.subclass),
        ),
        (
            format!("{prefix}.m_iItemDefinitionIndex"),
            FieldValue::Unsigned16(c.definition),
        ),
        (
            format!("{prefix}.m_iEntityQuality"),
            FieldValue::Signed32(3),
        ),
        (
            format!("{prefix}.m_iItemIDHigh"),
            FieldValue::Unsigned32(high),
        ),
        (
            format!("{prefix}.m_iItemIDLow"),
            FieldValue::Unsigned32(low),
        ),
        (
            format!("{prefix}.m_iAccountID"),
            FieldValue::Unsigned32(account),
        ),
        (
            format!("{prefix}.m_NetworkedDynamicAttributes.m_Attributes"),
            FieldValue::Unsigned32(3),
        ),
        (
            format!("{prefix}.m_szCustomName"),
            FieldValue::String(c.custom_name.clone()),
        ),
        (
            "m_nFallbackPaintKit".into(),
            FieldValue::Signed32(c.paint_kit),
        ),
        (
            "m_nFallbackSeed".into(),
            FieldValue::Signed32(c.seed as i32),
        ),
        ("m_flFallbackWear".into(), FieldValue::Float(c.wear)),
    ];
    push_attribute(&mut fields, prefix, 0, 6, c.paint_kit as f32);
    push_attribute(&mut fields, prefix, 1, 7, c.seed);
    push_attribute(&mut fields, prefix, 2, 8, c.wear);
    fields
}

fn weapon_fields(rule: &WeaponRule) -> Vec<(String, FieldValue)> {
    let c = &rule.cosmetic;
    let account = rule.steam as u32;
    let (high, low) = fnv_identity(rule.steam, c.definition, 0x5745_4150_4F4E_4944);
    let prefix = "m_AttributeManager.m_Item";
    let mut fields = vec![
        (
            format!("{prefix}.m_iItemDefinitionIndex"),
            FieldValue::Unsigned16(c.definition),
        ),
        (
            format!("{prefix}.m_iEntityQuality"),
            FieldValue::Signed32(c.quality),
        ),
        (
            format!("{prefix}.m_iItemIDHigh"),
            FieldValue::Unsigned32(high),
        ),
        (
            format!("{prefix}.m_iItemIDLow"),
            FieldValue::Unsigned32(low),
        ),
        (
            format!("{prefix}.m_iAccountID"),
            FieldValue::Unsigned32(account),
        ),
        (
            format!("{prefix}.m_NetworkedDynamicAttributes.m_Attributes"),
            FieldValue::Unsigned32(rule.paint_attributes.count as u32),
        ),
        (
            format!("{prefix}.m_szCustomName"),
            FieldValue::String(c.custom_name.clone()),
        ),
        (
            "m_nFallbackPaintKit".into(),
            FieldValue::Signed32(c.paint_kit),
        ),
        (
            "m_nFallbackSeed".into(),
            FieldValue::Signed32(c.seed as i32),
        ),
        ("m_flFallbackWear".into(), FieldValue::Float(c.wear)),
    ];
    if let Some(mask) = c.mesh_group_mask {
        fields.push((
            "CBodyComponent.m_skeletonInstance.m_modelState.m_MeshGroupMask".into(),
            FieldValue::Unsigned64(mask),
        ));
    }
    push_attribute(
        &mut fields,
        prefix,
        rule.paint_attributes.indices[0],
        6,
        c.paint_kit as f32,
    );
    push_attribute(
        &mut fields,
        prefix,
        rule.paint_attributes.indices[1],
        7,
        c.seed,
    );
    push_attribute(
        &mut fields,
        prefix,
        rule.paint_attributes.indices[2],
        8,
        c.wear,
    );
    fields
}

fn glove_fields(rule: &GloveRule) -> Vec<(String, FieldValue)> {
    let c = &rule.cosmetic;
    let account = rule.steam as u32;
    let quality = if c.paint_kit > 0 && c.quality == 0 {
        3
    } else {
        c.quality
    };
    let (high, low) = fnv_identity(rule.steam, c.definition, 0x474C_4F56_455F_4944);
    let prefix = "m_EconGloves";
    let mut fields = vec![
        (
            format!("{prefix}.m_iItemDefinitionIndex"),
            FieldValue::Unsigned16(c.definition),
        ),
        (
            format!("{prefix}.m_iEntityQuality"),
            FieldValue::Signed32(quality),
        ),
        (
            format!("{prefix}.m_iEntityLevel"),
            FieldValue::Unsigned32(1),
        ),
        (
            format!("{prefix}.m_iItemIDHigh"),
            FieldValue::Unsigned32(high),
        ),
        (
            format!("{prefix}.m_iItemIDLow"),
            FieldValue::Unsigned32(low),
        ),
        (
            format!("{prefix}.m_iAccountID"),
            FieldValue::Unsigned32(account),
        ),
        (
            format!("{prefix}.m_iInventoryPosition"),
            FieldValue::Unsigned32(48),
        ),
        (
            format!("{prefix}.m_bInitialized"),
            FieldValue::Boolean(true),
        ),
        (
            format!("{prefix}.m_NetworkedDynamicAttributes.m_Attributes"),
            FieldValue::Unsigned32(rule.paint_attributes.count as u32),
        ),
        (
            format!("{prefix}.m_szCustomName"),
            FieldValue::String(c.custom_name.clone()),
        ),
        (
            "CBodyComponent.m_skeletonInstance.m_modelState.m_nBodyGroupChoices.0000".into(),
            FieldValue::Signed32(1),
        ),
        (
            "CBodyComponent.m_skeletonInstance.m_modelState.m_nBodyGroupChoices.0001".into(),
            FieldValue::Signed32(1),
        ),
        ("m_nEconGlovesChanged".into(), FieldValue::Unsigned8(1)),
    ];
    push_attribute(
        &mut fields,
        prefix,
        rule.paint_attributes.indices[0],
        6,
        c.paint_kit as f32,
    );
    push_attribute(
        &mut fields,
        prefix,
        rule.paint_attributes.indices[1],
        7,
        c.seed,
    );
    push_attribute(
        &mut fields,
        prefix,
        rule.paint_attributes.indices[2],
        8,
        c.wear,
    );
    fields
}

fn entity_team(entity: &Entity) -> Option<Team> {
    match field_integer(entity, "m_iTeamNum")? as u8 {
        2 => Some(Team::T),
        3 => Some(Team::Ct),
        _ => None,
    }
}

fn glove_rule_for_entity_with_team<'a>(
    plan: &'a RewritePlan,
    entity: &Entity,
) -> Option<(Team, &'a GloveRule)> {
    let rules = plan.gloves.get(&entity.handle())?;
    if let Some(team) = entity_team(entity) {
        if let Some(rule) = rules.get(&team) {
            return Some((team, rule));
        }
    }
    rules.get(&Team::Any).map(|rule| (Team::Any, rule))
}

fn glove_rule_for_entity<'a>(plan: &'a RewritePlan, entity: &Entity) -> Option<&'a GloveRule> {
    glove_rule_for_entity_with_team(plan, entity).map(|(_, rule)| rule)
}

fn should_materialize_glove_rule(
    materialized_team_by_pawn: &mut HashMap<u32, Team>,
    pawn_handle: u32,
    team: Team,
    event: EntityEvents,
) -> bool {
    if event != EntityEvents::Created && materialized_team_by_pawn.get(&pawn_handle) == Some(&team)
    {
        return false;
    }
    materialized_team_by_pawn.insert(pawn_handle, team);
    true
}

struct CosmeticRewriter {
    plan: RewritePlan,
    // A Pawn's team is not guaranteed to be present in its Created delta.
    // Remember the last rule that was fully materialized so the first later
    // update carrying m_iTeamNum can add the missing m_EconGloves subtree.
    // Every Created record is independent, including seek/full-packet
    // snapshots that reuse the same handle and team. Never deduplicate those.
    materialized_glove_team_by_pawn: HashMap<u32, Team>,
}

fn replacement_from_fields(
    fields: Vec<(String, FieldValue)>,
    field_name: &str,
) -> Option<FieldValue> {
    fields
        .into_iter()
        .find_map(|(name, value)| (name == field_name).then_some(value))
}

fn sticker_field_replacement(rule: &StickerRule, field_name: &str) -> Option<FieldValue> {
    if field_name == "m_AttributeManager.m_Item.m_NetworkedDynamicAttributes.m_Attributes"
        && !rule.created_attributes.is_empty()
    {
        return Some(FieldValue::Unsigned32(rule.attribute_count as u32));
    }
    for (attribute_index, field) in WEAPON_PAINT_FIELDS.iter().enumerate() {
        if field_name != field.raw && field_name != field.initial {
            continue;
        }
        let value = *rule.attribute_values.get(&attribute_index)?;
        return Some(FieldValue::Float(value));
    }
    None
}

fn sticker_fields(rule: &StickerRule, include_count: bool) -> Vec<(String, FieldValue)> {
    let prefix = "m_AttributeManager.m_Item";
    let mut fields =
        Vec::with_capacity(rule.attribute_values.len() * 2 + rule.created_attributes.len() * 5 + 1);
    if include_count && !rule.created_attributes.is_empty() {
        fields.push((
            format!("{prefix}.m_NetworkedDynamicAttributes.m_Attributes"),
            FieldValue::Unsigned32(rule.attribute_count as u32),
        ));
    }
    for (attribute_index, replacement) in &rule.attribute_values {
        let base =
            format!("{prefix}.m_NetworkedDynamicAttributes.m_Attributes.{attribute_index:04}");
        let value = FieldValue::Float(*replacement);
        fields.push((format!("{base}.m_iRawValue32"), value.clone()));
        fields.push((format!("{base}.m_flInitialValue"), value));
    }
    for attribute in &rule.created_attributes {
        push_attribute(
            &mut fields,
            prefix,
            attribute.index,
            attribute.definition,
            attribute.value,
        );
    }
    fields
}

impl DemoRewriter for CosmeticRewriter {
    fn interests(&self) -> RewriteInterests {
        RewriteInterests::ENTITY_FIELDS
            | RewriteInterests::STRING_TABLE_ENTRIES
            | RewriteInterests::DEMO_STRING_TABLES
            | RewriteInterests::SVC_CREATE_STRING_TABLE
    }

    fn should_rewrite_entity(
        &mut self,
        _ctx: &Context,
        _event: EntityEvents,
        entity: &Entity,
    ) -> bool {
        self.plan.knives.contains_key(&entity.handle())
            || self.plan.gloves.contains_key(&entity.handle())
            || self.plan.weapons.contains_key(&entity.handle())
            || self.plan.sticker_weapons.contains_key(&entity.handle())
            || self.plan.keychain_weapons.contains_key(&entity.handle())
    }

    fn replace_entity_field(
        &mut self,
        _ctx: &Context,
        _event: EntityEvents,
        entity: &Entity,
        field_name: &str,
        _value: &FieldValue,
    ) -> Option<FieldValue> {
        if let Some(rule) = self.plan.knives.get(&entity.handle()) {
            return replacement_from_fields(knife_fields(rule), field_name);
        }
        if let Some(rule) = self.plan.weapons.get(&entity.handle()) {
            if let Some(replacement) = replacement_from_fields(weapon_fields(rule), field_name) {
                return Some(replacement);
            }
        }
        if let Some(rule) = glove_rule_for_entity(&self.plan, entity) {
            return replacement_from_fields(glove_fields(rule), field_name);
        }
        if let Some(rule) = self.plan.sticker_weapons.get(&entity.handle()) {
            return sticker_field_replacement(rule, field_name);
        }
        None
    }

    fn additional_entity_fields(
        &mut self,
        _ctx: &Context,
        event: EntityEvents,
        entity: &Entity,
    ) -> Result<Vec<(String, FieldValue)>, source2_demo::error::ParserError> {
        let mut fields = Vec::new();
        if event == EntityEvents::Created {
            self.materialized_glove_team_by_pawn
                .remove(&entity.handle());
        }
        // Handle-independent item rules can be materialized as soon as the
        // entity is created. Glove rules are different: team may arrive in a
        // later Updated delta, so they are handled below for both events.
        if event == EntityEvents::Created {
            if let Some(rule) = self.plan.knives.get(&entity.handle()) {
                fields.extend(knife_fields(rule));
            }
            if let Some(rule) = self.plan.weapons.get(&entity.handle()) {
                fields.extend(weapon_fields(rule));
            }
            if let Some(rule) = self.plan.sticker_weapons.get(&entity.handle()) {
                fields.extend(sticker_fields(
                    rule,
                    !self.plan.weapons.contains_key(&entity.handle()),
                ));
            }
        }

        if let Some((team, rule)) = glove_rule_for_entity_with_team(&self.plan, entity) {
            let handle = entity.handle();
            if should_materialize_glove_rule(
                &mut self.materialized_glove_team_by_pawn,
                handle,
                team,
                event,
            ) {
                fields.extend(glove_fields(rule));
            }
        }
        if let Some(rule) = self.plan.keychain_weapons.get(&entity.handle()) {
            let keychain_fields = keychains::fields(
                entity, &fields, rule,
            ).map_err(|error| source2_demo::error::ParserError::IoError(format!(
                "keychain rewrite failed at tick {}, handle {}: {error:#}", _ctx.tick(), entity.handle()
            )))?;
            // Keychain allocation sees the complete paint/sticker overlay and
            // owns the final vector length, so it must be appended last.
            fields.extend(keychain_fields);
        }
        Ok(fields)
    }

    fn rewrite_string_table_entry(
        &mut self,
        _ctx: &Context,
        _tick: u32,
        table_name: &str,
        entry: &mut StringTableEntryUpdate,
    ) -> Result<(), source2_demo::error::ParserError> {
        let Some(first) = self.plan.resources.first() else {
            return Ok(());
        };
        if table_name.eq_ignore_ascii_case("AnimAssetData") && entry.index() >= first.index {
            entry.set_index(entry.index() + self.plan.resources.len() as i32);
        }
        Ok(())
    }

    fn rewrite_svc_create_string_table(
        &mut self,
        _ctx: &Context,
        _tick: u32,
        message: &mut CSvcMsgCreateStringTable,
    ) -> Result<MessageRewrite, source2_demo::error::ParserError> {
        if !message.name().eq_ignore_ascii_case("AnimAssetData") || self.plan.resources.is_empty() {
            return Ok(MessageRewrite::Keep);
        }
        for resource in &self.plan.resources {
            insert_create_string_table_entry(
                message,
                resource.index,
                resource.key.clone(),
                resource.value.clone(),
            )?;
        }
        Ok(MessageRewrite::Rewrite)
    }

    fn rewrite_demo_string_tables(
        &mut self,
        _ctx: &Context,
        _tick: u32,
        message: &mut CDemoStringTables,
    ) -> Result<MessageRewrite, source2_demo::error::ParserError> {
        let mut changed = false;
        for table in &mut message.tables {
            if !table.table_name().eq_ignore_ascii_case("AnimAssetData") {
                continue;
            }
            for resource in &self.plan.resources {
                let index = resource.index.max(0) as usize;
                if index > table.items.len() {
                    return Err(source2_demo::error::ParserError::IoError(format!(
                        "AnimAssetData insertion index {} exceeds snapshot size {}",
                        resource.index,
                        table.items.len()
                    )));
                }
                table.items.insert(
                    index,
                    ItemsT {
                        str: Some(resource.key.clone()),
                        data: Some(resource.value.clone()),
                    },
                );
                changed = true;
            }
        }
        Ok(if changed {
            MessageRewrite::Rewrite
        } else {
            MessageRewrite::Keep
        })
    }
}

fn discover(input: &Path) -> Result<Discovery> {
    let file =
        File::open(input).with_context(|| format!("open source demo {}", input.display()))?;
    let mut parser = Parser::from_reader(BufReader::with_capacity(IO_BUFFER_CAPACITY, file))?;
    let state = parser.register_observer::<Discovery>();
    parser.run_to_end()?;
    let mut state = state.borrow_mut();
    Ok(std::mem::take(&mut *state))
}

fn discover_from_slice(input: &[u8]) -> Result<Discovery> {
    let mut parser = Parser::from_slice(input)?;
    let state = parser.register_observer::<Discovery>();
    parser.run_to_end()?;
    let mut state = state.borrow_mut();
    Ok(std::mem::take(&mut *state))
}

fn elapsed_ms(start: Instant) -> u64 {
    start.elapsed().as_millis().min(u64::MAX as u128) as u64
}

// Unlike the sequential Parser observer, DemoWriter decodes every full-packet
// entity snapshot, including snapshots normally skipped during linear playback.
// Use its read-only field callbacks to verify actual per-record state as well
// as every independent seek checkpoint. No bytes are retained or written to disk.
#[derive(Default)]
struct VerificationSink {
    position: u64,
    length: u64,
}

impl Write for VerificationSink {
    fn write(&mut self, bytes: &[u8]) -> std::io::Result<usize> {
        self.position += bytes.len() as u64;
        self.length = self.length.max(self.position);
        Ok(bytes.len())
    }

    fn flush(&mut self) -> std::io::Result<()> {
        Ok(())
    }
}

impl Seek for VerificationSink {
    fn seek(&mut self, from: SeekFrom) -> std::io::Result<u64> {
        let next = match from {
            SeekFrom::Start(value) => value as i128,
            SeekFrom::End(value) => self.length as i128 + value as i128,
            SeekFrom::Current(value) => self.position as i128 + value as i128,
        };
        self.position = u64::try_from(next).map_err(|_| {
            std::io::Error::new(
                std::io::ErrorKind::InvalidInput,
                "invalid verification seek",
            )
        })?;
        Ok(self.position)
    }
}

struct GloveSnapshotVerifier {
    plan: RewritePlan,
    expected_fields: HashMap<(u32, Team), Vec<(String, FieldValue)>>,
    verified_rules: HashSet<(u32, Team)>,
    created_records: usize,
    checked_records: usize,
    verified_keychains: HashSet<u32>,
    keychain_records: usize,
    keychain_created: usize,
}

impl GloveSnapshotVerifier {
    fn new(plan: &RewritePlan) -> Self {
        let expected_fields = plan
            .gloves
            .iter()
            .flat_map(|(handle, rules)| {
                rules
                    .iter()
                    .map(|(team, rule)| ((*handle, *team), glove_fields(rule)))
            })
            .collect();
        Self {
            plan: plan.clone(),
            expected_fields,
            verified_rules: HashSet::new(),
            created_records: 0,
            checked_records: 0,
            verified_keychains: HashSet::new(),
            keychain_records: 0,
            keychain_created: 0,
        }
    }
}

impl DemoRewriter for GloveSnapshotVerifier {
    fn interests(&self) -> RewriteInterests {
        RewriteInterests::ENTITY_FIELDS
    }

    fn should_rewrite_entity(
        &mut self,
        _ctx: &Context,
        _event: EntityEvents,
        entity: &Entity,
    ) -> bool {
        self.plan.gloves.contains_key(&entity.handle())
            || self.plan.keychain_weapons.contains_key(&entity.handle())
    }

    fn additional_entity_fields(
        &mut self,
        ctx: &Context,
        event: EntityEvents,
        entity: &Entity,
    ) -> Result<Vec<(String, FieldValue)>, source2_demo::error::ParserError> {
        if let Some(rule) = self.plan.keychain_weapons.get(&entity.handle()) {
            keychains::verify(entity, rule).map_err(|error| source2_demo::error::ParserError::IoError(format!(
                "keychain snapshot verification failed at tick {}, event {event:?}, handle {}: {error:#}", ctx.tick(), entity.handle()
            )))?;
            self.verified_keychains.insert(entity.handle());
            self.keychain_records += 1;
            if event == EntityEvents::Created { self.keychain_created += 1; }
        }
        if let Some((team, _rule)) = glove_rule_for_entity_with_team(&self.plan, entity) {
            let handle = entity.handle();
            for (name, expected) in &self.expected_fields[&(handle, team)] {
                let actual = entity.try_get_property(name);
                if actual != Some(expected) {
                    return Err(source2_demo::error::ParserError::IoError(format!(
                        "glove snapshot verification failed at tick {}, event {event:?}, pawn handle {handle}, team {team:?}, field {name}: expected {expected:?}, observed {actual:?}",
                        ctx.tick()
                    )));
                }
            }
            self.verified_rules.insert((handle, team));
            self.checked_records += 1;
            if event == EntityEvents::Created {
                self.created_records += 1;
            }
        }
        Ok(Vec::new())
    }
}

fn verify_glove_snapshots(plan: &RewritePlan, output: &Path) -> Result<(usize, usize, usize, usize)> {
    if plan.gloves.is_empty() && plan.keychain_weapons.is_empty() {
        return Ok((0, 0, 0, 0));
    }
    let input = BufReader::with_capacity(IO_BUFFER_CAPACITY, File::open(output)?);
    let mut writer = DemoWriter::from_reader(input, VerificationSink::default())?;
    let state = writer.add_rewriter(GloveSnapshotVerifier::new(plan));
    writer.run()?;
    let state = state.borrow();
    for (handle, rules) in &plan.gloves {
        for team in rules.keys() {
            if !state.verified_rules.contains(&(*handle, *team)) {
                bail!("no verified glove snapshot for pawn handle {handle}, team {team:?}");
            }
        }
    }
    for handle in plan.keychain_weapons.keys() {
        if !state.verified_keychains.contains(handle) {
            bail!("no verified keychain snapshot for weapon handle {handle}");
        }
    }
    Ok((state.checked_records, state.created_records, state.keychain_records, state.keychain_created))
}

fn verify_sticker_rewrites(plan: &RewritePlan, discovery: &Discovery) -> Result<usize> {
    let mut verified = 0;
    for (handle, rule) in &plan.sticker_weapons {
        let weapon = discovery
            .weapons
            .get(handle)
            .ok_or_else(|| anyhow!("rewritten demo lost sticker weapon handle {handle}"))?;
        for (logical_slot, desired) in rule.kits.iter().copied().enumerate() {
            if desired <= 0 {
                continue;
            }
            for (component, expected) in rule.expected_attribute_values[logical_slot]
                .iter()
                .copied()
                .enumerate()
            {
                let Some(expected) = expected else {
                    continue;
                };
                let observed = &weapon.sticker_attribute_values[logical_slot][component];
                if observed.len() != 1 || !observed.contains(&expected) {
                    bail!(
                        "sticker verification failed for weapon handle {handle}, slot {}, definition {}: desired bits {expected}, observed {:?}",
                        logical_slot + 1,
                        sticker_definition(logical_slot, component),
                        observed
                    );
                }
            }
            verified += 1;
        }
    }
    Ok(verified)
}

fn verify_glove_rewrites(plan: &RewritePlan, discovery: &Discovery) -> Result<usize> {
    let mut verified = 0;
    for (handle, rules) in &plan.gloves {
        let pawn = discovery
            .pawns
            .get(handle)
            .ok_or_else(|| anyhow!("rewritten demo lost glove pawn handle {handle}"))?;
        for (team, rule) in rules {
            let c = &rule.cosmetic;
            let quality = if c.paint_kit > 0 && c.quality == 0 {
                3
            } else {
                c.quality
            };
            let (expected_high, expected_low) =
                fnv_identity(rule.steam, c.definition, 0x474C_4F56_455F_4944);
            let expected_account = rule.steam as u32;
            let team_definition_matches = team.number().map_or(true, |number| {
                pawn.glove_definitions_by_team.get(&number) == Some(&c.definition)
            });
            if !team_definition_matches
                || !pawn.glove_definitions.contains(&c.definition)
                || !pawn.glove_item_id_highs.contains(&expected_high)
                || !pawn.glove_item_id_lows.contains(&expected_low)
                || !pawn.glove_account_ids.contains(&expected_account)
                || !pawn.glove_qualities.contains(&quality)
            {
                bail!(
                    "glove identity verification failed for pawn handle {handle}, team {team:?}: desired definition/quality/id/account {}/{}/{}:{}/{}, observed team definitions {:?}, definitions {:?}, qualities {:?}, highs {:?}, lows {:?}, accounts {:?}",
                    c.definition,
                    quality,
                    expected_high,
                    expected_low,
                    expected_account,
                    pawn.glove_definitions_by_team,
                    pawn.glove_definitions,
                    pawn.glove_qualities,
                    pawn.glove_item_id_highs,
                    pawn.glove_item_id_lows,
                    pawn.glove_account_ids
                );
            }
            for (slot, expected) in [
                (0usize, (c.paint_kit as f32).to_bits()),
                (1usize, c.seed.to_bits()),
                (2usize, c.wear.to_bits()),
            ] {
                if !pawn.glove_paint_values[slot].contains(&expected) {
                    bail!(
                        "glove finish verification failed for pawn handle {handle}, team {team:?}, attribute definition {}: desired bits {expected}, observed {:?}",
                        6 + slot,
                        pawn.glove_paint_values[slot]
                    );
                }
            }
            if !pawn
                .glove_attribute_counts
                .iter()
                .any(|count| *count >= rule.paint_attributes.count)
                || !pawn.glove_custom_names.contains(&c.custom_name)
                || pawn
                    .glove_body_group_choices
                    .iter()
                    .any(|choices| !choices.contains(&1))
                || !pawn
                    .glove_change_counters
                    .iter()
                    .any(|counter| *counter != 0)
            {
                bail!(
                    "glove presentation verification failed for pawn handle {handle}, team {team:?}: counts {:?}, names {:?}, body groups {:?}, change counters {:?}",
                    pawn.glove_attribute_counts,
                    pawn.glove_custom_names,
                    pawn.glove_body_group_choices,
                    pawn.glove_change_counters
                );
            }
        }
        verified += 1;
    }
    Ok(verified)
}

fn rewrite(request: &Request) -> Result<Response> {
    let total_started = Instant::now();
    if !matches!(request.schema_version, 1 | 2) {
        bail!("unsupported request schema {}", request.schema_version);
    }
    if request.input_dem == request.output_dem {
        bail!("input and output demo paths must differ");
    }
    let plans = parse_plans(request)?;
    let input_size = std::fs::metadata(&request.input_dem)
        .with_context(|| format!("inspect source demo {}", request.input_dem.display()))?
        .len();
    let mut input_read_ms = 0;
    let mut input_mode = "buffered_stream".to_string();
    let in_memory = if input_size <= IN_MEMORY_DEMO_LIMIT {
        let read_started = Instant::now();
        let bytes = std::fs::read(&request.input_dem)
            .with_context(|| format!("read source demo {}", request.input_dem.display()))?;
        input_read_ms = elapsed_ms(read_started);
        input_mode = "single_read_memory".to_string();
        Some(bytes)
    } else {
        None
    };

    let discovery_started = Instant::now();
    let discovery = match in_memory.as_deref() {
        Some(bytes) => discover_from_slice(bytes)?,
        None => discover(&request.input_dem)?,
    };
    let discovery_ms = elapsed_ms(discovery_started);
    let plan = build_rewrite_plan(&plans, &discovery, request.cs2_game_dir.as_deref())?;
    let rewritten_knives = plan.knives.len();
    let rewritten_sticker_weapons = plan.sticker_weapons.len();
    let rewritten_sticker_slots = plan
        .sticker_weapons
        .values()
        .map(|rule| rule.kits.iter().filter(|kit| **kit > 0).count())
        .sum();
    let created_sticker_slots = plan.created_sticker_slots;
    let missing_sticker_slots = plan.missing_sticker_slots;
    debug_assert_eq!(
        rewritten_sticker_slots + missing_sticker_slots,
        plan.requested_sticker_slots
    );
    let counts = (
        plan.selected_pawns.len(),
        rewritten_knives,
        plan.gloves.len(),
        plan.weapons.len(),
        plan.painted_glove_passthroughs.len(),
        plan.painted_weapon_passthroughs.len(),
        plan.resources.len(),
    );
    if let Some(parent) = request.output_dem.parent() {
        std::fs::create_dir_all(parent)?;
    }
    let rewrite_started = Instant::now();
    let killfeed_rewriter = killfeed::KillfeedRewriter::new(&plans)?;
    let killfeed_enabled = killfeed_rewriter.enabled();
    let mut killfeed_audit = killfeed::Audit::default();
    let output = File::create(&request.output_dem)?;
    let output = BufWriter::with_capacity(IO_BUFFER_CAPACITY, output);
    match in_memory.as_deref() {
        Some(bytes) => {
            let mut writer = DemoWriter::from_slice(bytes, output)?;
            writer.add_rewriter(CosmeticRewriter {
                plan: plan.clone(),
                materialized_glove_team_by_pawn: HashMap::new(),
            });
            let killfeed_state = killfeed_enabled.then(|| writer.add_rewriter(killfeed_rewriter));
            writer.run()?;
            if let Some(state) = killfeed_state { killfeed_audit = std::mem::take(&mut state.borrow_mut().audit); }
        }
        None => {
            let input = File::open(&request.input_dem)?;
            let input = BufReader::with_capacity(IO_BUFFER_CAPACITY, input);
            let mut writer = DemoWriter::from_reader(input, output)?;
            writer.add_rewriter(CosmeticRewriter {
                plan: plan.clone(),
                materialized_glove_team_by_pawn: HashMap::new(),
            });
            let killfeed_state = killfeed_enabled.then(|| writer.add_rewriter(killfeed_rewriter));
            writer.run()?;
            if let Some(state) = killfeed_state { killfeed_audit = std::mem::take(&mut state.borrow_mut().audit); }
        }
    }
    let rewrite_ms = elapsed_ms(rewrite_started);
    let verification_started = Instant::now();
    let verified_death_events = killfeed::verify(&request.output_dem, &killfeed_audit)?;
    let rewritten_discovery = discover(&request.output_dem)?;
    let (verified_glove_records, verified_glove_created, verified_keychain_records, verified_keychain_created) =
        verify_glove_snapshots(&plan, &request.output_dem)?;
    let verified_gloves = verify_glove_rewrites(&plan, &rewritten_discovery)?;
    if verified_gloves != plan.gloves.len() {
        bail!(
            "glove verification count mismatch: planned {}, verified {verified_gloves}",
            plan.gloves.len()
        );
    }
    let verified_sticker_slots = verify_sticker_rewrites(&plan, &rewritten_discovery)?;
    if verified_sticker_slots != rewritten_sticker_slots {
        bail!(
            "sticker verification count mismatch: planned {rewritten_sticker_slots}, verified {verified_sticker_slots}"
        );
    }
    let verification_ms = elapsed_ms(verification_started);
    Ok(Response {
        ok: true,
        error: None,
        matched_player_count: plan.matched_players,
        skipped_player_count: plan.skipped_players,
        matched_knife_request_count: plan.matched_knife_requests,
        matched_glove_request_count: plan.matched_glove_requests,
        requested_sticker_slot_count: plan.requested_sticker_slots,
        pawn_count: counts.0,
        knife_count: counts.1,
        matched_knife_death_count: killfeed_audit.matched,
        rewritten_knife_death_count: killfeed_audit.changed,
        verified_death_event_count: verified_death_events,
        glove_count: verified_gloves,
        verified_glove_record_count: verified_glove_records,
        verified_glove_created_count: verified_glove_created,
        keychain_weapon_count: plan.keychain_weapons.len(),
        verified_keychain_record_count: verified_keychain_records,
        verified_keychain_created_count: verified_keychain_created,
        weapon_count: counts.3,
        painted_glove_passthrough_count: counts.4,
        painted_weapon_passthrough_count: counts.5,
        sticker_weapon_count: rewritten_sticker_weapons,
        sticker_slot_count: rewritten_sticker_slots,
        created_sticker_slot_count: created_sticker_slots,
        missing_sticker_slot_count: missing_sticker_slots,
        animation_resource_count: counts.6,
        animation_resource_source: if counts.6 == 0 {
            "none".to_string()
        } else {
            "installed_cs2".to_string()
        },
        input_mode,
        input_read_ms,
        discovery_ms,
        rewrite_ms,
        verification_ms,
        total_ms: elapsed_ms(total_started),
    })
}

fn arguments() -> Result<(PathBuf, PathBuf)> {
    let mut request = None;
    let mut response = None;
    let mut args = std::env::args_os().skip(1);
    while let Some(argument) = args.next() {
        match argument.to_string_lossy().as_ref() {
            "--request" => request = args.next().map(PathBuf::from),
            "--response" => response = args.next().map(PathBuf::from),
            other => bail!("unknown argument {other:?}"),
        }
    }
    Ok((
        request.context("missing --request")?,
        response.context("missing --response")?,
    ))
}

fn write_response(path: &Path, response: &Response) -> Result<()> {
    std::fs::write(path, serde_json::to_vec(response)?)?;
    Ok(())
}

fn run_main() -> Result<()> {
    let (request_path, response_path) = arguments()?;
    let result = (|| -> Result<Response> {
        let request: Request = serde_json::from_slice(
            &std::fs::read(&request_path)
                .with_context(|| format!("read request {}", request_path.display()))?,
        )?;
        rewrite(&request)
    })();
    match result {
        Ok(response) => write_response(&response_path, &response),
        Err(error) => {
            let response = Response {
                ok: false,
                error: Some(format!("{error:#}")),
                matched_player_count: 0,
                skipped_player_count: 0,
                matched_knife_request_count: 0,
                matched_glove_request_count: 0,
                requested_sticker_slot_count: 0,
                pawn_count: 0,
                knife_count: 0,
                matched_knife_death_count: 0,
                rewritten_knife_death_count: 0,
                verified_death_event_count: 0,
                glove_count: 0,
                verified_glove_record_count: 0,
                verified_glove_created_count: 0,
                keychain_weapon_count: 0,
                verified_keychain_record_count: 0,
                verified_keychain_created_count: 0,
                weapon_count: 0,
                painted_glove_passthrough_count: 0,
                painted_weapon_passthrough_count: 0,
                sticker_weapon_count: 0,
                sticker_slot_count: 0,
                created_sticker_slot_count: 0,
                missing_sticker_slot_count: 0,
                animation_resource_count: 0,
                animation_resource_source: String::new(),
                input_mode: String::new(),
                input_read_ms: 0,
                discovery_ms: 0,
                rewrite_ms: 0,
                verification_ms: 0,
                total_ms: 0,
            };
            let _ = write_response(&response_path, &response);
            Err(error)
        }
    }
}

fn main() -> Result<()> {
    std::thread::Builder::new()
        .name("nskinz-demo-rewrite".into())
        .stack_size(32 * 1024 * 1024)
        .spawn(run_main)
        .context("start demo rewrite worker")?
        .join()
        .map_err(|_| anyhow!("demo rewrite worker panicked"))?
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::{SystemTime, UNIX_EPOCH};

    fn temporary_directory(name: &str) -> PathBuf {
        let unique = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .expect("system clock before Unix epoch")
            .as_nanos();
        std::env::temp_dir().join(format!(
            "nskinz-demo-rewriter-{name}-{}-{unique}",
            std::process::id()
        ))
    }

    fn has_field(fields: &[(String, FieldValue)], name: &str, expected: FieldValue) -> bool {
        fields
            .iter()
            .any(|(field_name, value)| field_name == name && *value == expected)
    }

    #[test]
    fn glove_materialization_handles_late_teams_and_independent_created_records() {
        let handle = 7_471_247;
        let mut materialized = HashMap::new();

        // An entity that was created before m_iTeamNum arrived gets its full
        // glove state on the first update where the team-specific rule resolves.
        assert!(should_materialize_glove_rule(
            &mut materialized,
            handle,
            Team::Ct,
            EntityEvents::Updated
        ));
        assert!(!should_materialize_glove_rule(
            &mut materialized,
            handle,
            Team::Ct,
            EntityEvents::Updated
        ));

        // Reused Pawns can cross team epochs. Re-emit once for the new rule,
        // then avoid adding the same complete subtree to every later update.
        assert!(should_materialize_glove_rule(
            &mut materialized,
            handle,
            Team::T,
            EntityEvents::Updated
        ));
        assert!(!should_materialize_glove_rule(
            &mut materialized,
            handle,
            Team::T,
            EntityEvents::Updated
        ));

        // Seeking decodes another independent Created snapshot with the same
        // handle/team. It must contain all fields even after a prior rewrite.
        for _ in 0..4 {
            assert!(should_materialize_glove_rule(
                &mut materialized,
                handle,
                Team::T,
                EntityEvents::Created
            ));
        }
    }

    #[test]
    #[ignore = "requires NSKINZ_GLOVE_REGRESSION_REQUEST and NSKINZ_GLOVE_REJECT_DEMO fixtures"]
    fn real_demo_rejects_missing_seek_snapshots_and_rebuilds_all_epochs() {
        let request_path = std::env::var_os("NSKINZ_GLOVE_REGRESSION_REQUEST")
            .expect("set the real-demo rewrite request path");
        let bad_demo = std::env::var_os("NSKINZ_GLOVE_REJECT_DEMO")
            .expect("set the known failing output demo path");
        let request: Request =
            serde_json::from_slice(&std::fs::read(request_path).expect("read fixture request"))
                .expect("parse fixture request");
        let discovery = discover(&request.input_dem).expect("discover source");
        let plan = build_rewrite_plan(&parse_plans(&request).unwrap(), &discovery, None).unwrap();
        let failure = verify_glove_snapshots(&plan, Path::new(&bad_demo))
            .expect_err("a linear-only output must fail independent snapshot verification");
        assert!(format!("{failure:#}").contains("event Created"));
        eprintln!("known bad output rejected: {failure:#}");
        let response = rewrite(&request).expect("rebuild actual demo including seek snapshots");
        assert!(response.verified_glove_record_count > response.verified_glove_created_count);
        assert!(
            response.verified_glove_created_count > 1,
            "multiple independent snapshots must be checked"
        );
        eprintln!(
            "verified records={}, independent Created records={}",
            response.verified_glove_record_count, response.verified_glove_created_count
        );
    }

    #[test]
    #[ignore = "requires NSKINZ_DEFAULT_GLOVE_MATRIX with real source/output paths"]
    fn different_demos_rebuild_default_t_ct_gloves_and_verify_random_access() {
        #[derive(Deserialize)]
        struct Fixture {
            input_dem: PathBuf,
            output_dem: PathBuf,
        }
        let fixtures: Vec<Fixture> = serde_json::from_slice(
            &std::fs::read(
                std::env::var_os("NSKINZ_DEFAULT_GLOVE_MATRIX")
                    .expect("set the real-demo matrix JSON path"),
            )
            .unwrap(),
        )
        .unwrap();
        assert!(
            fixtures.len() >= 3,
            "use independent recordings, not variants of one Demo"
        );
        for fixture in fixtures {
            let source = discover(&fixture.input_dem).expect("discover matrix source");
            let mut passes = Vec::new();
            let accounts: BTreeSet<u64> = source.controller_steam.values().copied().collect();
            for steam in accounts {
                let pawns: Vec<_> = player_pawns(&source, steam)
                    .into_iter()
                    .filter_map(|handle| source.pawns.get(&handle))
                    .collect();
                let default_player = !pawns.is_empty()
                    && pawns.iter().all(|pawn| {
                        pawn.glove_definitions
                            .iter()
                            .all(|definition| matches!(definition, 5028 | 5029))
                    });
                if !default_player
                    || !pawns
                        .iter()
                        .any(|pawn| pawn.teams.iter().any(|team| matches!(team, 2 | 3)))
                {
                    continue;
                }
                passes.push(RequestPass {
                    keychains: Vec::new(),
                    steam_id64: steam.to_string(),
                    items: [(5028, 5034, 10033, "T"), (5029, 5030, 10037, "CT")]
                        .into_iter()
                        .map(|(definition, target, paint, team)| RequestItem {
                            definition_index: definition,
                            replacement_definition_index: Some(target),
                            preserve_source_definition: false,
                            paint_kit: paint,
                            quality: 0,
                            pattern_seed: 321.0,
                            wear: 0.07,
                            mesh_group_mask: None,
                            custom_name: "cross-demo glove regression".into(),
                            stickers: [0; STICKER_SLOT_COUNT],
                            sticker_transforms: [StickerTransform::default(); STICKER_SLOT_COUNT],
                            team: team.into(),
                        })
                        .collect(),
                });
            }
            assert!(
                !passes.is_empty(),
                "the source fixture must contain a default-glove player"
            );
            let request = Request {
                schema_version: 1,
                input_dem: fixture.input_dem,
                output_dem: fixture.output_dem,
                cs2_game_dir: None,
                passes,
            };
            let plan = build_rewrite_plan(&parse_plans(&request).unwrap(), &source, None).unwrap();
            let response =
                rewrite(&request).expect("cross-demo rebuild and complete snapshot verification");

            // Read each location through the Parser's independent seek path,
            // not through the writer used for production verification. Jump
            // backwards as well as forwards to exercise checkpoint restoration.
            let file = File::open(&request.output_dem).unwrap();
            let mut parser = Parser::from_reader(BufReader::new(file)).unwrap();
            parser.register_observer::<Discovery>();
            let last = parser.replay_info().playback_ticks().max(1) as u32;
            let mut seek_checks = 0;
            let mut seek_teams = BTreeSet::new();
            for step in [1, 3, 2, 7, 4, 6] {
                parser.jump_to_tick(last * step / 8).unwrap();
                for entity in parser.context().entities().iter() {
                    let Some((team, rule)) = glove_rule_for_entity_with_team(&plan, entity) else {
                        continue;
                    };
                    for (name, expected) in glove_fields(rule) {
                        assert_eq!(
                            entity.try_get_property(&name),
                            Some(&expected),
                            "{} seek to {}: handle {}, field {}",
                            request.input_dem.display(),
                            last * step / 8,
                            entity.handle(),
                            name
                        );
                    }
                    seek_checks += 1;
                    seek_teams.insert(team.number().unwrap_or_default());
                }
            }
            assert!(
                seek_checks > 0,
                "seek must encounter the selected default-glove players"
            );
            assert!(
                seek_teams.contains(&2) && seek_teams.contains(&3),
                "cross-demo fixture must exercise both default team glove epochs"
            );
            eprintln!("MATRIX PASS: {} players={} pawns={} records={} Created={} independent_seek_checks={} teams={:?}",
                request.input_dem.display(), response.matched_player_count, response.glove_count,
                response.verified_glove_record_count, response.verified_glove_created_count,
                seek_checks, seek_teams);
        }
    }

    #[test]
    fn sticker_ids_preserve_integer_union_bits() {
        let rule = StickerRule {
            kits: [76, 0, 0, 0, 0],
            expected_attribute_values: [[None; STICKER_ATTRIBUTE_COMPONENT_COUNT];
                STICKER_SLOT_COUNT],
            attribute_values: HashMap::from([(4, f32::from_bits(76))]),
            created_attributes: Vec::new(),
            attribute_count: 5,
        };
        let fields = sticker_fields(&rule, true);
        let encoded = FieldValue::Float(f32::from_bits(76));
        assert!(has_field(
            &fields,
            "m_AttributeManager.m_Item.m_NetworkedDynamicAttributes.m_Attributes.0004.m_iRawValue32",
            encoded.clone()
        ));
        assert!(has_field(
            &fields,
            "m_AttributeManager.m_Item.m_NetworkedDynamicAttributes.m_Attributes.0004.m_flInitialValue",
            encoded
        ));
        assert_eq!(
            sticker_field_replacement(
                &rule,
                "m_AttributeManager.m_Item.m_NetworkedDynamicAttributes.m_Attributes.0004.m_iRawValue32"
            ),
            Some(FieldValue::Float(f32::from_bits(76)))
        );
    }

    #[test]
    fn missing_sticker_slot_materializes_complete_attribute_set() {
        let steam = 76_561_198_812_513_923u64;
        let mut transforms = [StickerTransform::default(); STICKER_SLOT_COUNT];
        transforms[1] = StickerTransform {
            wear: 0.25,
            scale: 1.4,
            rotation: 37.0,
            ..Default::default()
        };
        let cosmetic = Cosmetic {
            definition: 60,
            preserve_source_definition: false,
            paint_kit: 0,
            quality: 0,
            seed: 0.0,
            wear: 0.0000001,
            mesh_group_mask: None,
            custom_name: String::new(),
            stickers: [0, 76, 0, 0, 0],
            sticker_transforms: transforms,
        };
        let mut any = TeamPlan::default();
        any.weapons.insert(60, cosmetic);
        let mut player = PlayerPlan::default();
        player.by_team.insert(Team::Any, any);
        let discovery = Discovery {
            controller_steam: HashMap::from([(1, steam)]),
            controller_pawns: HashMap::from([(1, HashSet::from([10]))]),
            pawns: HashMap::from([(
                10,
                PawnDiscovery {
                    controllers: HashSet::from([1]),
                    weapons: HashSet::from([100]),
                    ..Default::default()
                },
            )]),
            weapons: HashMap::from([(
                100,
                WeaponDiscovery {
                    definition: 60,
                    paint_attributes: PaintAttributeLayout {
                        count: 2,
                        ..Default::default()
                    },
                    ..Default::default()
                },
            )]),
            ..Default::default()
        };

        let plan = build_rewrite_plan(&HashMap::from([(steam, player)]), &discovery, None)
            .expect("missing sticker slot should be materialized");
        assert_eq!(plan.requested_sticker_slots, 1);
        assert_eq!(plan.created_sticker_slots, 1);
        assert_eq!(plan.missing_sticker_slots, 0);
        assert!(
            plan.weapons.is_empty(),
            "sticker-only plans must stay sticker-only"
        );
        let rule = plan.sticker_weapons.get(&100).expect("sticker rule");
        assert_eq!(rule.attribute_count, 6);
        assert_eq!(rule.created_attributes.len(), 4);
        let fields = sticker_fields(rule, true);
        assert!(has_field(
            &fields,
            "m_AttributeManager.m_Item.m_NetworkedDynamicAttributes.m_Attributes",
            FieldValue::Unsigned32(6),
        ));
        assert!(has_field(
            &fields,
            "m_AttributeManager.m_Item.m_NetworkedDynamicAttributes.m_Attributes.0002.m_iAttributeDefinitionIndex",
            FieldValue::Unsigned16(117),
        ));
        assert!(has_field(
            &fields,
            "m_AttributeManager.m_Item.m_NetworkedDynamicAttributes.m_Attributes.0002.m_iRawValue32",
            FieldValue::Float(f32::from_bits(76)),
        ));
        assert!(has_field(
            &fields,
            "m_AttributeManager.m_Item.m_NetworkedDynamicAttributes.m_Attributes.0003.m_iAttributeDefinitionIndex",
            FieldValue::Unsigned16(118),
        ));
        assert!(has_field(
            &fields,
            "m_AttributeManager.m_Item.m_NetworkedDynamicAttributes.m_Attributes.0003.m_iRawValue32",
            FieldValue::Float(0.25),
        ));
        assert!(has_field(
            &fields,
            "m_AttributeManager.m_Item.m_NetworkedDynamicAttributes.m_Attributes.0004.m_iRawValue32",
            FieldValue::Float(1.4),
        ));
        assert!(has_field(
            &fields,
            "m_AttributeManager.m_Item.m_NetworkedDynamicAttributes.m_Attributes.0005.m_iRawValue32",
            FieldValue::Float(37.0),
        ));
    }

    #[test]
    fn custom_position_extends_existing_slot_with_modern_placement_attributes() {
        let steam = 76_561_198_812_513_923u64;
        let mut transforms = [StickerTransform::default(); STICKER_SLOT_COUNT];
        transforms[0] = StickerTransform {
            wear: 0.2,
            scale: 1.25,
            rotation: 42.0,
            custom_position: true,
            offset_x: 0.175,
            offset_y: -0.225,
            schema: 0,
        };
        let cosmetic = Cosmetic {
            definition: 60,
            preserve_source_definition: false,
            paint_kit: 0,
            quality: 0,
            seed: 0.0,
            wear: 0.0000001,
            mesh_group_mask: None,
            custom_name: String::new(),
            stickers: [76, 0, 0, 0, 0],
            sticker_transforms: transforms,
        };
        let mut any = TeamPlan::default();
        any.weapons.insert(60, cosmetic);
        let mut player = PlayerPlan::default();
        player.by_team.insert(Team::Any, any);
        let mut weapon = WeaponDiscovery {
            definition: 60,
            paint_attributes: PaintAttributeLayout {
                count: 2,
                ..Default::default()
            },
            ..Default::default()
        };
        weapon.sticker_slots[0] = true;
        weapon.sticker_kits[0].insert(1);
        weapon.sticker_attribute_indices[0][0].insert(1);
        weapon.sticker_attribute_values[0][0].insert(1);
        let discovery = Discovery {
            controller_steam: HashMap::from([(1, steam)]),
            controller_pawns: HashMap::from([(1, HashSet::from([10]))]),
            pawns: HashMap::from([(
                10,
                PawnDiscovery {
                    controllers: HashSet::from([1]),
                    weapons: HashSet::from([100]),
                    ..Default::default()
                },
            )]),
            weapons: HashMap::from([(100, weapon)]),
            ..Default::default()
        };

        let plan = build_rewrite_plan(&HashMap::from([(steam, player)]), &discovery, None)
            .expect("existing sticker slot should gain custom placement attributes");
        assert_eq!(plan.created_sticker_slots, 0);
        let rule = plan.sticker_weapons.get(&100).expect("sticker rule");
        assert_eq!(rule.attribute_count, 8);
        assert_eq!(rule.created_attributes.len(), 6);
        assert_eq!(rule.attribute_values.get(&1), Some(&f32::from_bits(76)));
        let fields = sticker_fields(rule, true);
        for (index, definition) in [114u16, 115, 116, 278, 279, 290].into_iter().enumerate() {
            assert!(has_field(
                &fields,
                &format!(
                    "m_AttributeManager.m_Item.m_NetworkedDynamicAttributes.m_Attributes.{:04}.m_iAttributeDefinitionIndex",
                    index + 2
                ),
                FieldValue::Unsigned16(definition),
            ));
        }
        assert!(has_field(
            &fields,
            "m_AttributeManager.m_Item.m_NetworkedDynamicAttributes.m_Attributes.0005.m_iRawValue32",
            FieldValue::Float(0.175),
        ));
        assert!(has_field(
            &fields,
            "m_AttributeManager.m_Item.m_NetworkedDynamicAttributes.m_Attributes.0006.m_iRawValue32",
            FieldValue::Float(-0.225),
        ));
        assert!(has_field(
            &fields,
            "m_AttributeManager.m_Item.m_NetworkedDynamicAttributes.m_Attributes.0007.m_iRawValue32",
            FieldValue::Float(f32::from_bits(0)),
        ));
    }

    #[test]
    fn missing_sticker_attributes_follow_materialized_paint_attributes() {
        let steam = 76_561_198_812_513_923u64;
        let cosmetic = Cosmetic {
            definition: 60,
            preserve_source_definition: false,
            paint_kit: 1177,
            quality: 4,
            seed: 321.0,
            wear: 0.17,
            mesh_group_mask: Some(1),
            custom_name: "paint and sticker".into(),
            stickers: [76, 0, 0, 0, 0],
            sticker_transforms: [StickerTransform::default(); STICKER_SLOT_COUNT],
        };
        let mut any = TeamPlan::default();
        any.weapons.insert(60, cosmetic);
        let mut player = PlayerPlan::default();
        player.by_team.insert(Team::Any, any);
        let discovery = Discovery {
            controller_steam: HashMap::from([(1, steam)]),
            controller_pawns: HashMap::from([(1, HashSet::from([10]))]),
            pawns: HashMap::from([(
                10,
                PawnDiscovery {
                    controllers: HashSet::from([1]),
                    weapons: HashSet::from([100]),
                    ..Default::default()
                },
            )]),
            weapons: HashMap::from([(
                100,
                WeaponDiscovery {
                    definition: 60,
                    ..Default::default()
                },
            )]),
            ..Default::default()
        };

        let plan = build_rewrite_plan(&HashMap::from([(steam, player)]), &discovery, None)
            .expect("paint and missing sticker attributes should share one vector");
        let weapon = plan.weapons.get(&100).expect("weapon rule");
        assert_eq!(weapon.paint_attributes.indices, [0, 1, 2]);
        assert_eq!(weapon.paint_attributes.count, 7);
        assert!(has_field(
            &weapon_fields(weapon),
            "m_AttributeManager.m_Item.m_NetworkedDynamicAttributes.m_Attributes",
            FieldValue::Unsigned32(7),
        ));
        let sticker = plan.sticker_weapons.get(&100).expect("sticker rule");
        assert_eq!(sticker.attribute_count, 7);
        assert_eq!(sticker.created_attributes[0].index, 3);
        assert!(!sticker_fields(sticker, false).iter().any(|(name, _)| {
            name == "m_AttributeManager.m_Item.m_NetworkedDynamicAttributes.m_Attributes"
        }));
    }

    #[test]
    fn existing_paint_attribute_slots_are_reused_without_shrinking_vectors() {
        let layout = PaintAttributeLayout {
            count: 9,
            indices: [Some(4), None, Some(7)],
        };
        let materialized = materialize_paint_attributes(layout);
        assert_eq!(materialized.indices, [4, 9, 7]);
        assert_eq!(materialized.count, 10);

        let complete = materialize_paint_attributes(PaintAttributeLayout {
            count: 12,
            indices: [Some(8), Some(2), Some(11)],
        });
        assert_eq!(complete.indices, [8, 2, 11]);
        assert_eq!(complete.count, 12);
    }

    #[test]
    fn rewrite_plan_overwrites_existing_weapon_and_glove_paint() {
        let steam = 76_561_198_812_513_923u64;
        let weapon_cosmetic = Cosmetic {
            definition: 60,
            preserve_source_definition: false,
            paint_kit: 1177,
            quality: 4,
            seed: 321.0,
            wear: 0.17,
            mesh_group_mask: Some(1),
            custom_name: "weapon replacement".into(),
            stickers: [0; STICKER_SLOT_COUNT],
            sticker_transforms: [StickerTransform::default(); STICKER_SLOT_COUNT],
        };
        let glove_cosmetic = Cosmetic {
            definition: 5034,
            preserve_source_definition: false,
            paint_kit: 10033,
            quality: 3,
            seed: 7.0,
            wear: 0.22,
            mesh_group_mask: None,
            custom_name: "glove replacement".into(),
            stickers: [0; STICKER_SLOT_COUNT],
            sticker_transforms: [StickerTransform::default(); STICKER_SLOT_COUNT],
        };
        let mut any = TeamPlan::default();
        any.weapons.insert(60, weapon_cosmetic);
        let mut ct = TeamPlan::default();
        ct.glove = Some(glove_cosmetic);
        let mut player = PlayerPlan::default();
        player.by_team.insert(Team::Any, any);
        player.by_team.insert(Team::Ct, ct);

        let weapon_discovery = WeaponDiscovery {
            definition: 60,
            paint_attributes: PaintAttributeLayout {
                count: 10,
                indices: [Some(3), Some(6), Some(8)],
            },
            ..Default::default()
        };
        let pawn_discovery = PawnDiscovery {
            controllers: HashSet::from([1]),
            weapons: HashSet::from([100]),
            teams: HashSet::from([3]),
            glove_definitions_by_team: HashMap::from([(3, 5031)]),
            glove_paint_attributes_by_team: HashMap::from([(
                3,
                PaintAttributeLayout {
                    count: 7,
                    indices: [Some(1), Some(4), Some(5)],
                },
            )]),
            ..Default::default()
        };
        let discovery = Discovery {
            controller_steam: HashMap::from([(1, steam)]),
            controller_pawns: HashMap::from([(1, HashSet::from([10]))]),
            pawns: HashMap::from([(10, pawn_discovery)]),
            weapons: HashMap::from([(100, weapon_discovery)]),
            ..Default::default()
        };

        let plan = build_rewrite_plan(&HashMap::from([(steam, player)]), &discovery, None)
            .expect("painted source items should be rewritten in place");
        assert!(plan.painted_weapon_passthroughs.is_empty());
        assert!(plan.painted_glove_passthroughs.is_empty());
        let weapon = plan.weapons.get(&100).expect("existing weapon rewrite");
        assert_eq!(weapon.paint_attributes.indices, [3, 6, 8]);
        assert_eq!(weapon.paint_attributes.count, 10);
        let glove = plan
            .gloves
            .get(&10)
            .and_then(|rules| rules.get(&Team::Ct))
            .expect("existing CT glove rewrite");
        assert_eq!(glove.paint_attributes.indices, [1, 4, 5]);
        assert_eq!(glove.paint_attributes.count, 7);
        assert!(has_field(
            &weapon_fields(weapon),
            "m_AttributeManager.m_Item.m_NetworkedDynamicAttributes.m_Attributes.0003.m_iRawValue32",
            FieldValue::Float(1177.0),
        ));
        assert!(has_field(
            &glove_fields(glove),
            "m_EconGloves.m_NetworkedDynamicAttributes.m_Attributes.0001.m_iRawValue32",
            FieldValue::Float(10033.0),
        ));
    }

    #[test]
    fn rewrite_plan_skips_configured_players_missing_from_demo() {
        let valid_steam = 76_561_198_812_513_923u64;
        let missing_steam = 76_561_199_840_349_579u64;
        let cosmetic = Cosmetic {
            definition: 60,
            preserve_source_definition: false,
            paint_kit: 0,
            quality: 0,
            seed: 0.0,
            wear: 0.0000001,
            mesh_group_mask: None,
            custom_name: String::new(),
            stickers: [76, 0, 0, 0, 0],
            sticker_transforms: [StickerTransform::default(); STICKER_SLOT_COUNT],
        };
        let mut valid_team = TeamPlan::default();
        valid_team.weapons.insert(60, cosmetic.clone());
        let mut valid_plan = PlayerPlan::default();
        valid_plan.by_team.insert(Team::Any, valid_team);

        let mut missing_team = TeamPlan::default();
        missing_team.weapons.insert(60, cosmetic);
        let mut missing_plan = PlayerPlan::default();
        missing_plan.by_team.insert(Team::Any, missing_team);
        let plans = HashMap::from([(valid_steam, valid_plan), (missing_steam, missing_plan)]);

        let mut discovered_weapon = WeaponDiscovery::default();
        discovered_weapon.definition = 60;
        discovered_weapon.paint_attributes.count = 1;
        discovered_weapon.sticker_slots[0] = true;
        discovered_weapon.sticker_attribute_indices[0][0].insert(0);
        let discovery = Discovery {
            controller_steam: HashMap::from([(1, valid_steam)]),
            controller_pawns: HashMap::from([(1, HashSet::from([10]))]),
            pawns: HashMap::from([(
                10,
                PawnDiscovery {
                    controllers: HashSet::from([1]),
                    weapons: HashSet::from([100]),
                    ..Default::default()
                },
            )]),
            weapons: HashMap::from([(100, discovered_weapon)]),
            ..Default::default()
        };

        let plan = build_rewrite_plan(&plans, &discovery, None)
            .expect("valid demo player should survive an unrelated configured player");
        assert_eq!(plan.matched_players, 1);
        assert_eq!(plan.skipped_players, 1);
        assert_eq!(plan.requested_sticker_slots, 1);
        assert_eq!(plan.sticker_weapons.len(), 1);
    }

    #[test]
    fn request_cosmetic_ranges_match_cs2_fields() {
        let request = Request {
            schema_version: 1,
            input_dem: PathBuf::from("input.dem"),
            output_dem: PathBuf::from("output.dem"),
            cs2_game_dir: None,
            passes: vec![RequestPass {
                keychains: Vec::new(),
                steam_id64: "76561199840349579".into(),
                items: vec![
                    RequestItem {
                        definition_index: 7,
                        replacement_definition_index: None,
                        preserve_source_definition: false,
                        paint_kit: 801,
                        quality: 0,
                        pattern_seed: -1.0,
                        wear: 0.0000001,
                        mesh_group_mask: Some(2),
                        custom_name: String::new(),
                        stickers: [76, 0, 0, 0, 0],
                        sticker_transforms: [StickerTransform::default(); STICKER_SLOT_COUNT],
                        team: "ANY".into(),
                    },
                    RequestItem {
                        definition_index: 16,
                        replacement_definition_index: None,
                        preserve_source_definition: false,
                        paint_kit: 844,
                        quality: 0,
                        pattern_seed: 1001.0,
                        wear: 2.0,
                        mesh_group_mask: None,
                        custom_name: "M4 test".into(),
                        stickers: [0; STICKER_SLOT_COUNT],
                        sticker_transforms: [StickerTransform::default(); STICKER_SLOT_COUNT],
                        team: "ANY".into(),
                    },
                ],
            }],
        };
        let plans = parse_plans(&request).expect("parse cosmetic range request");
        let any = plans
            .get(&76_561_199_840_349_579)
            .and_then(|plan| plan.by_team.get(&Team::Any))
            .expect("ANY team plan");
        let ak = any.weapons.get(&7).expect("AK plan");
        assert_eq!(ak.seed, 0.0);
        assert_eq!(ak.wear.to_bits(), 0.0000001f32.to_bits());
        assert_eq!(ak.mesh_group_mask, Some(2));
        assert_eq!(ak.custom_name, "nSkinz CS2");
        assert_eq!(ak.stickers, [76, 0, 0, 0, 0]);
        let m4 = any.weapons.get(&16).expect("M4 plan");
        assert_eq!(m4.seed, 1000.0);
        assert_eq!(m4.wear, 1.0);
        assert_eq!(m4.custom_name, "M4 test");
    }

    fn make_vpk_with_entry(path: &Path, resource: &str) {
        let (directory_and_name, extension) = resource
            .rsplit_once('.')
            .expect("fixture resource must have an extension");
        let (directory, filename) = directory_and_name
            .rsplit_once('/')
            .expect("fixture resource must have a directory");

        let mut tree = Vec::new();
        tree.extend_from_slice(extension.as_bytes());
        tree.push(0);
        tree.extend_from_slice(directory.as_bytes());
        tree.push(0);
        tree.extend_from_slice(filename.as_bytes());
        tree.push(0);
        tree.extend_from_slice(&0u32.to_le_bytes()); // CRC32 is irrelevant to the directory lookup.
        tree.extend_from_slice(&0u16.to_le_bytes()); // No preload bytes.
        tree.extend_from_slice(&0u16.to_le_bytes()); // Archive index.
        tree.extend_from_slice(&0u32.to_le_bytes()); // Archive offset.
        tree.extend_from_slice(&1u32.to_le_bytes()); // A non-empty compiled resource.
        tree.extend_from_slice(&0xFFFFu16.to_le_bytes());
        tree.extend_from_slice(&[0, 0, 0]); // End filename, directory and extension lists.

        let mut bytes = Vec::new();
        bytes.extend_from_slice(&VPK_SIGNATURE.to_le_bytes());
        bytes.extend_from_slice(&1u32.to_le_bytes());
        bytes.extend_from_slice(&(tree.len() as u32).to_le_bytes());
        bytes.extend_from_slice(&tree);
        std::fs::write(path, bytes).expect("write VPK fixture");
    }

    #[test]
    fn weapon_animation_metadata_matches_cs2_string_table_layout() {
        assert_eq!(
            WEAPON_ANIM_ASSET_METADATA,
            &[
                1, 0, 0, 0, 3, 0, 0, 0, b'w', b'e', b'a', b'p', b'o', b'n', 0, b'w', b'e', b'a',
                b'p', b'o', b'n', b'_', b'o', b'f', b'f', b's', b'e', b't', 0, b'e', b'c', b'o',
                b'n', 0, 0, 0, 0, 0,
            ]
        );
    }

    #[test]
    fn installed_animation_resource_is_resolved_from_cs2_vpk() {
        let root = temporary_directory("vpk-resource");
        std::fs::create_dir_all(&root).expect("create VPK fixture directory");
        let vpk = root.join("pak01_dir.vpk");
        make_vpk_with_entry(&vpk, "animation/skeletons/weapons/knife_m9.vnmskel_c");

        assert!(
            vpk_contains_path(&vpk, "animation/skeletons/weapons/knife_m9.vnmskel_c")
                .expect("read VPK fixture")
        );
        assert!(
            !vpk_contains_path(&vpk, "animation/skeletons/weapons/knife_karambit.vnmskel_c")
                .expect("read VPK fixture")
        );
        require_installed_resource(&root, "animation/skeletons/weapons/knife_m9.vnmskel")
            .expect("resolve installed animation resource from VPK");

        std::fs::remove_dir_all(&root).expect("remove VPK fixture directory");
    }

    #[test]
    fn installed_animation_resource_accepts_loose_compiled_file() {
        let root = temporary_directory("loose-resource");
        let resource = root.join("animation/skeletons/weapons/knife_default_t.vnmskel_c");
        std::fs::create_dir_all(resource.parent().expect("resource parent"))
            .expect("create loose resource directory");
        std::fs::write(&resource, [1u8]).expect("write loose resource fixture");

        require_installed_resource(&root, "animation/skeletons/weapons/knife_default_t.vnmskel")
            .expect("resolve loose installed animation resource");

        std::fs::remove_dir_all(&root).expect("remove loose resource fixture directory");
    }

    #[test]
    fn installed_knife_variants_cover_every_supported_definition() {
        let expected_models = [
            (42, 0x9322_FB72_0F3E_1DB5),
            (59, 0x66AC_5E62_9C8C_6E6C),
            (500, 0xD3FF_68A1_E5D0_94B8),
            (503, 0xD122_A4BD_B5AB_0BE0),
            (505, 0x4BD0_0438_B87D_B94F),
            (506, 0x67B9_19D8_9B78_02B0),
            (507, 0x14BD_F873_249B_A0B5),
            (508, 0xBBB5_EF19_8628_5654),
            (509, 0x5810_71FC_7EE2_D9FF),
            (512, 0x4D2E_C28F_4D17_3446),
            (514, 0x66F6_6989_F99F_7220),
            (515, 0xBB0B_CD50_C58C_C3D9),
            (516, 0xF81C_0236_F8DF_3E9D),
            (517, 0x26A1_2D7F_5921_DC1E),
            (518, 0xC14F_A104_1260_7942),
            (519, 0xBA79_58CF_5F07_96EC),
            (520, 0xDFFD_0BB1_935D_FEB1),
            (521, 0x788D_7EC6_428F_5E66),
            (522, 0x75EC_08DC_90C3_5EB4),
            (523, 0x1760_BBE9_8C91_7B4A),
            (525, 0x6402_A1DF_A760_7197),
            (526, 0xBB68_2220_DF9D_8D89),
        ];
        assert_eq!(KNIFE_ASSETS.len(), expected_models.len());
        for (definition, model) in expected_models {
            let variant = installed_variant(definition).expect("supported knife variant");
            assert_eq!(variant.model, model, "definition {definition} model hash");
            assert_eq!(
                variant.subclass,
                murmur_hash2_ascii_ci(&definition.to_string(), 0x3141_5926),
                "definition {definition} subclass token"
            );
            assert_ne!(variant.model, 0);
            assert_ne!(variant.subclass, 0);
        }
        assert_eq!(installed_variant(500).unwrap().subclass, 3_933_374_535);
        assert!(installed_variant(524).is_none());
    }

    #[test]
    fn installed_knife_variant_matches_native_demo_samples() {
        let samples = [
            (42, 10_602_312_940_084_141_493, 294_910_436),
            (59, 7_398_392_065_489_137_260, 1_058_568_214),
            (503, 15_069_788_437_838_760_928, 3_787_235_507),
            (507, 1_494_623_824_775_979_189, 1_731_408_398),
            (508, 13_525_979_948_781_164_116, 1_638_561_588),
            (515, 13_478_092_056_631_755_737, 1_353_709_123),
            (521, 8_686_738_646_252_871_270, 3_206_681_373),
        ];
        for (definition, model, subclass) in samples {
            assert_eq!(
                installed_variant(definition),
                Some(KnifeVariant { model, subclass }),
                "definition {definition}"
            );
        }
    }

    #[test]
    fn default_cosmetic_rebuilds_materialize_complete_paint_attributes() {
        let cosmetic = Cosmetic {
            definition: 5034,
            preserve_source_definition: false,
            paint_kit: 10033,
            quality: 3,
            seed: 950.0,
            wear: 0.226326,
            mesh_group_mask: None,
            custom_name: "nSkinz CS2".into(),
            stickers: [0; STICKER_SLOT_COUNT],
            sticker_transforms: [StickerTransform::default(); STICKER_SLOT_COUNT],
        };
        let glove = glove_fields(&GloveRule {
            steam: 76_561_199_674_674_473,
            cosmetic: cosmetic.clone(),
            paint_attributes: materialize_paint_attributes(Default::default()),
        });
        assert!(has_field(
            &glove,
            "m_EconGloves.m_NetworkedDynamicAttributes.m_Attributes",
            FieldValue::Unsigned32(3)
        ));
        assert!(has_field(
            &glove,
            "m_EconGloves.m_szCustomName",
            FieldValue::String("nSkinz CS2".into())
        ));
        assert!(has_field(
            &glove,
            "m_EconGloves.m_NetworkedDynamicAttributes.m_Attributes.0000.m_iAttributeDefinitionIndex",
            FieldValue::Unsigned16(6)
        ));
        assert!(has_field(
            &glove,
            "m_EconGloves.m_NetworkedDynamicAttributes.m_Attributes.0000.m_iRawValue32",
            FieldValue::Float(10033.0)
        ));
        assert!(has_field(
            &glove,
            "m_EconGloves.m_NetworkedDynamicAttributes.m_Attributes.0001.m_iAttributeDefinitionIndex",
            FieldValue::Unsigned16(7)
        ));
        assert!(has_field(
            &glove,
            "m_EconGloves.m_NetworkedDynamicAttributes.m_Attributes.0002.m_iAttributeDefinitionIndex",
            FieldValue::Unsigned16(8)
        ));
        assert!(has_field(
            &glove,
            "CBodyComponent.m_skeletonInstance.m_modelState.m_nBodyGroupChoices.0000",
            FieldValue::Signed32(1)
        ));
        assert!(has_field(
            &glove,
            "CBodyComponent.m_skeletonInstance.m_modelState.m_nBodyGroupChoices.0001",
            FieldValue::Signed32(1)
        ));
        assert!(has_field(
            &glove,
            "m_EconGloves.m_iEntityQuality",
            FieldValue::Signed32(3)
        ));
        assert!(has_field(
            &glove,
            "m_nEconGlovesChanged",
            FieldValue::Unsigned8(1)
        ));
        let weapon = weapon_fields(&WeaponRule {
            steam: 76_561_199_674_674_473,
            cosmetic: Cosmetic {
                definition: 44,
                preserve_source_definition: false,
                paint_kit: 1,
                quality: 0,
                seed: 3.0,
                wear: 0.2,
                mesh_group_mask: Some(2),
                custom_name: "AK custom".into(),
                stickers: [0; STICKER_SLOT_COUNT],
                sticker_transforms: [StickerTransform::default(); STICKER_SLOT_COUNT],
            },
            paint_attributes: materialize_paint_attributes(Default::default()),
        });
        assert!(has_field(
            &weapon,
            "m_AttributeManager.m_Item.m_NetworkedDynamicAttributes.m_Attributes",
            FieldValue::Unsigned32(3)
        ));
        assert!(has_field(
            &weapon,
            "m_AttributeManager.m_Item.m_NetworkedDynamicAttributes.m_Attributes.0000.m_iRawValue32",
            FieldValue::Float(1.0)
        ));
        assert!(has_field(
            &weapon,
            "m_nFallbackPaintKit",
            FieldValue::Signed32(1)
        ));
        assert!(has_field(
            &weapon,
            "m_AttributeManager.m_Item.m_szCustomName",
            FieldValue::String("AK custom".into())
        ));
        assert!(has_field(
            &weapon,
            "CBodyComponent.m_skeletonInstance.m_modelState.m_MeshGroupMask",
            FieldValue::Unsigned64(2)
        ));
    }
}
