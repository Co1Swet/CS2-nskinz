//! Read-only death-event and attacker-identity audit for real Demo fixtures.
use anyhow::{Context as _, Result};
use serde_json::{Value, json};
use source2_demo::prelude::*;
use std::collections::BTreeMap;
use std::fs::{File, OpenOptions};
use std::io::{BufReader, Write};
use std::path::PathBuf;

#[derive(Default)]
struct Inspector {
    deaths: Vec<Value>,
}

impl Observer for Inspector {
    fn interests(&self) -> Interests {
        Interests::ENTITY_STATE | Interests::BASE_GAME_EVENT
    }

    fn on_game_event(&mut self, ctx: &Context, event: &GameEvent) -> ObserverResult {
        if event.name() != "player_death" {
            return Ok(());
        }
        let fields: BTreeMap<_, _> = event
            .iter()
            .map(|(name, value)| (name.to_string(), format!("{value:?}")))
            .collect();
        let slot: i32 = event.get_value("attacker")?.try_into()?;
        let controller = (0..65535)
            .contains(&slot)
            .then(|| ctx.entities().get_by_index(slot as usize + 1).ok())
            .flatten();
        let pawn = controller
            .and_then(|entity| entity.try_get_property("m_hPlayerPawn"))
            .and_then(|value| {
                let handle: u64 = value.try_into().ok()?;
                ctx.entities().get_by_handle(handle as usize).ok()
            });
        let attacker_knives: Vec<_> = pawn
            .into_iter()
            .flat_map(|pawn| {
                (0..64).filter_map(move |slot| {
                    let name = format!("m_pWeaponServices.m_hMyWeapons.{slot:04}");
                    let handle: u64 = pawn.try_get_property(&name)?.try_into().ok()?;
                    let knife = ctx.entities().get_by_handle(handle as usize).ok()?;
                    if knife.class().name() != "CKnife" {
                        return None;
                    }
                    let fields: BTreeMap<_, _> = [
                        "m_AttributeManager.m_Item.m_iItemDefinitionIndex",
                        "m_AttributeManager.m_Item.m_iItemIDHigh",
                        "m_AttributeManager.m_Item.m_iItemIDLow",
                        "CBodyComponent.m_skeletonInstance.m_modelState.m_hModel",
                    ]
                    .into_iter()
                    .filter_map(|field| {
                        knife
                            .try_get_property(field)
                            .map(|value| (field, format!("{value:?}")))
                    })
                    .collect();
                    Some(json!({"handle":knife.handle(),"fields":fields}))
                })
            })
            .collect();
        let controllers: Vec<_> = ctx
            .entities()
            .iter()
            .filter(|entity| entity.class().name().contains("PlayerController"))
            .map(|entity| {
                let fields: BTreeMap<_, _> = [
                    "m_steamID",
                    "m_iszPlayerName",
                    "m_iTeamNum",
                    "m_iPlayerSlot",
                    "m_hPlayerPawn",
                    "m_hPawn",
                ]
                .into_iter()
                .filter_map(|field| {
                    entity
                        .try_get_property(field)
                        .map(|value| (field, format!("{value:?}")))
                })
                .collect();
                json!({"index": entity.index(), "handle": entity.handle(), "fields": fields})
            })
            .collect();
        self.deaths.push(json!({"tick": ctx.tick(), "event_id": event.id(), "fields": fields, "controllers": controllers,"attacker_knives":attacker_knives}));
        Ok(())
    }
}

fn main() -> Result<()> {
    let args: Vec<_> = std::env::args_os().skip(1).map(PathBuf::from).collect();
    anyhow::ensure!(
        args.len() == 2,
        "usage: inspect_deaths <source.dem> <new-report.json>"
    );
    let mut output = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&args[1])?;
    let file = File::open(&args[0]).with_context(|| format!("open {}", args[0].display()))?;
    let mut parser = Parser::from_reader(BufReader::with_capacity(8 * 1024 * 1024, file))?;
    let observer = parser.register_observer::<Inspector>();
    parser.run_to_end()?;
    let state = observer.borrow();
    let definitions: Vec<_> = parser.context().game_events().iter()
        .filter(|(_, definition)| definition.name() == "player_death")
        .map(|(id, definition)| json!({"id": id, "keys": definition.keys().map(|key| json!({"name":key.name(),"type":key.type_id(),"index":key.id()})).collect::<Vec<_>>()})).collect();
    output.write_all(&serde_json::to_vec_pretty(
        &json!({"source":args[0], "death_definitions":definitions,"deaths":state.deaths}),
    )?)?;
    eprintln!("{} deaths -> {}", state.deaths.len(), args[1].display());
    Ok(())
}
