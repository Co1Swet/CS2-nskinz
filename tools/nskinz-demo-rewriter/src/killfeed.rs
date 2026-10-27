//! Persist per-attacker knife identities in death events, without a live HUD hook.
use super::*;
use source2_demo::proto::{
    CMsgSource1LegacyGameEvent, CMsgSource1LegacyGameEventList, EBaseGameEvents, Message,
};

type Event = CMsgSource1LegacyGameEvent;

pub fn knife_token(definition: u16) -> Option<&'static str> {
    match definition {
        42 => Some("knife"),
        59 => Some("knife_t"),
        500 => Some("bayonet"),
        508 => Some("knife_m9_bayonet"),
        514 => Some("knife_survival_bowie"),
        520 => Some("knife_gypsy_jackknife"),
        523 => Some("knife_widowmaker"),
        _ => knife_asset(definition)?
            .model
            .rsplit('/')
            .next()?
            .strip_prefix("weapon_")?
            .strip_suffix(".vmdl"),
    }
}

fn is_knife(token: &str) -> bool {
    let token = token.strip_prefix("weapon_").unwrap_or(token);
    matches!(token, "knife_default_ct" | "knife_default_t")
        || KNIFE_ASSETS
            .iter()
            .any(|asset| knife_token(asset.definition) == Some(token))
}

#[derive(Clone)]
struct Definition {
    name: String,
    keys: HashMap<String, (usize, i32)>,
}

impl Definition {
    fn key<'a>(
        &self,
        event: &'a Event,
        name: &str,
    ) -> Option<&'a source2_demo::proto::c_msg_source1_legacy_game_event::KeyT> {
        event.keys.get(self.keys.get(name)?.0)
    }

    fn string<'a>(&self, event: &'a Event, name: &str) -> Option<&'a str> {
        let key = self.key(event, name)?;
        (key.r#type() == 1).then(|| key.val_string())
    }
}

#[derive(Default)]
struct Schema {
    definitions: HashMap<i32, Definition>,
}

