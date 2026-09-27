use anyhow::{anyhow, bail, Context as _, Result};
use serde::{Deserialize, Serialize};
use source2_demo::prelude::*;
use source2_demo::proto::{c_demo_string_tables::ItemsT, CDemoStringTables, CSvcMsgCreateStringTable};
use source2_demo::writer::{
    insert_create_string_table_entry, DemoRewriter, DemoWriter, MessageRewrite,
    RewriteInterests, StringTableEntryUpdate,
};
use std::collections::{BTreeSet, HashMap, HashSet};
use std::fs::File;
use std::io::{BufReader, BufWriter};
use std::path::{Path, PathBuf};

#[derive(Debug, Deserialize)]
struct Request {
    schema_version: u32,
    input_dem: PathBuf,
    output_dem: PathBuf,
    passes: Vec<RequestPass>,
}

#[derive(Debug, Deserialize)]
struct RequestPass {
    steam_id64: String,
    items: Vec<RequestItem>,
}

#[derive(Clone, Debug, Deserialize)]
struct RequestItem {
    definition_index: u16,
    replacement_definition_index: Option<u16>,
    paint_kit: i32,
    pattern_seed: f32,
    wear: f32,
    team: String,
}

#[derive(Debug, Serialize)]
struct Response {
    ok: bool,
    error: Option<String>,
    pawn_count: usize,
    knife_count: usize,
    glove_count: usize,
    animation_resource_count: usize,
}

#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
enum Team {
    T,
    Ct,
}

impl Team {
    fn parse(value: &str) -> Result<Self> {
        match value.to_ascii_uppercase().as_str() {
            "T" => Ok(Self::T),
            "CT" => Ok(Self::Ct),
            _ => bail!("unsupported team {value:?}"),
        }
    }

    fn number(self) -> u8 {
        match self {
            Self::T => 2,
            Self::Ct => 3,
        }
    }
}

#[derive(Clone, Debug)]
struct Cosmetic {
    definition: u16,
    paint_kit: i32,
    seed: f32,
    wear: f32,
}

#[derive(Default)]
struct TeamPlan {
    knife: Option<Cosmetic>,
    glove: Option<Cosmetic>,
}

#[derive(Default)]
struct PlayerPlan {
    by_team: HashMap<Team, TeamPlan>,
}

#[derive(Clone, Copy, Debug)]
struct KnifeVariant {
    model: u64,
    subclass: u32,
}

#[derive(Default)]
struct PawnDiscovery {
    controllers: HashSet<u32>,
    weapons: HashSet<u32>,
    teams: HashSet<u8>,
}

#[derive(Default)]
struct Discovery {
    controller_steam: HashMap<u32, u64>,
    controller_pawns: HashMap<u32, HashSet<u32>>,
    pawns: HashMap<u32, PawnDiscovery>,
    knives: HashSet<u32>,
    variants: HashMap<u16, KnifeVariant>,
    anim_assets: HashMap<String, Vec<u8>>,
}

fn integer(value: &FieldValue) -> Option<u64> {
    value.try_into().ok()
}

fn field_integer(entity: &Entity, name: &str) -> Option<u64> {
    entity.get_property(name).ok().and_then(integer)
}

impl Observer for Discovery {
    fn interests(&self) -> Interests {
        Interests::ENTITY_STATE
            | Interests::ENTITY_EVENTS
            | Interests::STRING_TABLE_STATE
            | Interests::STRING_TABLE_ENTRIES
            | Interests::REPLAY_END
    }

