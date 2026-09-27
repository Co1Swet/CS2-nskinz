use source2_demo::prelude::*;
use source2_demo::proto::*;
use std::io::BufReader;

#[derive(Default)]
struct Chat;

#[observer]
#[uses_string_tables]
impl Chat {
    #[on_message]
    fn on_chat_message(&mut self, ctx: &Context, msg: CCitadelUserMsgChatMsg) -> ObserverResult {
        let userinfo = ctx.string_tables().get_by_name("userinfo")?;
        let info = CMsgPlayerInfo::decode(
            userinfo
                .get_row(msg.player_slot() as usize)?
                .value()
                .unwrap(),
        )?;
        println!(
            "[{}] {}: {}",
            if msg.all_chat() { "ALL" } else { "TEAM" },
            info.name(),
            msg.text()
        );
        Ok(())
    }
}

fn main() -> anyhow::Result<()> {
    let args = std::env::args().collect::<Vec<_>>();
    let Some(filepath) = args.get(1) else {
        eprintln!("Usage: {} <demofile>", args[0]);
        return Ok(());
    };

    let input = BufReader::new(std::fs::File::open(filepath)?);
    let mut parser = Parser::from_reader(input)?;

    parser.register_observer::<Chat>();

    let start = std::time::Instant::now();
    parser.run_to_end()?;
    println!("Elapsed: {:?}", start.elapsed());

    Ok(())
}
