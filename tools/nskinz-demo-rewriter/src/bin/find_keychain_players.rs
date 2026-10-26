//! Locate living players holding a weapon with recorded keychain attributes.
use anyhow::{Context as _, Result};
use serde::Serialize;
use source2_demo::prelude::*;
use std::collections::BTreeMap;
use std::fs::{File, OpenOptions};
use std::io::{BufReader, Write};
use std::path::PathBuf;

#[derive(Serialize)]
struct Sighting {
    player: String,
    xuid: u64,
    weapon_definition: u32,
    keychain_id: u32,
    first_tick: u32,
    last_tick: u32,
    sample_count: usize,
    example_ticks: Vec<u32>,
}

#[derive(Default)]
struct Finder {
    sightings: BTreeMap<(u64, u32, u32), Sighting>,
}

fn integer(entity: &Entity, name: &str) -> Option<u64> {
    entity.try_get_property(name).and_then(|v| v.try_into().ok())
}

fn keychain_id(entity: &Entity) -> Option<u32> {
    let prefix = "m_AttributeManager.m_Item.m_NetworkedDynamicAttributes.m_Attributes";
    let count = integer(entity, prefix).unwrap_or_default().min(4096);
    for slot in 0..count {
        let base = format!("{prefix}.{slot:04}");
        if integer(entity, &format!("{base}.m_iAttributeDefinitionIndex")) != Some(299) {
            continue;
        }
        let bits = match entity.try_get_property(&format!("{base}.m_iRawValue32"))? {
            FieldValue::Float(value) => value.to_bits(),
            value => {
                let integer: u64 = value.try_into().ok()?;
                integer.try_into().ok()?
            }
        };
        return (bits > 0).then_some(bits);
    }
    None
}

impl Observer for Finder {
    fn interests(&self) -> Interests {
        Interests::ENTITY_STATE | Interests::TICK_END
    }

    fn on_tick_end(&mut self, ctx: &Context) -> ObserverResult {
        // One authoritative sample per second is enough to locate a test view.
        if ctx.tick() % 64 != 0 {
            return Ok(());
        }
        for pawn in ctx.entities().iter().filter(|e| e.class().name() == "CCSPlayerPawn") {
            if integer(pawn, "m_iHealth").unwrap_or_default() == 0 {
                continue;
            }
            let Some(weapon) = integer(pawn, "m_pWeaponServices.m_hActiveWeapon")
                .and_then(|h| ctx.entities().get_by_handle(h as usize).ok())
            else { continue; };
            let Some(kit) = keychain_id(weapon) else { continue; };
            let Some(controller) = ["m_hController", "m_hOriginalController"].iter()
                .filter_map(|name| integer(pawn, name))
                .find_map(|h| ctx.entities().get_by_handle(h as usize).ok())
            else { continue; };
            let Some(xuid) = integer(controller, "m_steamID").filter(|v| *v > 0) else { continue; };
            let definition = integer(weapon, "m_AttributeManager.m_Item.m_iItemDefinitionIndex")
                .unwrap_or_default() as u32;
            let name = ["m_iszPlayerName", "m_sSanitizedPlayerName"].iter()
                .find_map(|field| match controller.try_get_property(field) {
                    Some(FieldValue::String(name)) => Some(name.clone()),
                    _ => None,
                }).unwrap_or_default();
            let sighting = self.sightings.entry((xuid, definition, kit)).or_insert(Sighting {
                player: name, xuid, weapon_definition: definition, keychain_id: kit,
                first_tick: ctx.tick(), last_tick: ctx.tick(), sample_count: 0, example_ticks: Vec::new(),
            });
            sighting.last_tick = ctx.tick();
            sighting.sample_count += 1;
            if ctx.tick() >= 5000 && sighting.example_ticks.len() < 12 {
                sighting.example_ticks.push(ctx.tick());
            }
        }
        Ok(())
    }
}

fn main() -> Result<()> {
    let args: Vec<_> = std::env::args_os().skip(1).map(PathBuf::from).collect();
    anyhow::ensure!(args.len() == 2, "usage: find_keychain_players <source.dem> <new-report.json>");
    let mut output = OpenOptions::new().write(true).create_new(true).open(&args[1])?;
    let file = File::open(&args[0]).with_context(|| format!("open {}", args[0].display()))?;
    let mut parser = Parser::from_reader(BufReader::with_capacity(8 * 1024 * 1024, file))?;
    let state = parser.register_observer::<Finder>();
    parser.run_to_end()?;
    let state = state.borrow();
    let report = serde_json::json!({
        "source_demo": args[0],
        "sample_interval_ticks": 64,
        "players_holding_keychain_weapons": state.sightings.values().collect::<Vec<_>>(),
    });
    output.write_all(&serde_json::to_vec_pretty(&report)?)?;
    eprintln!("{} player/weapon/keychain combinations -> {}", state.sightings.len(), args[1].display());
    Ok(())
}
