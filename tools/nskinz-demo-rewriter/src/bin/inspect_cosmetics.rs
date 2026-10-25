use anyhow::{Context as _, Result};
use serde::Serialize;
use source2_demo::prelude::*;
use std::collections::{BTreeMap, HashMap};
use std::fs::File;
use std::io::BufReader;
use std::path::PathBuf;

#[derive(Clone, Debug, Serialize)]
struct Snapshot {
    tick: u32,
    event: String,
    class: String,
    handle: u32,
    fields: BTreeMap<String, String>,
}

#[derive(Default)]
struct CosmeticInspector {
    last_signature_by_handle: HashMap<u32, String>,
    snapshots: Vec<Snapshot>,
}

fn insert_field(fields: &mut BTreeMap<String, String>, entity: &Entity, name: String) {
    if let Some(value) = entity.try_get_property(&name) {
        fields.insert(name, format!("{value:?}"));
    }
}

fn collect_fields(class: &str, event: EntityEvents, entity: &Entity) -> BTreeMap<String, String> {
    let mut fields = BTreeMap::new();
    for name in [
        "m_iTeamNum",
        "m_hController",
        "m_hOriginalController",
        "m_bNeedToReApplyGloves",
        "m_nEconGlovesChanged",
        "CBodyComponent.m_skeletonInstance.m_modelState.m_nBodyGroupChoices.0000",
        "CBodyComponent.m_skeletonInstance.m_modelState.m_nBodyGroupChoices.0001",
        "m_hOwnerEntity",
        "m_hOwner",
    ] {
        insert_field(&mut fields, entity, name.to_string());
    }

    let prefix = if class == "CCSPlayerPawn" {
        "m_EconGloves"
    } else {
        "m_AttributeManager.m_Item"
    };
    for suffix in [
        "m_iItemDefinitionIndex",
        "m_iEntityQuality",
        "m_iEntityLevel",
        "m_iItemIDHigh",
        "m_iItemIDLow",
        "m_iAccountID",
        "m_iInventoryPosition",
        "m_bInitialized",
        "m_szCustomName",
        "m_NetworkedDynamicAttributes.m_Attributes",
    ] {
        insert_field(&mut fields, entity, format!("{prefix}.{suffix}"));
    }
    let attribute_count = entity
        .try_get_property(&format!(
            "{prefix}.m_NetworkedDynamicAttributes.m_Attributes"
        ))
        .and_then(|value| match value {
            FieldValue::Unsigned32(count) => Some(*count as usize),
            _ => None,
        })
        .unwrap_or_default();
    for slot in 0..attribute_count.min(256) {
        for suffix in [
            "m_iAttributeDefinitionIndex",
            "m_iRawValue32",
            "m_flInitialValue",
        ] {
            insert_field(
                &mut fields,
                entity,
                format!("{prefix}.m_NetworkedDynamicAttributes.m_Attributes.{slot:04}.{suffix}"),
            );
        }
    }
    if class == "CCSPlayerPawn" {
        for slot in 0..8 {
            insert_field(
                &mut fields,
                entity,
                format!("m_pWeaponServices.m_hMyWearables.{slot:04}"),
            );
        }
    }
    if event == EntityEvents::Created {
        for field in entity.fields().into_iter().filter(|field| {
            field.name.contains("EconGloves")
                || field.name.contains("NeedToReApplyGloves")
                || field.name.contains("EconGlovesChanged")
                || field.name.contains("BodyGroupChoices")
                || field.name.contains("MyWearables")
        }) {
            if let Some(value) = field.value {
                fields.insert(field.name, format!("{value:?}"));
            }
        }
    }
    fields
}

impl Observer for CosmeticInspector {
    fn interests(&self) -> Interests {
        Interests::ENTITY_STATE | Interests::ENTITY_EVENTS | Interests::REPLAY_END
    }

    fn on_entity(&mut self, ctx: &Context, event: EntityEvents, entity: &Entity) -> ObserverResult {
        let class = entity.class().name();
        if class != "CCSPlayerPawn" && !class.contains("Wearable") && !class.contains("Glove") {
            return Ok(());
        }

        let fields = collect_fields(class, event, entity);
        let signature = format!("{fields:?}");
        let handle = entity.handle();
        if event != EntityEvents::Deleted
            && self.last_signature_by_handle.get(&handle) == Some(&signature)
        {
            return Ok(());
        }
        if event == EntityEvents::Deleted {
            self.last_signature_by_handle.remove(&handle);
        } else {
            self.last_signature_by_handle.insert(handle, signature);
        }
        self.snapshots.push(Snapshot {
            tick: ctx.tick(),
            event: format!("{event:?}"),
            class: class.to_string(),
            handle,
            fields,
        });
        Ok(())
    }
}

fn main() -> Result<()> {
    let mut args = std::env::args_os().skip(1);
    let input = args
        .next()
        .map(PathBuf::from)
        .context("usage: inspect_cosmetics <input.dem> <output.json>")?;
    let output = args
        .next()
        .map(PathBuf::from)
        .context("usage: inspect_cosmetics <input.dem> <output.json>")?;
    let seek_ticks = args
        .map(|value| value.to_string_lossy().parse::<u32>())
        .collect::<std::result::Result<Vec<_>, _>>()?;

    let file = File::open(&input).with_context(|| format!("open {}", input.display()))?;
    let mut parser = Parser::from_reader(BufReader::with_capacity(8 * 1024 * 1024, file))?;
    let state = parser.register_observer::<CosmeticInspector>();
    if !seek_ticks.is_empty() {
        let mut snapshots = Vec::new();
        for tick in seek_ticks {
            parser.jump_to_tick(tick)?;
            for entity in parser.context().entities().iter() {
                if entity.class().name() == "CCSPlayerPawn" {
                    snapshots.push(Snapshot {
                        tick,
                        event: "Seek".into(),
                        class: entity.class().name().into(),
                        handle: entity.handle(),
                        fields: collect_fields(
                            entity.class().name(),
                            EntityEvents::Created,
                            entity,
                        ),
                    });
                }
            }
        }
        std::fs::write(&output, serde_json::to_vec_pretty(&snapshots)?)?;
        eprintln!("{} seek snapshots -> {}", snapshots.len(), output.display());
        return Ok(());
    }
    parser.run_to_end()?;
    let state = state.borrow();
    std::fs::write(&output, serde_json::to_vec_pretty(&state.snapshots)?)?;
    eprintln!(
        "{} snapshots -> {}",
        state.snapshots.len(),
        output.display()
    );
    Ok(())
}
