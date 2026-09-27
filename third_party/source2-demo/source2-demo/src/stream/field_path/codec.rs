use crate::reader::{BitsReader, SliceReader};
use crate::writer::BitsWriter;
use crate::stream::field_path::{FieldOp, FIELD_OPS};
use bitter::BitReader;
use std::cmp::Ordering;
use std::collections::BinaryHeap;
use std::rc::Rc;

const DECODE_TABLE_BITS: u32 = 8;
const DECODE_TABLE_LEN: usize = 1 << DECODE_TABLE_BITS;

#[derive(Clone, Debug)]
pub(crate) struct FieldPathCodec {
    tree: Rc<FieldPathTree>,
    decode_table: Rc<[Option<FieldPathDecodeEntry>; DECODE_TABLE_LEN]>,
}

impl Default for FieldPathCodec {
    fn default() -> Self {
        let tree = Rc::new(FieldPathTree::default());
        Self {
            decode_table: Rc::new(tree.decode_table()),
            tree,
        }
    }
}

impl FieldPathCodec {
    #[inline]
    pub(crate) fn read_op(&self, reader: &mut SliceReader) -> FieldOp {
        reader.refill();
        if reader.bit_reader.lookahead_bits() >= DECODE_TABLE_BITS {
            let bits = reader.bit_reader.peek(DECODE_TABLE_BITS) as usize;
            if let Some(entry) = self.decode_table[bits] {
                reader.bit_reader.consume(entry.bit_len as u32);
                return entry.op;
            }
        }

        let mut node = self.tree.as_ref();
        loop {
            node = if reader.read_bool() {
                node.right()
            } else {
                node.left()
            };
            if let FieldPathTree::Leaf { value, .. } = node {
                return FIELD_OPS[*value as usize].0;
            }
        }
    }

    pub(crate) fn write_op<W: BitsWriter>(
        &self,
        writer: &mut W,
        op: FieldOp,
    ) -> std::io::Result<()> {
        let mut code = 0u64;
        let mut bit_len = 0u32;
        if !self.tree.find_code(op, &mut code, &mut bit_len) {
            return Err(std::io::Error::new(
                std::io::ErrorKind::InvalidInput,
                "field-path operation is absent from the Huffman tree",
            ));
        }
        writer.write_bits(bit_len, code)
    }
}

#[derive(Clone, Copy, Debug)]
struct FieldPathDecodeEntry {
    op: FieldOp,
    bit_len: u8,
}

#[derive(Clone, Debug)]
enum FieldPathTree {
    Leaf {
        weight: u32,
        value: u32,
    },
    Node {
        weight: u32,
        value: u32,
        left: Box<FieldPathTree>,
        right: Box<FieldPathTree>,
    },
}

impl Default for FieldPathTree {
    fn default() -> Self {
        let mut trees = FIELD_OPS
            .iter()
            .map(|(_, weight)| weight)
            .enumerate()
            .map(|(v, &w)| FieldPathTree::Leaf {
                value: v as u32,
                weight: if w == 0 { 1 } else { w },
            })
            .collect::<BinaryHeap<FieldPathTree>>();
        let mut n = 40;
        while let (Some(a), Some(b)) = (trees.pop(), trees.pop()) {
            trees.push(FieldPathTree::Node {
                weight: a.weight() + b.weight(),
                value: n,
                left: a.into(),
                right: b.into(),
            });
            n += 1;
            if trees.len() == 1 {
                break;
            }
        }
        trees.pop().unwrap()
    }
}

impl FieldPathTree {
    fn find_code(&self, op: FieldOp, code: &mut u64, bit_len: &mut u32) -> bool {
        match self {
            FieldPathTree::Leaf { value, .. } => FIELD_OPS[*value as usize].0 == op,
            FieldPathTree::Node { left, right, .. } => {
                *bit_len += 1;
                if left.find_code(op, code, bit_len) {
                    return true;
                }
                *bit_len -= 1;
                let right_bit = 1u64 << *bit_len;
                *code |= right_bit;
                *bit_len += 1;
                if right.find_code(op, code, bit_len) {
                    return true;
                }
                *bit_len -= 1;
                *code &= !right_bit;
                false
            }
        }
    }

    fn weight(&self) -> u32 {
        match self {
            FieldPathTree::Leaf { weight, .. } | FieldPathTree::Node { weight, .. } => *weight,
        }
    }
    fn value(&self) -> u32 {
        match self {
            FieldPathTree::Leaf { value, .. } | FieldPathTree::Node { value, .. } => *value,
        }
    }
    fn left(&self) -> &FieldPathTree {
        match self {
            FieldPathTree::Node { left, .. } => left,
            FieldPathTree::Leaf { .. } => unreachable!(),
        }
    }
    fn right(&self) -> &FieldPathTree {
        match self {
            FieldPathTree::Node { right, .. } => right,
            FieldPathTree::Leaf { .. } => unreachable!(),
        }
    }
    fn decode_table(&self) -> [Option<FieldPathDecodeEntry>; DECODE_TABLE_LEN] {
        let mut table = [None; DECODE_TABLE_LEN];
        self.fill_decode_table(&mut table, 0, 0);
        table
    }
    fn fill_decode_table(
        &self,
        table: &mut [Option<FieldPathDecodeEntry>; DECODE_TABLE_LEN],
        code: usize,
        bit_len: u8,
    ) {
        match self {
            FieldPathTree::Leaf { value, .. } => {
                if bit_len as u32 <= DECODE_TABLE_BITS {
                    let mask = if bit_len == 0 {
                        0
                    } else {
                        (1usize << bit_len) - 1
                    };
                    let entry = Some(FieldPathDecodeEntry {
                        op: FIELD_OPS[*value as usize].0,
                        bit_len,
                    });
                    for (bits, slot) in table.iter_mut().enumerate() {
                        if bits & mask == code {
                            *slot = entry;
                        }
                    }
                }
            }
            FieldPathTree::Node { left, right, .. } => {
                left.fill_decode_table(table, code, bit_len + 1);
                right.fill_decode_table(table, code | (1usize << bit_len), bit_len + 1);
            }
        }
    }
}

impl PartialEq for FieldPathTree {
    fn eq(&self, other: &Self) -> bool {
        self.weight() == other.weight() && self.value() == other.value()
    }
}
impl Eq for FieldPathTree {}
impl PartialOrd for FieldPathTree {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}
impl Ord for FieldPathTree {
    fn cmp(&self, other: &Self) -> Ordering {
        match self.weight().cmp(&other.weight()) {
            Ordering::Equal => self.value().cmp(&other.value()),
            ord => ord.reverse(),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::writer::{BitsWriter, BitstreamWriter};

    #[test]
    fn every_field_path_operation_round_trips_through_huffman_codes() {
        let codec = FieldPathCodec::default();
        for (expected, _) in FIELD_OPS {
            let mut bytes = Vec::new();
            let mut writer = BitstreamWriter::new(&mut bytes);
            codec.write_op(&mut writer, expected).unwrap();
            writer.flush().unwrap();
            drop(writer);
            let mut reader = SliceReader::new(&bytes);
            assert_eq!(codec.read_op(&mut reader), expected);
        }
    }
}
