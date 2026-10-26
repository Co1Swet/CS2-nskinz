//! Read-only audit of networked keychain classes and firearm Econ attributes.
//! Client schema classes alone do not prove that a class is serialized in a Demo.
use anyhow::{Context as _, Result};
use serde::Serialize;
use source2_demo::prelude::*;
use std::collections::BTreeMap;
use std::fs::{File, OpenOptions};
use std::io::{BufReader, Write};
use std::path::PathBuf;

const ITEM_PREFIX: &str = "m_AttributeManager.m_Item";
const KEYCHAIN_ATTRIBUTES: &[u32] = &[299, 300, 301, 302, 306, 314, 321];

#[derive(Serialize)]
struct AttributeSnapshot {
    raw_debug: String,
    raw_bits: Option<u32>,
}

#[derive(Serialize)]
struct ItemSnapshot {
    tick: u32,
    class: String,
    handle: u32,
    attributes: BTreeMap<u32, AttributeSnapshot>,
}

#[derive(Default)]
struct KeychainInspector {
    module_record_count: usize,
    weapon_created_count: usize,
    items: Vec<ItemSnapshot>,
}

fn unsigned_property(entity: &Entity, name: &str) -> Option<u32> {
    match entity.try_get_property(name)? {
        FieldValue::Unsigned8(value) => Some(u32::from(*value)),
        FieldValue::Unsigned16(value) => Some(u32::from(*value)),
        FieldValue::Unsigned32(value) => Some(*value),
        FieldValue::Unsigned64(value) => u32::try_from(*value).ok(),
        _ => None,
    }
}

impl Observer for KeychainInspector {
    fn interests(&self) -> Interests {
        Interests::ENTITY_STATE | Interests::ENTITY_EVENTS
    }

    fn on_entity(&mut self, ctx: &Context, event: EntityEvents, entity: &Entity) -> ObserverResult {
        let class = entity.class().name();
        if class.to_ascii_lowercase().contains("keychain") {
            self.module_record_count += 1;
        }
        if event != EntityEvents::Created
            || entity
                .try_get_property(&format!("{ITEM_PREFIX}.m_iItemDefinitionIndex"))
                .is_none()
        {
            return Ok(());
        }
        self.weapon_created_count += 1;
        let prefix = format!("{ITEM_PREFIX}.m_NetworkedDynamicAttributes.m_Attributes");
        let count = unsigned_property(entity, &prefix).unwrap_or_default();
        let mut attributes = BTreeMap::new();
        // Inspect the actual vector length. The cap only bounds diagnostic work
        // for an untrusted input; it is not a serialized vector length assumption.
        for slot in 0..count.min(4096) {
            let attribute = format!("{prefix}.{slot:04}");
            let Some(definition) =
                unsigned_property(entity, &format!("{attribute}.m_iAttributeDefinitionIndex"))
            else {
                continue;
            };
            if !KEYCHAIN_ATTRIBUTES.contains(&definition) {
                continue;
            }
            if let Some(raw) = entity.try_get_property(&format!("{attribute}.m_iRawValue32")) {
                let raw_bits = match raw {
                    FieldValue::Float(value) => Some(value.to_bits()),
                    FieldValue::Unsigned32(value) => Some(*value),
                    _ => None,
                };
                attributes.insert(
                    definition,
                    AttributeSnapshot {
                        raw_debug: format!("{raw:?}"),
                        raw_bits,
                    },
                );
            }
        }
        if !attributes.is_empty() {
            self.items.push(ItemSnapshot {
                tick: ctx.tick(),
                class: class.into(),
                handle: entity.handle(),
                attributes,
            });
        }
        Ok(())
    }
}

fn main() -> Result<()> {
    let args: Vec<_> = std::env::args_os().skip(1).map(PathBuf::from).collect();
    anyhow::ensure!(
        args.len() == 2,
        "usage: inspect_keychains <input.dem> <output.json>"
    );
    // Refuse to overwrite any existing file, including the source Demo or an
    // alias to it. Each run must have a new report path.
    let mut output = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&args[1])
        .with_context(|| format!("create new report {}", args[1].display()))?;
    let file = File::open(&args[0]).with_context(|| format!("open {}", args[0].display()))?;
    let mut parser = Parser::from_reader(BufReader::with_capacity(8 * 1024 * 1024, file))?;
    let state = parser.register_observer::<KeychainInspector>();
    parser.run_to_end()?;
    let classes: Vec<_> = parser
        .context()
        .classes()
        .iter()
        .map(|c| c.name())
        .collect();
    let module_classes: Vec<_> = classes
        .iter()
        .filter(|name| name.to_ascii_lowercase().contains("keychain"))
        .copied()
        .collect();
    let state = state.borrow();
    let report = serde_json::json!({
        "input_dem": args[0],
        "scan": "linear entity stream plus complete network class table; Created item attributes only",
        "network_class_count": classes.len(),
        "keychain_network_classes": module_classes,
        "keychain_module_record_count": state.module_record_count,
        "econ_entity_created_count": state.weapon_created_count,
        "keychain_attribute_created_count": state.items.len(),
        "keychain_items": state.items,
        "network_classes": classes,
    });
    output.write_all(&serde_json::to_vec_pretty(&report)?)?;
    eprintln!(
        "{} classes, {} keychain network classes, {} module records, {} Created items with keychain attributes -> {}",
        classes.len(), module_classes.len(), state.module_record_count, state.items.len(), args[1].display()
    );
    Ok(())
}