    fn on_entity(&mut self, _ctx: &Context, event: EntityEvents, entity: &Entity) -> ObserverResult {
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
            for field in entity.fields() {
                let name = field.name.to_ascii_lowercase();
                if !(name.ends_with("m_hplayerpawn") || name.ends_with("m_hpawn")) {
                    continue;
                }
                if let Some(pawn) = field.value.and_then(integer).map(|value| value as u32) {
                    if pawn != u32::MAX && pawn != 0xFFFF_FFFE {
                        self.controller_pawns.entry(handle).or_default().insert(pawn);
                    }
                }
            }
        } else if class == "CCSPlayerPawn" {
            let pawn = self.pawns.entry(handle).or_default();
            for field in entity.fields() {
                let name = field.name.to_ascii_lowercase();
                let Some(value) = field.value.and_then(integer) else {
                    continue;
                };
                if name == "m_iteamnum" {
                    pawn.teams.insert(value as u8);
                } else if name.contains("controller") && name.contains("m_h") {
                    let controller = value as u32;
                    if controller != u32::MAX && controller != 0xFFFF_FFFE {
                        pawn.controllers.insert(controller);
                    }
                } else if name.starts_with("m_pweaponservices.m_hmyweapons.") {
                    let weapon = value as u32;
                    if weapon != u32::MAX && weapon != 0xFFFF_FFFE {
                        pawn.weapons.insert(weapon);
                    }
                }
            }
        } else if class == "CKnife" {
            self.knives.insert(handle);
            let definition = field_integer(entity, "m_AttributeManager.m_Item.m_iItemDefinitionIndex")
                .map(|value| value as u16);
            let model = field_integer(
                entity,
                "CBodyComponent.m_skeletonInstance.m_modelState.m_hModel",
            );
            let subclass = field_integer(entity, "m_nSubclassID").map(|value| value as u32);
            if let (Some(definition), Some(model), Some(subclass)) = (definition, model, subclass) {
                if definition != 0 && model != 0 && subclass != 0 {
                    self.variants.insert(definition, KnifeVariant { model, subclass });
                }
            }
        }
        Ok(())
    }

    fn on_stop(&mut self, ctx: &Context) -> ObserverResult {
        for table in ctx.string_tables().iter() {
            if !table.name().eq_ignore_ascii_case("AnimAssetData") {
                continue;
            }
            for row in table.iter() {
                if let Some(value) = row.value() {
                    self.anim_assets.insert(row.key().to_string(), value.to_vec());
                }
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
        for item in &pass.items {
            let team = Team::parse(&item.team)?;
            let target = item
                .replacement_definition_index
                .unwrap_or(item.definition_index);
            let cosmetic = Cosmetic {
                definition: target,
                paint_kit: item.paint_kit,
                seed: item.pattern_seed,
                wear: item.wear.clamp(0.000001, 1.0),
            };
            let team_plan = plan.by_team.entry(team).or_default();
            match item.definition_index {
                42 | 59 => team_plan.knife = Some(cosmetic),
                5028 | 5029 => team_plan.glove = Some(cosmetic),
                other => bail!("unsupported demo cosmetic source definition {other}"),
            }
        }
    }
    if result.is_empty() {
        bail!("request contains no usable player cosmetic passes");
    }
    Ok(result)
}

fn fallback_variant(definition: u16) -> Option<KnifeVariant> {
    let (model, subclass) = match definition {
        42 => (10_602_312_940_084_141_493, 294_910_436),
        59 => (7_398_392_065_482_137_260, 1_058_568_214),
        503 => (15_069_788_437_838_760_928, 3_787_235_507),
        507 => (1_494_623_824_775_979_189, 1_731_408_398),
        508 => (13_525_979_948_781_164_116, 1_638_561_588),
        515 => (13_478_092_056_631_755_737, 1_353_709_123),
        521 => (8_686_738_646_252_871_270, 3_206_681_373),
        _ => return None,
    };
    Some(KnifeVariant { model, subclass })
}

fn skeleton_for(definition: u16) -> Option<&'static str> {
    Some(match definition {
        42 => "animation/skeletons/weapons/knife_default_ct.vnmskel",
        59 => "animation/skeletons/weapons/knife_default_t.vnmskel",
        500 => "animation/skeletons/weapons/knife_bayonet.vnmskel",
        503 => "animation/skeletons/weapons/knife_css.vnmskel",
        505 => "animation/skeletons/weapons/knife_flip.vnmskel",
        506 => "animation/skeletons/weapons/knife_gut.vnmskel",
        507 => "animation/skeletons/weapons/knife_karambit.vnmskel",
        508 => "animation/skeletons/weapons/knife_m9.vnmskel",
        509 => "animation/skeletons/weapons/knife_tactical.vnmskel",
        512 => "animation/skeletons/weapons/knife_falchion.vnmskel",
        514 => "animation/skeletons/weapons/knife_bowie.vnmskel",
        515 => "animation/skeletons/weapons/knife_butterfly.vnmskel",
        516 => "animation/skeletons/weapons/knife_push.vnmskel",
        517 => "animation/skeletons/weapons/knife_cord.vnmskel",
        518 => "animation/skeletons/weapons/knife_canis.vnmskel",
        519 => "animation/skeletons/weapons/knife_ursus.vnmskel",
        520 => "animation/skeletons/weapons/knife_navaja.vnmskel",
        521 => "animation/skeletons/weapons/knife_outdoor.vnmskel",
        522 => "animation/skeletons/weapons/knife_stiletto.vnmskel",
        523 => "animation/skeletons/weapons/knife_talon.vnmskel",
        525 => "animation/skeletons/weapons/knife_skeleton.vnmskel",
        526 => "animation/skeletons/weapons/knife_kukri.vnmskel",
        _ => return None,
    })
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
}