impl Schema {
    fn death(&mut self, msg_type: i32, payload: &[u8]) -> Result<Option<(Definition, Event)>> {
        if msg_type == EBaseGameEvents::GeSource1LegacyGameEventList as i32 {
            let message = CMsgSource1LegacyGameEventList::decode(payload)?;
            self.definitions = message
                .descriptors
                .into_iter()
                .map(|descriptor| {
                    let keys = descriptor
                        .keys
                        .iter()
                        .enumerate()
                        .map(|(index, key)| (key.name().to_owned(), (index, key.r#type())))
                        .collect();
                    (
                        descriptor.eventid(),
                        Definition {
                            name: descriptor.name().into(),
                            keys,
                        },
                    )
                })
                .collect();
            return Ok(None);
        }
        if msg_type != EBaseGameEvents::GeSource1LegacyGameEvent as i32 {
            return Ok(None);
        }
        let event = Event::decode(payload)?;
        let Some(definition) = self.definitions.get(&event.eventid()) else {
            anyhow::ensure!(
                event.event_name() != "player_death",
                "death event has no descriptor"
            );
            return Ok(None);
        };
        if definition.name != "player_death" {
            return Ok(None);
        }
        anyhow::ensure!(
            event.keys.len() == definition.keys.len(),
            "death event key count differs from its descriptor"
        );
        for (index, kind) in definition.keys.values() {
            anyhow::ensure!(
                event.keys[*index].r#type() == *kind,
                "death event key type differs from its descriptor"
            );
        }
        Ok(Some((definition.clone(), event)))
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
struct Actor {
    steam: u64,
    team: Team,
}

fn controller_actor(controller: &Entity) -> Option<Actor> {
    if !controller.class().name().contains("PlayerController") {
        return None;
    }
    // Bots have SteamID 0. They are resolved actors without any configurable
    // player plan, rather than a reason to reject another player's demo.
    let steam = field_integer(controller, "m_steamID")?;
    let team = entity_team(controller)?;
    Some(Actor { steam, team })
}

fn attacker(ctx: &Context, definition: &Definition, event: &Event) -> Result<Option<Actor>> {
    // CS2 type 9 identifies a zero-based player slot. Type 3 legacy user IDs
    // must not be interpreted using this rule. The real fixtures include slot 0.
    let slot = definition
        .key(event, "attacker")
        .filter(|key| key.r#type() == 9)
        .map(|key| key.val_short())
        .filter(|slot| (0..65535).contains(slot));
    let by_slot = slot
        .and_then(|slot| ctx.entities().get_by_index(slot as usize + 1).ok())
        .and_then(controller_actor);

    // Event Pawn handles can retain serial bits absent from the networked
    // handle representation. Resolve its index, then validate the reverse
    // controller -> Pawn relationship at this exact packet position.
    let by_pawn = definition
        .key(event, "attacker_pawn")
        .filter(|key| key.r#type() == 8)
        .map(|key| key.val_long() as u32)
        .filter(|handle| *handle != u32::MAX)
        .and_then(|handle| ctx.entities().get_by_index((handle & 0x3fff) as usize).ok())
        .filter(|pawn| pawn.class().name() == "CCSPlayerPawn")
        .and_then(|pawn| {
            ["m_hController", "m_hOriginalController"]
                .into_iter()
                .find_map(|field| {
                    let handle = field_integer(pawn, field)? as u32;
                    let controller = ctx
                        .entities()
                        .get_by_index((handle & 0x3fff) as usize)
                        .ok()?;
                    if controller.handle() != handle {
                        return None;
                    }
                    let actor = controller_actor(controller)?;
                    let owns_pawn = ["m_hPlayerPawn", "m_hPawn"]
                        .into_iter()
                        .filter_map(|field| field_integer(controller, field))
                        .any(|handle| handle as u32 == pawn.handle());
                    owns_pawn.then_some(Actor {
                        team: entity_team(pawn).unwrap_or(actor.team),
                        ..actor
                    })
                })
        });
    if let (Some(slot), Some(pawn)) = (by_slot, by_pawn) {
        anyhow::ensure!(
            slot == pawn,
            "death attacker slot and Pawn disagree: {slot:?} versus {pawn:?}"
        );
    }
    Ok(by_pawn.or(by_slot))
}

#[derive(Clone)]
struct Target {
    token: &'static str,
    item_id: String,
    faux_id: String,
}

fn target(steam: u64, cosmetic: &Cosmetic) -> Result<Target> {
    let token = knife_token(cosmetic.definition).context("knife has no death-event token")?;
    let (high, low) = fnv_identity(steam, cosmetic.definition, 0x4B4E_4946_455F_4944);
    let item_id = (u64::from(high) << 32 | u64::from(low)).to_string();
    // Verified against real default and painted knife death records: F-prefix,
    // paint in the next 32 bits, definition in the low 16 bits.
    let faux_id = (0xF000_0000_0000_0000u64
        | ((cosmetic.paint_kit.max(0) as u64) << 16)
        | u64::from(cosmetic.definition))
    .to_string();
    Ok(Target {
        token,
        item_id,
        faux_id,
    })
}

// Edit only the requested nested protobuf value bytes. Prost round-tripping
// an entire event would discard unknown fields from newer clients.
struct Span {
    field: u32,
    wire: u8,
    start: usize,
    body: usize,
    end: usize,
}

fn varint(bytes: &[u8], offset: &mut usize) -> Result<u64> {
    let mut result = 0u64;
    for shift in (0..70).step_by(7) {
        let byte = *bytes.get(*offset).context("truncated protobuf varint")?;
        *offset += 1;
        anyhow::ensure!(shift != 63 || byte <= 1, "protobuf varint overflow");
        result |= u64::from(byte & 127) << shift;
        if byte & 128 == 0 {
            return Ok(result);
        }
    }
    bail!("protobuf varint overflow")
}

fn put_varint(mut value: u64, output: &mut Vec<u8>) {
    while value >= 128 {
        output.push(value as u8 | 128);
        value >>= 7;
    }
    output.push(value as u8);
}

fn spans(bytes: &[u8]) -> Result<Vec<Span>> {
    let mut offset = 0usize;
    let mut fields = Vec::new();
    while offset < bytes.len() {
        let start = offset;
        let tag = varint(bytes, &mut offset)?;
        let field = u32::try_from(tag >> 3)?;
        anyhow::ensure!(field > 0, "invalid protobuf field number");
        let wire = (tag & 7) as u8;
        let mut body = offset;
        match wire {
            0 => {
                varint(bytes, &mut offset)?;
            }
            1 => offset = offset.checked_add(8).context("protobuf length overflow")?,
            2 => {
                let length = usize::try_from(varint(bytes, &mut offset)?)?;
                body = offset;
                offset = offset
                    .checked_add(length)
                    .context("protobuf length overflow")?;
            }
            5 => offset = offset.checked_add(4).context("protobuf length overflow")?,
            _ => bail!("unsupported protobuf wire type {wire}"),
        }
        anyhow::ensure!(offset <= bytes.len(), "truncated protobuf field");
        fields.push(Span {
            field,
            wire,
            start,
            body,
            end: offset,
        });
    }
    Ok(fields)
}

fn encode_value(field: u32, wire: u8, value: &[u8], output: &mut Vec<u8>) {
    put_varint(u64::from(field) << 3 | u64::from(wire), output);
    if wire == 2 {
        put_varint(value.len() as u64, output);
    }
    output.extend_from_slice(value);
}

fn replace_values(
    payload: &[u8],
    replacements: &HashMap<usize, (u32, u8, Vec<u8>)>,
) -> Result<Vec<u8>> {
    let mut result = Vec::with_capacity(payload.len());
    let mut key_index = 0usize;
    for span in spans(payload)? {
        if span.field == 3 && span.wire == 2 {
            if let Some((value_field, wire, value)) = replacements.get(&key_index) {
                let mut key = Vec::new();
                let original = &payload[span.body..span.end];
                let mut replaced = false;
                for value_span in spans(original)? {
                    if value_span.field == *value_field {
                        anyhow::ensure!(
                            value_span.wire == *wire,
                            "unexpected death key value wire type"
                        );
                        encode_value(*value_field, *wire, value, &mut key);
                        replaced = true;
                    } else {
                        key.extend_from_slice(&original[value_span.start..value_span.end]);
                    }
                }
                if !replaced {
                    encode_value(*value_field, *wire, value, &mut key);
                }
                encode_value(3, 2, &key, &mut result);
            } else {
                result.extend_from_slice(&payload[span.start..span.end]);
            }
            key_index += 1;
        } else {
            result.extend_from_slice(&payload[span.start..span.end]);
        }
    }
    Ok(result)
}

fn replace_death(
    payload: &[u8],
    definition: &Definition,
    event: &Event,
    target: &Target,
) -> Result<Vec<u8>> {
    let mut replacements = HashMap::new();
    for (name, desired) in [
        ("weapon", target.token),
        ("weapon_itemid", target.item_id.as_str()),
        ("weapon_fauxitemid", target.faux_id.as_str()),
    ] {
        let Some((index, kind)) = definition.keys.get(name) else {
            continue;
        };
        let (field, wire, bytes) = match kind {
            1 => (2, 2, desired.as_bytes().to_vec()),
            7 if name != "weapon" => {
                let mut bytes = Vec::new();
                put_varint(desired.parse()?, &mut bytes);
                (8, 0, bytes)
            }
            _ => bail!("unsupported death field type {kind} for {name}"),
        };
        anyhow::ensure!(
            event.keys.get(*index).is_some(),
            "missing death field {name}"
        );
        replacements.insert(*index, (field, wire, bytes));
    }
    replace_values(payload, &replacements)
}

#[derive(Default)]
pub struct Audit {
    pub expected: Vec<(u32, Vec<u8>)>,
    pub matched: usize,
    pub changed: usize,
}

pub struct KillfeedRewriter {
    schema: Schema,
    targets: HashMap<(u64, Team), Target>,
    pub audit: Audit,
}

impl KillfeedRewriter {
    pub fn new(plans: &HashMap<u64, PlayerPlan>) -> Result<Self> {
        let mut targets = HashMap::new();
        for (steam, player) in plans {
            for (team, plan) in &player.by_team {
                if let Some(cosmetic) = &plan.knife {
                    targets.insert((*steam, *team), target(*steam, cosmetic)?);
                }
            }
        }
        Ok(Self {
            schema: Schema::default(),
            targets,
            audit: Audit::default(),
        })
    }
    pub fn enabled(&self) -> bool {
        !self.targets.is_empty()
    }

    fn requested_target(
        &self,
        definition: &Definition,
        event: &Event,
        actor: Actor,
    ) -> Option<&Target> {
        if !definition.string(event, "weapon").is_some_and(is_knife) {
            return None;
        }
        self.targets
            .get(&(actor.steam, actor.team))
            .or_else(|| self.targets.get(&(actor.steam, Team::Any)))
    }
}

impl DemoRewriter for KillfeedRewriter {
    fn interests(&self) -> RewriteInterests {
        RewriteInterests::PACKET_MESSAGE
    }

    fn rewrite_packet_message(
        &mut self,
        ctx: &Context,
        tick: u32,
        msg_type: i32,
        payload: &[u8],
    ) -> std::result::Result<MessageRewrite, source2_demo::error::ParserError> {
        let result = (|| -> Result<_> {
            let Some((definition, event)) = self.schema.death(msg_type, payload)? else {
                return Ok(MessageRewrite::Keep);
            };
            let mut next = payload.to_vec();
            if definition.string(&event, "weapon").is_some_and(is_knife) {
                // An unresolved real attacker must not silently retain an old
                // knife icon while the rewrite reports complete success.
                let actor = attacker(ctx, &definition, &event)?;
                let slot = definition
                    .key(&event, "attacker")
                    .map(|key| key.val_short());
                anyhow::ensure!(
                    actor.is_some() || matches!(slot, None | Some(-1) | Some(65535)),
                    "cannot resolve knife-death attacker"
                );
                if let Some(actor) = actor {
                    if let Some(target) = self.requested_target(&definition, &event, actor) {
                        next = replace_death(payload, &definition, &event, target)?;
                        self.audit.matched += 1;
                    }
                }
            }
            let changed = next != payload;
            self.audit.expected.push((tick, next.clone()));
            if changed {
                self.audit.changed += 1;
                Ok(MessageRewrite::Replace(next))
            } else {
                Ok(MessageRewrite::Keep)
            }
        })();
        result.map_err(|error| {
            source2_demo::error::ParserError::IoError(format!(
                "knife killfeed rewrite failed at tick {tick}: {error:#}"
            ))
        })
    }
}

struct Verifier<'a> {
    schema: Schema,
    expected: &'a [(u32, Vec<u8>)],
    checked: usize,
}

impl DemoRewriter for Verifier<'_> {
    fn interests(&self) -> RewriteInterests {
        RewriteInterests::PACKET_MESSAGE
    }
    fn rewrite_packet_message(
        &mut self,
        _ctx: &Context,
        tick: u32,
        msg_type: i32,
        payload: &[u8],
    ) -> std::result::Result<MessageRewrite, source2_demo::error::ParserError> {
        let result = (|| -> Result<_> {
            if self.schema.death(msg_type, payload)?.is_some() {
                let expected = self
                    .expected
                    .get(self.checked)
                    .context("unexpected extra death event")?;
                anyhow::ensure!(
                    expected.0 == tick && expected.1 == payload,
                    "death record {} differs from the expected field-preserving result",
                    self.checked
                );
                self.checked += 1;
            }
            Ok(MessageRewrite::Keep)
        })();
        result.map_err(|error| {
            source2_demo::error::ParserError::IoError(format!(
                "knife killfeed verification failed at tick {tick}: {error:#}"
            ))
        })
    }
}

pub fn verify(output: &Path, audit: &Audit) -> Result<usize> {
    if audit.expected.is_empty() {
        return Ok(0);
    }
    let input = BufReader::with_capacity(IO_BUFFER_CAPACITY, File::open(output)?);
    let mut writer = DemoWriter::from_reader(input, VerificationSink::default())?;
    let state = writer.add_rewriter(Verifier {
        schema: Schema::default(),
        expected: &audit.expected,
        checked: 0,
    });
    writer.run()?;
    let checked = state.borrow().checked;
    anyhow::ensure!(
        checked == audit.expected.len(),
        "death event count changed: expected {}, verified {checked}",
        audit.expected.len()
    );
    Ok(checked)
}

#[cfg(test)]
mod tests {
    use super::*;
    use source2_demo::proto::{
        c_msg_source1_legacy_game_event::KeyT,
        c_msg_source1_legacy_game_event_list::{DescriptorT, KeyT as DescriptorKey},
    };
    use std::collections::BTreeMap;

    fn fixture() -> (Schema, Event) {
        // A different event ID and reordered keys guard against fixed IDs/slots.
        let names = [
            ("weapon_itemid", 1),
            ("headshot", 6),
            ("weapon", 1),
            ("attacker", 9),
            ("weapon_fauxitemid", 1),
            ("assister", 9),
            ("thrusmoke", 6),
        ];
        let list = CMsgSource1LegacyGameEventList {
            descriptors: vec![DescriptorT {
                eventid: Some(917),
                name: Some("player_death".into()),
                keys: names
                    .iter()
                    .map(|(name, kind)| DescriptorKey {
                        name: Some((*name).into()),
                        r#type: Some(*kind),
                    })
                    .collect(),
            }],
        };
        let mut schema = Schema::default();
        schema
            .death(
                EBaseGameEvents::GeSource1LegacyGameEventList as i32,
                &list.encode_to_vec(),
            )
            .unwrap();
        let mut keys: Vec<_> = names
            .iter()
            .map(|(_, kind)| KeyT {
                r#type: Some(*kind),
                ..Default::default()
            })
            .collect();
        keys[0].val_string = Some("52685124810".into());
        keys[1].val_bool = Some(false);
        keys[2].val_string = Some("knife_butterfly".into());
        keys[3].val_short = Some(0);
        keys[4].val_string = Some("17293822569105588739".into());
        keys[5].val_short = Some(8);
        keys[6].val_bool = Some(true);
        (
            schema,
            Event {
                eventid: Some(917),
                keys,
                server_tick: Some(1234),
                passthrough: Some(1),
                ..Default::default()
            },
        )
    }

    fn plans(steam: u64) -> HashMap<u64, PlayerPlan> {
        let request: Request = serde_json::from_value(serde_json::json!({
            "schema_version":2,"input_dem":"source.dem","output_dem":"output.dem","passes":[{"steam_id64":steam.to_string(),"items":[
                {"definition_index":59,"replacement_definition_index":507,"paint_kit":570,"pattern_seed":1.0,"wear":0.02,"team":"T"},
                {"definition_index":42,"replacement_definition_index":508,"paint_kit":568,"pattern_seed":1.0,"wear":0.02,"team":"CT"}
            ]}]
        })).unwrap();
        parse_plans(&request).unwrap()
    }

    #[test]
    fn knife_tokens_cover_exact_installed_weapon_names() {
        let expected = [
            (42, "knife"),
            (59, "knife_t"),
            (500, "bayonet"),
            (503, "knife_css"),
            (505, "knife_flip"),
            (506, "knife_gut"),
            (507, "knife_karambit"),
            (508, "knife_m9_bayonet"),
            (509, "knife_tactical"),
            (512, "knife_falchion"),
            (514, "knife_survival_bowie"),
            (515, "knife_butterfly"),
            (516, "knife_push"),
            (517, "knife_cord"),
            (518, "knife_canis"),
            (519, "knife_ursus"),
            (520, "knife_gypsy_jackknife"),
            (521, "knife_outdoor"),
            (522, "knife_stiletto"),
            (523, "knife_widowmaker"),
            (525, "knife_skeleton"),
            (526, "knife_kukri"),
        ];
        assert_eq!(expected.len(), KNIFE_ASSETS.len());
        for (definition, token) in expected {
            assert_eq!(knife_token(definition), Some(token));
            assert!(is_knife(token));
        }
        assert!(!is_knife("ak47"));
        assert!(!is_knife("knife_damage"));
        assert!(knife_token(524).is_none());
    }

    #[test]
    fn attacker_team_and_knife_scope_do_not_change_other_players_or_guns() {
        let steam = 76561198000000001;
        let rewriter = KillfeedRewriter::new(&plans(steam)).unwrap();
        let (schema, mut event) = fixture();
        let definition = &schema.definitions[&917];
        assert_eq!(
            rewriter
                .requested_target(
                    definition,
                    &event,
                    Actor {
                        steam,
                        team: Team::T
                    }
                )
                .unwrap()
                .token,
            "knife_karambit"
        );
        assert_eq!(
            rewriter
                .requested_target(
                    definition,
                    &event,
                    Actor {
                        steam,
                        team: Team::Ct
                    }
                )
                .unwrap()
                .token,
            "knife_m9_bayonet"
        );
        assert!(
            rewriter
                .requested_target(
                    definition,
                    &event,
                    Actor {
                        steam: steam + 1,
                        team: Team::T
                    }
                )
                .is_none()
        );
        assert!(
            rewriter
                .requested_target(
                    definition,
                    &event,
                    Actor {
                        steam: 0,
                        team: Team::T
                    }
                )
                .is_none()
        );
        event.keys[2].val_string = Some("ak47".into());
        assert!(
            rewriter
                .requested_target(
                    definition,
                    &event,
                    Actor {
                        steam,
                        team: Team::T
                    }
                )
                .is_none()
        );
        let empty = KillfeedRewriter::new(&HashMap::new()).unwrap();
        assert!(!empty.enabled());
    }

    #[test]
    fn event_patch_preserves_unknown_fields_assists_flags_and_envelope() {
        let steam = 76561198000000001;
        let rewriter = KillfeedRewriter::new(&plans(steam)).unwrap();
        let (schema, event) = fixture();
        let mut payload = event.encode_to_vec();
        let unknown = b"\xA2\x06\x06future"; // unknown length-delimited outer field
        payload.extend_from_slice(unknown);
        // Add an unknown field inside the weapon key as well.
        let nested_unknown = b"\xA0\x06\x7B";
        let mut with_nested = Vec::new();
        let mut index = 0;
        for span in spans(&payload).unwrap() {
            if span.field == 3 && span.wire == 2 {
                if index == 2 {
                    let mut body = payload[span.body..span.end].to_vec();
                    body.extend_from_slice(nested_unknown);
                    encode_value(3, 2, &body, &mut with_nested);
                } else {
                    with_nested.extend_from_slice(&payload[span.start..span.end]);
                }
                index += 1;
            } else {
                with_nested.extend_from_slice(&payload[span.start..span.end]);
            }
        }
        let target = rewriter
            .requested_target(
                &schema.definitions[&917],
                &event,
                Actor {
                    steam,
                    team: Team::T,
                },
            )
            .unwrap();
        let rewritten =
            replace_death(&with_nested, &schema.definitions[&917], &event, target).unwrap();
        assert!(rewritten.ends_with(unknown));
        assert!(
            rewritten
                .windows(nested_unknown.len())
                .any(|v| v == nested_unknown)
        );
        let actual = Event::decode(rewritten.as_slice()).unwrap();
        let mut expected = event.clone();
        expected.keys[0].val_string = Some(target.item_id.clone());
        expected.keys[2].val_string = Some("knife_karambit".into());
        expected.keys[4].val_string =
            Some((0xF000_0000_0000_0000u64 | 570 << 16 | 507).to_string());
        assert_eq!(actual, expected);
        let (high, low) = fnv_identity(steam, 507, 0x4B4E_4946_455F_4944);
        assert_eq!(
            actual.keys[0].val_string(),
            (u64::from(high) << 32 | u64::from(low)).to_string()
        );
    }

    #[test]
    fn descriptor_key_types_and_malformed_wire_fail_explicitly() {
        let (mut schema, mut event) = fixture();
        event.keys[2].r#type = Some(7);
        assert!(
            schema
                .death(
                    EBaseGameEvents::GeSource1LegacyGameEvent as i32,
                    &event.encode_to_vec()
                )
                .is_err()
        );
        assert!(spans(&[0x12, 0xff]).is_err());
        assert!(spans(&[0]).is_err());
    }

    #[derive(Clone)]
    struct Death {
        tick: u32,
        actor: Option<Actor>,
        weapon: String,
        fields: BTreeMap<String, String>,
    }
    #[derive(Default)]
    struct Deaths {
        rows: Vec<Death>,
    }
    impl Observer for Deaths {
        fn interests(&self) -> Interests {
            Interests::ENTITY_STATE | Interests::BASE_GAME_EVENT
        }
        fn on_game_event(&mut self, ctx: &Context, event: &GameEvent) -> ObserverResult {
            if event.name() != "player_death" {
                return Ok(());
            }
            let slot: i32 = event.get_value("attacker")?.try_into()?;
            let actor = (0..65535)
                .contains(&slot)
                .then(|| ctx.entities().get_by_index(slot as usize + 1).ok())
                .flatten()
                .and_then(controller_actor);
            let weapon = event.get_value("weapon")?.try_into()?;
            let fields = event
                .iter()
                .map(|(name, value)| (name.to_string(), format!("{value:?}")))
                .collect();
            self.rows.push(Death {
                tick: ctx.tick(),
                actor,
                weapon,
                fields,
            });
            Ok(())
        }
    }

    fn read_deaths(path: &Path) -> Vec<Death> {
        let mut parser = Parser::from_reader(BufReader::new(File::open(path).unwrap())).unwrap();
        let state = parser.register_observer::<Deaths>();
        parser.run_to_end().unwrap();
        let rows = std::mem::take(&mut state.borrow_mut().rows);
        rows
    }

    #[test]
    #[ignore = "requires NSKINZ_KILLFEED_MATRIX and NSKINZ_KILLFEED_OUTPUT real Demo fixtures"]
    fn real_knife_deaths_match_attacker_and_survive_seek_without_other_event_changes() {
        std::thread::Builder::new()
            .stack_size(16 * 1024 * 1024)
            .spawn(real_matrix)
            .unwrap()
            .join()
            .unwrap();
    }

    fn real_matrix() {
        #[derive(Deserialize)]
        struct Fixture {
            input_dem: PathBuf,
        }
        let fixtures: Vec<Fixture> = serde_json::from_slice(
            &std::fs::read(std::env::var_os("NSKINZ_KILLFEED_MATRIX").unwrap()).unwrap(),
        )
        .unwrap();
        let directory = PathBuf::from(std::env::var_os("NSKINZ_KILLFEED_OUTPUT").unwrap());
        std::fs::create_dir_all(&directory).unwrap();
        let game_dir = PathBuf::from(std::env::var_os("NSKINZ_CS2_GAME_DIR").unwrap());
        for (index, fixture) in fixtures.into_iter().enumerate() {
            let source = read_deaths(&fixture.input_dem);
            let mut counts: BTreeMap<u64, usize> = BTreeMap::new();
            for row in source.iter().filter(|row| is_knife(&row.weapon)) {
                if let Some(actor) = row.actor {
                    *counts.entry(actor.steam).or_default() += 1;
                }
            }
            assert!(counts.len() >= 3, "use a real knife-round recording");
            let mut owners: Vec<_> = counts.into_iter().collect();
            owners.sort_by_key(|(steam, count)| (std::cmp::Reverse(*count), *steam));
            let selected: HashSet<_> = owners
                .iter()
                .enumerate()
                .filter(|(i, _)| i % 2 == 0)
                .map(|(_, v)| v.0)
                .collect();
            let passes=selected.iter().map(|steam|serde_json::json!({"steam_id64":steam.to_string(),"items":[
                {"definition_index":59,"replacement_definition_index":507,"paint_kit":0,"pattern_seed":1.0,"wear":0.02,"team":"T"},
                {"definition_index":42,"replacement_definition_index":500,"paint_kit":0,"pattern_seed":1.0,"wear":0.02,"team":"CT"}
            ]})).collect::<Vec<_>>();
            let request:Request=serde_json::from_value(serde_json::json!({"schema_version":2,"input_dem":fixture.input_dem,"output_dem":directory.join(format!("fixture-{index}-fixed.dem")),"cs2_game_dir":game_dir,"passes":passes})).unwrap();
            let response = rewrite(&request).unwrap();
            let actual = read_deaths(&request.output_dem);
            assert_eq!(source.len(), actual.len());
            let mut matched = 0;
            let mut unconfigured_knives = 0;
            let mut matched_ticks = Vec::new();
            for (before, after) in source.iter().zip(&actual) {
                assert_eq!(before.tick, after.tick);
                assert_eq!(before.actor, after.actor);
                if is_knife(&before.weapon)
                    && before
                        .actor
                        .is_some_and(|actor| selected.contains(&actor.steam))
                {
                    let actor = before.actor.unwrap();
                    let definition = if actor.team == Team::T { 507 } else { 500 };
                    assert_eq!(after.weapon, knife_token(definition).unwrap());
                    assert_eq!(
                        after.fields["weapon_fauxitemid"],
                        format!(
                            "String(\"{}\")",
                            0xF000_0000_0000_0000u64 | u64::from(definition)
                        )
                    );
                    let (high, low) = fnv_identity(actor.steam, definition, 0x4B4E_4946_455F_4944);
                    assert_eq!(
                        after.fields["weapon_itemid"],
                        format!("String(\"{}\")", u64::from(high) << 32 | u64::from(low))
                    );
                    for (key, value) in &before.fields {
                        if !["weapon", "weapon_itemid", "weapon_fauxitemid"].contains(&key.as_str())
                        {
                            assert_eq!(after.fields.get(key), Some(value), "changed {key}");
                        }
                    }
                    matched += 1;
                    matched_ticks.push(before.tick);
                } else {
                    assert_eq!(before.fields, after.fields);
                    if is_knife(&before.weapon) {
                        unconfigured_knives += 1;
                    }
                }
            }
            assert!(matched >= 2 && unconfigured_knives > 0);
            assert_eq!(response.matched_knife_death_count, matched);
            assert_eq!(response.rewritten_knife_death_count, matched);
            assert_eq!(response.verified_death_event_count, actual.len());
            // Replay across each kill after forward and backward jumps. Compare
            // against independent linear parsing, including all unmodified keys.
            let mut parser =
                Parser::from_reader(BufReader::new(File::open(&request.output_dem).unwrap()))
                    .unwrap();
            let state = parser.register_observer::<Deaths>();
            for tick in matched_ticks.iter().rev().chain(matched_ticks.iter()) {
                parser.jump_to_tick(tick.saturating_sub(64)).unwrap();
                state.borrow_mut().rows.clear();
                parser.run_to_tick(tick + 1).unwrap();
                let rows = state.borrow();
                let replayed = rows
                    .rows
                    .iter()
                    .find(|row| row.tick == *tick)
                    .expect("seek lost target death");
                let expected = actual.iter().find(|row| row.tick == *tick).unwrap();
                assert_eq!(replayed.fields, expected.fields);
            }
            eprintln!(
                "knife killfeed fixture {index}: deaths={}, changed={matched}, unconfigured_knives={unconfigured_knives}, seek_checks={}",
                actual.len(),
                matched_ticks.len() * 2
            );
        }
    }
}
