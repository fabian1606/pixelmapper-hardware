//! `pmnet`: command-line tool for the Pixelmapper node and tubes.

use clap::{Parser, Subcommand};
use pixelmapper_net::rdm::Uid;
use std::path::PathBuf;

#[derive(Parser)]
#[command(name = "pmnet", version, about = "Pixelmapper Art-Net / RDM tool")]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand)]
enum Command {
    /// Discover Art-Net nodes (ArtPoll).
    Poll,
    /// Show the RDM table of devices.
    Tod,
    /// Send RDM GET/SET to a device.
    Rdm {
        #[command(subcommand)]
        action: RdmAction,
    },
    /// Node configuration.
    Node {
        #[command(subcommand)]
        action: NodeAction,
    },
    /// Flash a tube firmware image.
    Flash {
        /// Target tube UID (MMMM:DDDDDDDD).
        uid: Uid,
        /// Firmware image file.
        image: PathBuf,
        /// Rescue a tube stuck in the bootloader.
        #[arg(long)]
        rescue: bool,
    },
}

#[derive(Subcommand)]
enum RdmAction {
    /// GET a parameter.
    Get {
        /// Target UID.
        uid: Uid,
        /// Parameter ID (name or number).
        pid: String,
    },
    /// SET a parameter.
    Set {
        /// Target UID.
        uid: Uid,
        /// Parameter ID (name or number).
        pid: String,
        /// Parameter data as hex.
        data: String,
    },
}

#[derive(Subcommand)]
enum NodeAction {
    /// Show or change node configuration.
    Config,
}

fn main() {
    let cli = Cli::parse();
    let name = match cli.command {
        Command::Poll => "poll",
        Command::Tod => "tod",
        Command::Rdm { .. } => "rdm",
        Command::Node { .. } => "node",
        Command::Flash { .. } => "flash",
    };
    eprintln!("pmnet {name}: not implemented yet");
    std::process::exit(2);
}
