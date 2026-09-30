use crate::entity::field::{Decode, Encode, FieldPath, FieldState, Serializer, Skip};
use crate::entity::{Entity, EntityEvents};
use crate::error::ParserError;
use crate::parser::demo::writer::{
    DecodedEntityField, DemoWriter, FieldReplacement, FieldReplacementValue,
};
use crate::proto::{CSvcMsgPacketEntities, Message};
use crate::reader::{BitsReader, FieldPathCodec, MessageReader, SliceReader};
use crate::stream::copy::{bit_position, copy_original_bits};
use crate::stream::field_path::FieldOp;
use crate::writer::{BitsWriter, BitstreamWriter};
use std::cmp::Ordering;
use std::io::{Seek, Write};

impl<'a, R, W> DemoWriter<'a, R, W>
where
    R: BitsReader + MessageReader,
    W: Write + Seek,
{
    pub(crate) fn rewrite_svc_packet_entities(
        &mut self,
        msg: &[u8],
    ) -> Result<Option<Vec<u8>>, ParserError> {
        let mut packet_entities = CSvcMsgPacketEntities::decode(msg)?;
        let Some(entity_data) = packet_entities.entity_data.as_deref() else {
            return Ok(None);
        };

        let (rewritten, changed) =
            self.rewrite_entity_data(entity_data, packet_entities.updated_entries())?;
        if changed {
            packet_entities.entity_data = Some(rewritten);
            packet_entities.serialized_entities = None;
            Ok(Some(packet_entities.encode_to_vec()))
        } else {
            Ok(None)
        }
    }

    fn rewrite_entity_data(
        &mut self,
        entity_data: &[u8],
        updated_entries: i32,
    ) -> Result<(Vec<u8>, bool), ParserError> {
        let mut reader = SliceReader::new(entity_data);
        self.entity_rewrite.replacements.clear();
        let mut index = usize::MAX;
        let path_reader = self.entity_rewrite.field_path_codec.clone();

        for _ in 0..updated_entries {
            let delta = reader.read_ubit_var();
            index = index.wrapping_add((delta + 1) as usize);

            let cmd = reader.read_bits(2);

            if cmd == 1 {
                continue;
            }

            match EntityEvents::from_cmd(cmd) {
                EntityEvents::Created => {
                    self.rewrite_entity_created(&mut reader, &path_reader, index)?;
                }
                EntityEvents::Updated => {
                    self.rewrite_entity_updated(&mut reader, &path_reader, index)?;
                }
                EntityEvents::Deleted => {
                    self.parser.context.entities.entities_vec[index].index = u32::MAX;
                }
            }
        }

        if self.entity_rewrite.replacements.is_empty() {
            return Ok((Vec::new(), false));
        }

        let mut out = Vec::with_capacity(entity_data.len());
        let mut writer = BitstreamWriter::new(&mut out);
        let mut copy_start = 0;
        let mut replacements = std::mem::take(&mut self.entity_rewrite.replacements);
        for replacement in replacements.drain(..) {
            copy_original_bits(
                entity_data,
                copy_start,
                replacement.value_start - copy_start,
                &mut writer,
            )?;
            match replacement.value {
                FieldReplacementValue::Field {
                    serializer,
                    fp,
                    value,
                } => serializer.get_decoder(&fp).encode(&mut writer, &value)?,
                FieldReplacementValue::EncodedBits { bytes, bit_len } => {
                    writer.write_bits_as_bytes(&bytes, bit_len as u32)?;
                }
            }
            copy_start = replacement.value_end;
        }
        self.entity_rewrite.replacements = replacements;
        copy_original_bits(
            entity_data,
            copy_start,
            entity_data.len() * 8 - copy_start,
            &mut writer,
        )?;
        writer.flush()?;
        drop(writer);
        Ok((out, true))
    }

    fn rewrite_entity_created(
        &mut self,
        reader: &mut SliceReader<'_>,
        path_reader: &FieldPathCodec,
        index: usize,
    ) -> Result<(), ParserError> {
        let class_id = reader.read_bits(self.parser.context.classes.class_id_size) as i32;

        let serial = reader.read_bits(17);

        let _ = reader.read_var_u32();

        let class = self
            .parser
            .context
            .classes
            .get_by_id_rc(class_id as usize)
            .clone();

        let state = self.entity_baseline_state(class_id, &class.serializer);
        let mut entity = Entity::new(index as u32, serial, class, state);

        let track = self.should_track_entity(EntityEvents::Created, &entity);
        let rewrite = self.should_rewrite_entity(EntityEvents::Created, &entity);
        if track || rewrite {
            self.rewrite_fields(
                reader,
                path_reader,
                EntityEvents::Created,
                &mut entity,
                track,
                rewrite,
            )?;
        } else {
            self.skip_original_fields(reader, path_reader, &entity);
        }
        if !track {
            entity.state = FieldState::default();
        }
        self.parser.context.entities.entities_vec[index] = entity;

        Ok(())
    }

    fn rewrite_entity_updated(
        &mut self,
        reader: &mut SliceReader<'_>,
        path_reader: &FieldPathCodec,
        index: usize,
    ) -> Result<(), ParserError> {
        let class = self.parser.context.entities.entities_vec[index]
            .class
            .clone();
        let placeholder = Entity {
            index: u32::MAX,
            serial: 0,
            class,
            state: FieldState::default(),
        };
        let mut entity = std::mem::replace(
            &mut self.parser.context.entities.entities_vec[index],
            placeholder,
        );
        let track = self.should_track_entity(EntityEvents::Updated, &entity);
        let rewrite = self.should_rewrite_entity(EntityEvents::Updated, &entity);
        if track || rewrite {
            self.rewrite_fields(
                reader,
                path_reader,
                EntityEvents::Updated,
                &mut entity,
                track,
                rewrite,
            )?;
        } else {
            self.skip_original_fields(reader, path_reader, &entity);
        }
        if !track {
            entity.state = FieldState::default();
        }
        self.parser.context.entities.entities_vec[index] = entity;

        Ok(())
    }

    fn skip_original_fields(
        &mut self,
        reader: &mut SliceReader<'_>,
        path_reader: &FieldPathCodec,
        entity: &Entity,
    ) {
        self.entity_rewrite.rewrite_paths.clear();
        let mut fp = FieldPath::default();

        loop {
            reader.refill();
            let op = path_reader.read_op(reader);
            if let FieldOp::FieldPathEncodeFinish = op {
                break;
            }
            op.execute(reader, &mut fp);
            self.entity_rewrite.rewrite_paths.push(fp);
        }

        for fp in &self.entity_rewrite.rewrite_paths {
            entity.class.serializer.get_decoder(fp).skip(reader);
        }
    }

    fn rewrite_fields(
        &mut self,
        reader: &mut SliceReader<'_>,
        path_reader: &FieldPathCodec,
        event: EntityEvents,
        entity: &mut Entity,
        track: bool,
        rewrite: bool,
    ) -> Result<(), ParserError> {
        self.entity_rewrite.rewrite_paths.clear();
        let section_start = bit_position(reader);
        let mut fp = FieldPath::default();

        loop {
            reader.refill();
            let op = path_reader.read_op(reader);
            if op == FieldOp::FieldPathEncodeFinish {
                break;
            }
            op.execute(reader, &mut fp);
            self.entity_rewrite.rewrite_paths.push(fp);
        }

        if !rewrite {
            for fp in self.entity_rewrite.rewrite_paths.iter() {
                let decoder = entity.class.serializer.get_decoder(fp);
                let value = decoder.decode(reader);
                entity.state.set(fp, value);
            }
            return Ok(());
        }

        if !track {
            let mut paths = std::mem::take(&mut self.entity_rewrite.rewrite_paths);
            for fp in paths.iter().copied() {
                let name = entity.class.serializer.get_name(&fp);
                let decoder = entity.class.serializer.get_decoder(&fp);
                let value_start = bit_position(reader);
                let value = decoder.decode(reader);
                let value_end = bit_position(reader);

                if let Some(next_value) = self.replace_entity_field(event, entity, &name, &value) {
                    entity.state.set(&fp, next_value.clone());
                    self.entity_rewrite.replacements.push(FieldReplacement {
                        value: FieldReplacementValue::Field {
                            serializer: entity.class.serializer.clone(),
                            fp,
                            value: next_value,
                        },
                        value_start,
                        value_end,
                    });
                }
            }
            paths.clear();
            self.entity_rewrite.rewrite_paths = paths;
            return Ok(());
        }

        self.entity_rewrite.decoded_fields.clear();
        let mut paths = std::mem::take(&mut self.entity_rewrite.rewrite_paths);
        for fp in paths.iter().copied() {
            let name = entity.class.serializer.get_name(&fp);
            let decoder = entity.class.serializer.get_decoder(&fp);
            let value_start = bit_position(reader);
            let value = decoder.decode(reader);
            let value_end = bit_position(reader);
            entity.state.set(&fp, value);
            self.entity_rewrite.decoded_fields.push(DecodedEntityField {
                fp,
                name,
                value_start,
                value_end,
            });
        }
        let original_paths = paths.clone();
        paths.clear();
        self.entity_rewrite.rewrite_paths = paths;

        let mut decoded_fields = std::mem::take(&mut self.entity_rewrite.decoded_fields);
        let mut replaced_values = Vec::new();
        for field in decoded_fields.drain(..) {
            let Some(value) = entity.state.get_value(&field.fp) else {
                continue;
            };
            let replacement = self.replace_entity_field(event, entity, &field.name, value);

            if let Some(next_value) = replacement {
                entity.state.set(&field.fp, next_value.clone());
                replaced_values.push((field.fp, next_value, field.value_start, field.value_end));
            }
        }

        let extra_fields = self.additional_entity_fields(event, entity)?;
        if extra_fields.is_empty() {
            for (fp, value, value_start, value_end) in replaced_values {
                self.entity_rewrite.replacements.push(FieldReplacement {
                    value: FieldReplacementValue::Field {
                        serializer: entity.class.serializer.clone(),
                        fp,
                        value,
                    },
                    value_start,
                    value_end,
                });
            }
        } else {
            let section_end = bit_position(reader);
            let mut values = Vec::new();
            for fp in original_paths.iter().copied() {
                if let Some(value) = entity.state.get_value(&fp) {
                    values.push((fp, value.clone()));
                }
            }
            for (name, value) in extra_fields {
                let fp = entity.class.serializer.get_path(&name).map_err(|error| {
                    ParserError::IoError(format!("cannot materialize entity field {name}: {error}"))
                })?;
                entity.state.set(&fp, value.clone());
                if let Some(existing) = values.iter_mut().find(|(path, _)| *path == fp) {
                    existing.1 = value;
                } else {
                    values.push((fp, value));
                }
            }
            values.sort_by(|(left, _), (right, _)| compare_field_paths(left, right));
            let (bytes, bit_len) = encode_fields(
                &self.entity_rewrite.field_path_codec,
                &entity.class.serializer,
                &values,
            )?;
            self.entity_rewrite.replacements.push(FieldReplacement {
                value: FieldReplacementValue::EncodedBits { bytes, bit_len },
                value_start: section_start,
                value_end: section_end,
            });
        }
        self.entity_rewrite.decoded_fields = decoded_fields;

        Ok(())
    }

    fn entity_baseline_state(&mut self, class_id: i32, serializer: &Serializer) -> FieldState {
        self.parser
            .context
            .baselines
            .states
            .entry(class_id)
            .or_insert_with(|| {
                let mut state = FieldState::default();
                if let Some(baseline) = self.parser.context.baselines.baselines.get(&class_id) {
                    self.parser.field_reader.read_fields(
                        &mut SliceReader::new(baseline.as_ref()),
                        serializer,
                        &mut state,
                    );
                }
                state
            })
            .clone()
    }
}