#[derive(Clone)]
struct AnimAsset {
    key: String,
    value: Vec<u8>,
}

struct RewritePlan {
    knives: HashMap<u32, KnifeRule>,
    gloves: HashMap<u32, GloveRule>,
    resources: Vec<AnimAsset>,
    selected_pawns: HashSet<u32>,
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
        if pawn.controllers.iter().any(|controller| controllers.contains(controller)) {
            pawns.insert(*pawn_handle);
        }
    }
    pawns
}

fn choose_team(pawn: &PawnDiscovery, plan: &PlayerPlan) -> Option<Team> {
    for team in [Team::T, Team::Ct] {
        if pawn.teams.contains(&team.number()) && plan.by_team.contains_key(&team) {
            return Some(team);
        }
    }
    if plan.by_team.len() == 1 {
        plan.by_team.keys().next().copied()
    } else {
        None
    }
}

fn build_rewrite_plan(
    plans: &HashMap<u64, PlayerPlan>,
    discovery: &Discovery,
) -> Result<RewritePlan> {
    let mut knives = HashMap::new();
    let mut gloves = HashMap::new();
    let mut selected_pawns = HashSet::new();
    let mut target_definitions = BTreeSet::new();

    for (steam, player_plan) in plans {
        let pawns = player_pawns(discovery, *steam);
        if pawns.is_empty() {
            bail!("SteamID {steam} was not linked to any CCSPlayerPawn in the demo");
        }
        for pawn_handle in pawns {
            let Some(pawn) = discovery.pawns.get(&pawn_handle) else {
                continue;
            };
            let Some(team) = choose_team(pawn, player_plan) else {
                continue;
            };
            let Some(team_plan) = player_plan.by_team.get(&team) else {
                continue;
            };
            selected_pawns.insert(pawn_handle);
            if let Some(cosmetic) = &team_plan.glove {
                gloves.insert(
                    pawn_handle,
                    GloveRule { steam: *steam, cosmetic: cosmetic.clone() },
                );
            }
            if let Some(cosmetic) = &team_plan.knife {
                let variant = discovery
                    .variants
                    .get(&cosmetic.definition)
                    .copied()
                    .or_else(|| fallback_variant(cosmetic.definition))
                    .ok_or_else(|| anyhow!(
                        "knife definition {} has no model/subclass calibration in this demo",
                        cosmetic.definition
                    ))?;
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
    }
    if knives.is_empty() && gloves.is_empty() {
        bail!("no owned knife or pawn glove targets were resolved from the demo");
    }

    let mut resources = Vec::new();
    for definition in target_definitions {
        let key = skeleton_for(definition)
            .ok_or_else(|| anyhow!("knife definition {definition} has no skeleton mapping"))?;
        let value = discovery.anim_assets.get(key).cloned().ok_or_else(|| {
            anyhow!("animation resource {key} is not present in the source demo")
        })?;
        resources.push(AnimAsset { key: key.to_string(), value });
    }
    Ok(RewritePlan { knives, gloves, resources, selected_pawns })
}

fn push_attribute(fields: &mut Vec<(String, FieldValue)>, prefix: &str, slot: usize, definition: u16, value: f32) {
    let base = format!("{prefix}.m_NetworkedDynamicAttributes.m_Attributes.{slot:04}");
    fields.push((format!("{base}.m_iAttributeDefinitionIndex"), FieldValue::Unsigned16(definition)));
    fields.push((format!("{base}.m_iRawValue32"), FieldValue::Float(value)));
    fields.push((format!("{base}.m_flInitialValue"), FieldValue::Float(value)));
    fields.push((format!("{base}.m_nRefundableCurrency"), FieldValue::Signed32(0)));
    fields.push((format!("{base}.m_bSetBonus"), FieldValue::Boolean(false)));
}

fn knife_fields(rule: &KnifeRule) -> Vec<(String, FieldValue)> {
    let c = &rule.cosmetic;
    let account = rule.steam as u32;
    let (high, low) = fnv_identity(rule.steam, c.definition, 0x4B4E_4946_455F_4944);
    let prefix = "m_AttributeManager.m_Item";
    let mut fields = vec![
        ("CBodyComponent.m_skeletonInstance.m_modelState.m_hModel".into(), FieldValue::Unsigned64(rule.variant.model)),
        ("m_nSubclassID".into(), FieldValue::Unsigned32(rule.variant.subclass)),
        (format!("{prefix}.m_iItemDefinitionIndex"), FieldValue::Unsigned16(c.definition)),
        (format!("{prefix}.m_iEntityQuality"), FieldValue::Signed32(3)),
        (format!("{prefix}.m_iItemIDHigh"), FieldValue::Unsigned32(high)),
        (format!("{prefix}.m_iItemIDLow"), FieldValue::Unsigned32(low)),
        (format!("{prefix}.m_iAccountID"), FieldValue::Unsigned32(account)),
        (format!("{prefix}.m_NetworkedDynamicAttributes.m_Attributes"), FieldValue::Unsigned32(3)),
        (format!("{prefix}.m_szCustomName"), FieldValue::String("nSkinzCS2".into())),
        ("m_nFallbackPaintKit".into(), FieldValue::Signed32(c.paint_kit)),
        ("m_nFallbackSeed".into(), FieldValue::Signed32(c.seed as i32)),
        ("m_flFallbackWear".into(), FieldValue::Float(c.wear)),
    ];
    push_attribute(&mut fields, prefix, 0, 6, c.paint_kit as f32);
    push_attribute(&mut fields, prefix, 1, 7, c.seed);
    push_attribute(&mut fields, prefix, 2, 8, c.wear);
    fields
}

fn glove_fields(rule: &GloveRule) -> Vec<(String, FieldValue)> {
    let c = &rule.cosmetic;
    let account = rule.steam as u32;
    let (high, low) = fnv_identity(rule.steam, c.definition, 0x474C_4F56_455F_4944);
    let prefix = "m_EconGloves";
    let mut fields = vec![
        (format!("{prefix}.m_iItemDefinitionIndex"), FieldValue::Unsigned16(c.definition)),
        (format!("{prefix}.m_iEntityQuality"), FieldValue::Signed32(3)),
        (format!("{prefix}.m_iEntityLevel"), FieldValue::Unsigned32(1)),
        (format!("{prefix}.m_iItemIDHigh"), FieldValue::Unsigned32(high)),
        (format!("{prefix}.m_iItemIDLow"), FieldValue::Unsigned32(low)),
        (format!("{prefix}.m_iAccountID"), FieldValue::Unsigned32(account)),
        (format!("{prefix}.m_iInventoryPosition"), FieldValue::Unsigned32(48)),
        (format!("{prefix}.m_bInitialized"), FieldValue::Boolean(true)),
        (format!("{prefix}.m_NetworkedDynamicAttributes.m_Attributes"), FieldValue::Unsigned32(3)),
        (format!("{prefix}.m_szCustomName"), FieldValue::String("nSkinzCS2".into())),
        ("m_nEconGlovesChanged".into(), FieldValue::Unsigned8(1)),
    ];
    push_attribute(&mut fields, prefix, 0, 6, c.paint_kit as f32);
    push_attribute(&mut fields, prefix, 1, 7, c.seed);
    push_attribute(&mut fields, prefix, 2, 8, c.wear);
    fields
}

struct CosmeticRewriter {
    plan: RewritePlan,
}

impl DemoRewriter for CosmeticRewriter {
    fn interests(&self) -> RewriteInterests {
        RewriteInterests::ENTITY_FIELDS
            | RewriteInterests::STRING_TABLE_ENTRIES
            | RewriteInterests::DEMO_STRING_TABLES
            | RewriteInterests::SVC_CREATE_STRING_TABLE
    }

    fn should_track_entity(&mut self, _ctx: &Context, _event: EntityEvents, entity: &Entity) -> bool {
        self.plan.knives.contains_key(&entity.handle()) || self.plan.gloves.contains_key(&entity.handle())
    }

    fn should_rewrite_entity(&mut self, _ctx: &Context, _event: EntityEvents, entity: &Entity) -> bool {
        self.plan.knives.contains_key(&entity.handle()) || self.plan.gloves.contains_key(&entity.handle())
    }

    fn additional_entity_fields(
        &mut self,
        _ctx: &Context,
        event: EntityEvents,
        entity: &Entity,
    ) -> Result<Vec<(String, FieldValue)>, source2_demo::error::ParserError> {
        if event != EntityEvents::Created {
            return Ok(Vec::new());
        }
        if let Some(rule) = self.plan.knives.get(&entity.handle()) {
            return Ok(knife_fields(rule));
        }
        if let Some(rule) = self.plan.gloves.get(&entity.handle()) {
            return Ok(glove_fields(rule));
        }
        Ok(Vec::new())
    }

    fn rewrite_string_table_entry(
        &mut self,
        _ctx: &Context,
        _tick: u32,
        table_name: &str,
        entry: &mut StringTableEntryUpdate,
    ) -> Result<(), source2_demo::error::ParserError> {
        if table_name.eq_ignore_ascii_case("AnimAssetData") && entry.index() >= 2 {
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
        for (offset, resource) in self.plan.resources.iter().enumerate() {
            insert_create_string_table_entry(
                message,
                2 + offset as i32,
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
            for (offset, resource) in self.plan.resources.iter().enumerate() {
                table.items.insert(
                    2 + offset,
                    ItemsT { str: Some(resource.key.clone()), data: Some(resource.value.clone()) },
                );
            }
            changed |= !self.plan.resources.is_empty();
        }
        Ok(if changed { MessageRewrite::Rewrite } else { MessageRewrite::Keep })
    }
}

fn discover(input: &Path) -> Result<Discovery> {
    let file = File::open(input).with_context(|| format!("open source demo {}", input.display()))?;
    let mut parser = Parser::from_reader(BufReader::new(file))?;
    let state = parser.register_observer::<Discovery>();
    parser.run_to_end()?;
    let mut state = state.borrow_mut();
    Ok(std::mem::take(&mut *state))
}

fn rewrite(request: &Request) -> Result<Response> {
    if request.schema_version != 1 {
        bail!("unsupported request schema {}", request.schema_version);
    }
    if request.input_dem == request.output_dem {
        bail!("input and output demo paths must differ");
    }
    let plans = parse_plans(request)?;
    let discovery = discover(&request.input_dem)?;
    let plan = build_rewrite_plan(&plans, &discovery)?;
    let counts = (
        plan.selected_pawns.len(),
        plan.knives.len(),
        plan.gloves.len(),
        plan.resources.len(),
    );
    if let Some(parent) = request.output_dem.parent() {
        std::fs::create_dir_all(parent)?;
    }
    let input = File::open(&request.input_dem)?;
    let output = File::create(&request.output_dem)?;
    let mut writer = DemoWriter::from_reader(BufReader::new(input), BufWriter::new(output))?;
    writer.add_rewriter(CosmeticRewriter { plan });
    writer.run()?;
    Ok(Response {
        ok: true,
        error: None,
        pawn_count: counts.0,
        knife_count: counts.1,
        glove_count: counts.2,
        animation_resource_count: counts.3,
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
    Ok((request.context("missing --request")?, response.context("missing --response")?))
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
                pawn_count: 0,
                knife_count: 0,
                glove_count: 0,
                animation_resource_count: 0,
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
