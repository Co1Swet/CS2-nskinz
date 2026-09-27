use source2_demo::prelude::*;
use source2_demo::proto::*;
use std::io::BufReader;

#[derive(Default)]
struct Chat;

#[observer]
impl Chat {
    fn get_player_name(&self, ctx: &Context, player_id: u32) -> Option<String> {
        let gi = ctx.replay_info().game_info.as_ref()?;
        let dota = gi.dota.as_ref()?;
        let pi = dota.player_info.get(player_id as usize)?;
        String::from_utf8_lossy(pi.player_name()).to_string().into()
    }

    #[on_message]
    fn handle_chat_msg(
        &mut self,
        ctx: &Context,
        chat_msg: CDotaUserMsgChatMessage,
    ) -> ObserverResult {
        let name = self
            .get_player_name(ctx, chat_msg.source_player_id() as u32)
            .unwrap_or("<unknown>".to_string());

        println!("{}: {}", name, chat_msg.message_text());

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
