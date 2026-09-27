use source2_demo::prelude::*;
use source2_demo::proto::CDotaUserMsgChatMessage;
use source2_demo::writer::*;

use std::fs::File;
use std::io::{BufReader, BufWriter};

#[derive(Default)]
struct RemoveChat;

#[rewriter]
impl RemoveChat {
    #[rewrite_packet_message]
    fn remove_chat(
        &mut self,
        _message: CDotaUserMsgChatMessage,
    ) -> Result<MessageRewrite, ParserError> {
        Ok(MessageRewrite::Drop)
    }
}

fn main() -> anyhow::Result<()> {
    let args = std::env::args().collect::<Vec<_>>();
    let (input_path, output_path) = match args.as_slice() {
        [_, input, output] => (input, output),
        _ => {
            eprintln!("Usage: {} <input.dem> <output.dem>", args[0]);
            return Ok(());
        }
    };

    let input = BufReader::new(File::open(input_path)?);
    let output = BufWriter::new(File::create(output_path)?);

    let mut writer = DemoWriter::from_reader(input, output)?;
    writer.register_rewriter::<RemoveChat>();

    let start = std::time::Instant::now();
    writer.run()?;
    println!("Elapsed: {:?}", start.elapsed());

    Ok(())
}
