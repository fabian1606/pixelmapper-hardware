//! `pmnet-bridge`: the WebSocket bridge between the Pixelmapper web app and Art-Net 4 / RDM.
//! (Becomes `pmnet serve` once the pmnet CLI embeds `pmnet_bridge::cli`.)

use clap::{ArgAction, Parser, Subcommand};
use pmnet_bridge::cli::{run_serve, ServeArgs};
use std::process::ExitCode;
use tracing::Level;

#[derive(Parser)]
#[command(
    name = "pmnet-bridge",
    version,
    about = "WebSocket bridge between Pixelmapper and Art-Net 4 / RDM"
)]
struct Cli {
    /// More log output (-v debug, -vv trace).
    #[arg(short, long, action = ArgAction::Count, global = true)]
    verbose: u8,
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand)]
enum Command {
    /// Serve the bridge on ws://127.0.0.1:<port>/ (protocol/bridge-ws.md).
    Serve(ServeArgs),
}

#[tokio::main]
async fn main() -> ExitCode {
    let cli = Cli::parse();
    let level = match cli.verbose {
        0 => Level::INFO,
        1 => Level::DEBUG,
        _ => Level::TRACE,
    };
    tracing_subscriber::fmt()
        .with_max_level(level)
        .with_writer(std::io::stderr)
        .init();
    let result = match cli.command {
        Command::Serve(args) => run_serve(args).await,
    };
    match result {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("pmnet-bridge: {e}");
            ExitCode::FAILURE
        }
    }
}
