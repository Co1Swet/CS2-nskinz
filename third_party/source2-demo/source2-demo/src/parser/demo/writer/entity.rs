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
use crate::writer::{write_var_u32_to_buf, BitsWriter, BitstreamWriter};
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

        let (rewritten, changed, entry_bit_deltas, entry_commands) =
            self.rewrite_entity_data(entity_data, packet_entities.updated_entries())?;
        if changed {
            if let Some(serialized_entities) = packet_entities.serialized_entities.as_deref() {
                packet_entities.serialized_entities = Some(rewrite_serialized_entity_lengths(
                    serialized_entities,
                    &entry_bit_deltas,
                    &entry_commands,
                )?);
            }
            packet_entities.entity_data = Some(rewritten);
            Ok(Some(packet_entities.encode_to_vec()))
        } else {
            Ok(None)
        }
    }

    fn rewrite_entity_data(
        &mut self,
        entity_data: &[u8],
        updated_entries: i32,
    ) -> Result<(Vec<u8>, bool, Vec<i64>, Vec<u32>), ParserError> {
        let mut reader = SliceReader::new(entity_data);
        self.entity_rewrite.replacements.clear();
        let mut index = usize::MAX;
        let path_reader = self.entity_rewrite.field_path_codec.clone();
        let mut entry_ranges = Vec::with_capacity(updated_entries.max(0) as usize);
        let mut entry_commands = Vec::with_capacity(updated_entries.max(0) as usize);

        for _ in 0..updated_entries {
            let entry_start = bit_position(&reader);
            let delta = reader.read_ubit_var();
            index = index.wrapping_add((delta + 1) as usize);

            let cmd = reader.read_bits(2);
            entry_commands.push(cmd);

            if cmd != 1 {
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
            entry_ranges.push((entry_start, bit_position(&reader)));
        }

        if self.entity_rewrite.replacements.is_empty() {
            return Ok((Vec::new(), false, Vec::new(), Vec::new()));
        }

        let mut out = Vec::with_capacity(entity_data.len());
        let mut writer = BitstreamWriter::new(&mut out);
        let mut copy_start = 0;
        let mut entry_index = 0usize;
        let mut entry_bit_deltas = vec![0i64; entry_ranges.len()];
        let mut replacements = std::mem::take(&mut self.entity_rewrite.replacements);
        for replacement in replacements.drain(..) {
            while entry_index + 1 < entry_ranges.len()
                && replacement.value_start >= entry_ranges[entry_index].1
            {
                entry_index += 1;
            }
            let (entry_start, entry_end) = entry_ranges[entry_index];
            if replacement.value_start < entry_start || replacement.value_end > entry_end {
                return Err(ParserError::IoError(
                    "entity field replacement crossed a serialized entity boundary".into(),
                ));
            }
            copy_original_bits(
                entity_data,
                copy_start,
                replacement.value_start - copy_start,
                &mut writer,
            )?;
            let encoded_start = writer.bits_written();
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
            let encoded_len = writer.bits_written() - encoded_start;
            let original_len = replacement.value_end - replacement.value_start;
            entry_bit_deltas[entry_index] += encoded_len as i64 - original_len as i64;
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
        Ok((out, true, entry_bit_deltas, entry_commands))
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
        let path_finish_start;

        loop {
            reader.refill();
            let op_start = bit_position(reader);
            let op = path_reader.read_op(reader);
            if op == FieldOp::FieldPathEncodeFinish {
                path_finish_start = op_start;
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
        for field in &decoded_fields {
            let Some(value) = entity.state.get_value(&field.fp) else {
                continue;
            };
            let replacement = self.replace_entity_field(event, entity, &field.name, value);

            if let Some(next_value) = replacement {
                entity.state.set(&field.fp, next_value.clone());
                replaced_values.push((field.fp, next_value));
            }
        }

        let extra_fields = self.additional_entity_fields(event, entity)?;
        let mut added_values = Vec::new();
        for (name, value) in extra_fields {
            let fp = entity.class.serializer.get_path(&name).map_err(|error| {
                ParserError::IoError(format!("cannot materialize entity field {name}: {error}"))
            })?;
            entity.state.set(&fp, value.clone());
            if original_paths.contains(&fp) {
                if let Some(existing) = replaced_values.iter_mut().find(|(path, _)| *path == fp) {
                    existing.1 = value;
                } else {
                    replaced_values.push((fp, value));
                }
            } else if let Some(existing) = added_values.iter_mut().find(|(path, _)| *path == fp) {
                existing.1 = value;
            } else {
                added_values.push((fp, value));
            }
        }

        if added_values.is_empty() {
            for field in &decoded_fields {
                let Some((_, value)) = replaced_values.iter().find(|(path, _)| *path == field.fp)
                else {
                    continue;
                };
                self.entity_rewrite.replacements.push(FieldReplacement {
                    value: FieldReplacementValue::Field {
                        serializer: entity.class.serializer.clone(),
                        fp: field.fp,
                        value: value.clone(),
                    },
                    value_start: field.value_start,
                    value_end: field.value_end,
                });
            }
        } else {
            let section_end = bit_position(reader);
            added_values.sort_by(|(left, _), (right, _)| compare_field_paths(left, right));
            let (bytes, bit_len) = encode_fields_with_appended_paths(
                &self.entity_rewrite.field_path_codec,
                &entity.class.serializer,
                reader.source_buffer,
                section_start,
                path_finish_start,
                &original_paths,
                &decoded_fields,
                &replaced_values,
                &added_values,
            )?;
            self.entity_rewrite.replacements.push(FieldReplacement {
                value: FieldReplacementValue::EncodedBits { bytes, bit_len },
                value_start: section_start,
                value_end: section_end,
            });
        }
        decoded_fields.clear();
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

fn rewrite_serialized_entity_lengths(
    encoded: &[u8],
    entry_bit_deltas: &[i64],
    entry_commands: &[u32],
) -> Result<Vec<u8>, ParserError> {
    if entry_bit_deltas.len() != entry_commands.len() {
        return Err(ParserError::IoError(format!(
            "serialized entity metadata mismatch: packet has {} deltas and {} commands",
            entry_bit_deltas.len(),
            entry_commands.len()
        )));
    }

    let mut values = Vec::with_capacity(entry_bit_deltas.len());
    let mut offset = 0usize;
    while offset < encoded.len() {
        let mut value = 0u64;
        let mut shift = 0u32;
        loop {
            let Some(&byte) = encoded.get(offset) else {
                return Err(ParserError::IoError(
                    "serialized entity length table ended inside a varint".into(),
                ));
            };
            offset += 1;
            if shift >= 64 && byte & 0x7f != 0 {
                return Err(ParserError::IoError(
                    "serialized entity length table contains an overflowing varint".into(),
                ));
            }
            value |= u64::from(byte & 0x7f) << shift.min(63);
            if byte & 0x80 == 0 {
                break;
            }
            shift += 7;
            if shift > 63 {
                return Err(ParserError::IoError(
                    "serialized entity length table contains an oversized varint".into(),
                ));
            }
        }
        values.push(value);
    }

    let field_entry_indices = entry_commands
        .iter()
        .enumerate()
        .filter_map(|(index, cmd)| matches!(*cmd, 0 | 2).then_some(index))
        .collect::<Vec<_>>();
    let non_leave_entry_indices = entry_commands
        .iter()
        .enumerate()
        .filter_map(|(index, cmd)| (*cmd != 1).then_some(index))
        .collect::<Vec<_>>();
    let entry_indices = if values.len() == entry_bit_deltas.len() {
        (0..entry_bit_deltas.len()).collect::<Vec<_>>()
    } else if values.len() == field_entry_indices.len() {
        // Current CS2 packets omit leave-PVS and delete commands from this table:
        // neither command carries a serialized field payload whose size can change.
        field_entry_indices
    } else if values.len() == non_leave_entry_indices.len() {
        // Keep compatibility with builds that include delete commands but omit
        // leave-PVS commands from serialized_entities.
        non_leave_entry_indices
    } else {
        return Err(ParserError::IoError(format!(
            "serialized entity length count mismatch: table has {}, packet has {} entries ({} field-bearing, {} non-leave)",
            values.len(),
            entry_bit_deltas.len(),
            field_entry_indices.len(),
            non_leave_entry_indices.len()
        )));
    };

    let mut rewritten = Vec::with_capacity(encoded.len());
    for (value, entry_index) in values.into_iter().zip(entry_indices) {
        let adjusted = i128::from(value) + i128::from(entry_bit_deltas[entry_index]);
        let adjusted = u64::try_from(adjusted).map_err(|_| {
            ParserError::IoError("serialized entity bit length became negative".into())
        })?;
        write_var_u32_to_buf(&mut rewritten, adjusted);
    }
    Ok(rewritten)
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

fn write_field_path_transition<W: BitsWriter>(
    codec: &FieldPathCodec,
    writer: &mut W,
    current: &mut FieldPath,
    target: &FieldPath,
) -> Result<(), ParserError> {
    if target.last > current.last {
        codec.write_op(writer, FieldOp::PushNAndNonTopological)?;
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
        codec.write_op(writer, FieldOp::PopNAndNonTopographical)?;
        writer.write_ubit_var_fp_unchecked((current.last - target.last) as i32)?;
        for index in 0..=target.last {
            let delta = component_delta(current.path[index], target.path[index]);
            writer.write_bit(delta != 0)?;
            if delta != 0 {
                writer.write_var_i32(delta)?;
            }
        }
    } else {
        codec.write_op(writer, FieldOp::NonTopoComplex)?;
        for index in 0..=target.last {
            let delta = component_delta(current.path[index], target.path[index]);
            writer.write_bit(delta != 0)?;
            if delta != 0 {
                writer.write_var_i32(delta)?;
            }
        }
    }
    *current = *target;
    Ok(())
}

fn encode_fields_with_appended_paths(
    codec: &FieldPathCodec,
    serializer: &Serializer,
    source: &[u8],
    section_start: usize,
    path_finish_start: usize,
    original_paths: &[FieldPath],
    original_fields: &[DecodedEntityField],
    replaced_values: &[(FieldPath, crate::FieldValue)],
    added_values: &[(FieldPath, crate::FieldValue)],
) -> Result<(Vec<u8>, usize), ParserError> {
    let mut bytes = Vec::new();
    let mut writer = BitstreamWriter::new(&mut bytes);

    copy_original_bits(
        source,
        section_start,
        path_finish_start - section_start,
        &mut writer,
    )?;
    let mut current = original_paths.last().copied().unwrap_or_default();
    for (target, _) in added_values {
        write_field_path_transition(codec, &mut writer, &mut current, target)?;
    }
    codec.write_op(&mut writer, FieldOp::FieldPathEncodeFinish)?;

    for field in original_fields {
        if let Some((_, value)) = replaced_values.iter().find(|(path, _)| *path == field.fp) {
            serializer
                .get_decoder(&field.fp)
                .encode(&mut writer, value)?;
        } else {
            copy_original_bits(
                source,
                field.value_start,
                field.value_end - field.value_start,
                &mut writer,
            )?;
        }
    }
    for (fp, value) in added_values {
        serializer.get_decoder(fp).encode(&mut writer, value)?;
    }

    let bit_len = writer.bits_written();
    writer.flush()?;
    drop(writer);
    Ok((bytes, bit_len))
}

#[cfg(test)]
mod tests {
    use super::rewrite_serialized_entity_lengths;

    #[test]
    fn serialized_entity_lengths_follow_entity_bit_deltas() {
        let encoded = [0x0e, 0xb8, 0x03, 0x1e]; // 14, 440, 30 bits.
        let rewritten = rewrite_serialized_entity_lengths(&encoded, &[0, 472, -5], &[0, 2, 0])
            .expect("rewrite serialized entity lengths");
        assert_eq!(rewritten, [0x0e, 0x90, 0x07, 0x19]); // 14, 912, 25 bits.
    }

    #[test]
    fn serialized_entity_lengths_skip_entries_without_field_payloads() {
        let encoded = [0x0e, 0x1e]; // Update and create entries only.
        let rewritten = rewrite_serialized_entity_lengths(&encoded, &[5, 0, 0, -3], &[0, 1, 3, 2])
            .expect("rewrite sparse serialized entity lengths");
        assert_eq!(rewritten, [0x13, 0x1b]); // 19 and 27 bits.
    }

    #[test]
    fn serialized_entity_lengths_accept_117_values_for_191_packet_entries() {
        let mut commands = vec![1; 191];
        for command in commands.iter_mut().skip(74) {
            *command = 0;
        }
        let mut deltas = vec![0; 191];
        deltas[100] = 9;
        let encoded = vec![20; 117];

        let rewritten = rewrite_serialized_entity_lengths(&encoded, &deltas, &commands)
            .expect("rewrite the sparse length layout seen in current CS2 demos");

        assert_eq!(rewritten.len(), 117);
        assert_eq!(rewritten[26], 29);
    }

    #[test]
    fn serialized_entity_lengths_accept_any_field_entry_count() {
        for field_count in 0..=256 {
            let mut commands = Vec::with_capacity(field_count * 3);
            let mut deltas = Vec::with_capacity(field_count * 3);
            for index in 0..field_count {
                commands.push(1); // Leave PVS: no serialized field payload.
                deltas.push(0);
                commands.push(if index % 2 == 0 { 0 } else { 2 }); // Update/create.
                deltas.push(1);
                if index % 3 == 0 {
                    commands.push(3); // Delete: no serialized field payload.
                    deltas.push(0);
                }
            }
            let encoded = vec![20; field_count];

            let rewritten = rewrite_serialized_entity_lengths(&encoded, &deltas, &commands)
                .expect("rewrite an arbitrary sparse serialized entity length table");

            assert_eq!(rewritten, vec![21; field_count]);
        }
    }

    #[test]
    fn serialized_entity_lengths_support_delete_entries_without_leave_entries() {
        let encoded = [0x0e, 0x07, 0x1e]; // Update, delete and create entries.
        let rewritten = rewrite_serialized_entity_lengths(&encoded, &[5, 0, 0, -3], &[0, 1, 3, 2])
            .expect("rewrite non-leave serialized entity lengths");
        assert_eq!(rewritten, [0x13, 0x07, 0x1b]); // 19, 7 and 27 bits.
    }

    #[test]
    fn serialized_entity_length_count_must_match_a_supported_layout() {
        assert!(rewrite_serialized_entity_lengths(&[0x0e], &[0, 0], &[0, 2]).is_err());
    }

    #[test]
    fn serialized_entity_length_cannot_become_negative() {
        assert!(rewrite_serialized_entity_lengths(&[0x01], &[-2], &[0]).is_err());
    }
}