fn compare_field_paths(left: &FieldPath, right: &FieldPath) -> Ordering {
    for index in 0..=left.last.min(right.last) {
        match left.path[index].cmp(&right.path[index]) {
            Ordering::Equal => {}
            ordering => return ordering,
        }
    }
    left.last.cmp(&right.last)
}

fn component_delta(from: u16, to: u16) -> i32 {
    let from = if from == u16::MAX { -1 } else { from as i32 };
    to as i32 - from
}

fn encode_fields(
    codec: &FieldPathCodec,
    serializer: &Serializer,
    values: &[(FieldPath, crate::FieldValue)],
) -> Result<(Vec<u8>, usize), ParserError> {
    let mut bytes = Vec::new();
    let mut writer = BitstreamWriter::new(&mut bytes);
    let mut current = FieldPath::default();

    for (target, _) in values {
        if target.last > current.last {
            codec.write_op(&mut writer, FieldOp::PushNAndNonTopological)?;
            for index in 0..=current.last {
                let delta = component_delta(current.path[index], target.path[index]);
                writer.write_bit(delta != 0)?;
                if delta != 0 {
                    writer.write_var_i32(delta - 1)?;
                }
            }
            writer.write_ubit_var((target.last - current.last) as u32)?;
            for index in (current.last + 1)..=target.last {
                writer.write_ubit_var_fp(target.path[index] as i32)?;
            }
        } else if target.last < current.last {
            codec.write_op(&mut writer, FieldOp::PopNAndNonTopographical)?;
            writer.write_ubit_var_fp_unchecked((current.last - target.last) as i32)?;
            for index in 0..=target.last {
                let delta = component_delta(current.path[index], target.path[index]);
                writer.write_bit(delta != 0)?;
                if delta != 0 {
                    writer.write_var_i32(delta)?;
                }
            }
        } else {
            codec.write_op(&mut writer, FieldOp::NonTopoComplex)?;
            for index in 0..=target.last {
                let delta = component_delta(current.path[index], target.path[index]);
                writer.write_bit(delta != 0)?;
                if delta != 0 {
                    writer.write_var_i32(delta)?;
                }
            }
        }
        current = *target;
    }
    codec.write_op(&mut writer, FieldOp::FieldPathEncodeFinish)?;
    for (fp, value) in values {
        serializer.get_decoder(fp).encode(&mut writer, value)?;
    }
    let bit_len = writer.bits_written();
    writer.flush()?;
    drop(writer);
    Ok((bytes, bit_len))
}
