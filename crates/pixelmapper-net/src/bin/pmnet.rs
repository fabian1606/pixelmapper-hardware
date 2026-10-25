//! `pmnet`: command-line tool for the Pixelmapper node and tubes.
//!
//! Exit codes: 0 ok, 1 error (message on stderr with the stable [`pixelmapper_net::Error::code`]), 2 usage.
//!
//! Connection options (`--bind`, `--broadcast`, `--node-port`, `--no-reuse-port`) go before the subcommand
//! (`node config port` has its own `--bind <BindIndex>`); the other global options can go anywhere.

#[path = "pmnet/decode.rs"]
mod decode;
#[path = "pmnet/dmx.rs"]
mod dmx;
#[path = "pmnet/flash.rs"]
mod flash;
#[path = "pmnet/net.rs"]
mod net;
#[path = "pmnet/node.rs"]
mod node;
#[path = "pmnet/out.rs"]
mod out;
#[path = "pmnet/poll.rs"]
mod poll;
#[path = "pmnet/rdm.rs"]
mod rdm;
#[path = "pmnet/sim.rs"]
mod sim;

use clap::{Args, Parser, Subcommand, ValueEnum};
use pixelmapper_net::Error;
use std::net::SocketAddr;
use std::path::PathBuf;

/// Pixelmapper Art-Net 4 / RDM tool.
#[derive(Parser, Debug)]
#[command(name = "pmnet", version, about = "Pixelmapper Art-Net / RDM tool")]
pub struct Cli {
    #[command(flatten)]
    pub global: Global,
    #[command(subcommand)]
    pub command: Command,
}

/// Options shared by all network commands.
#[derive(Args, Debug, Clone)]
pub struct Global {
    /// Local address to bind (before the subcommand).
    #[arg(long, default_value = "0.0.0.0:6454")]
    pub bind: SocketAddr,
    /// ArtPoll/ArtSync destination, repeatable (default: directed broadcasts of all interfaces).
    #[arg(long)]
    pub broadcast: Vec<SocketAddr>,
    /// Destination port for the automatic broadcasts.
    #[arg(long, default_value_t = 6454)]
    pub node_port: u16,
    /// Do not set SO_REUSEPORT.
    #[arg(long)]
    pub no_reuse_port: bool,
    /// Timeout for RDM attempts and configuration replies (ms).
    #[arg(long, global = true)]
    pub timeout_ms: Option<u64>,
    /// Machine output: one JSON object per line.
    #[arg(long, global = true)]
    pub json: bool,
    /// Discovery wait (ms).
    #[arg(long, global = true, default_value_t = 3000)]
    pub wait: u64,
    /// Node: NodeId (`7FF0:80123456`, `mac:…`, `ip:…`) or IPv4 address. Default: the only node found.
    #[arg(long, global = true)]
    pub node: Option<String>,
    /// Port-Address (`Net:Sub:Uni` or 15-bit number).
    #[arg(long, global = true)]
    pub port: Option<String>,
    /// Port number 1..4 of the node.
    #[arg(long, global = true, value_parser = clap::value_parser!(u8).range(1..=4))]
    pub port_number: Option<u8>,
}

/// Top-level commands.
#[derive(Subcommand, Debug)]
pub enum Command {
    /// Discover Art-Net nodes (ArtPoll).
    Poll {
        /// Keep running and print events.
        #[arg(long)]
        watch: bool,
    },
    /// Show the RDM device table of the node's RDM ports (or --port).
    Tod {
        /// Send AtcFlush first (full discovery).
        #[arg(long)]
        flush: bool,
    },
    /// RDM requests.
    Rdm {
        #[command(subcommand)]
        action: RdmAction,
    },
    /// Node configuration.
    Node {
        #[command(subcommand)]
        action: NodeAction,
    },
    /// Send DMX (ArtDmx).
    Dmx(dmx::DmxArgs),
    /// Firmware images and updates.
    Flash {
        #[command(subcommand)]
        action: FlashAction,
    },
    /// Run a simulated node until Ctrl-C.
    Sim(sim::SimArgs),
}

/// `rdm …`
#[derive(Subcommand, Debug)]
pub enum RdmAction {
    /// GET a parameter.
    Get(RdmCmd),
    /// SET a parameter.
    Set(RdmCmd),
    /// Read everything needed to show a device.
    Describe {
        /// Target UID (MMMM:DDDDDDDD).
        uid: String,
    },
}

/// Arguments of `rdm get` / `rdm set`.
#[derive(Args, Debug)]
pub struct RdmCmd {
    /// Target UID (MMMM:DDDDDDDD).
    pub uid: String,
    /// Parameter ID (name, 0xHEX or decimal).
    pub pid: String,
    /// Parameter data as hex.
    pub data: Option<String>,
    /// Sub-device.
    #[arg(long, default_value_t = 0)]
    pub sub_device: u16,
}

/// `node …`
#[derive(Subcommand, Debug)]
pub enum NodeAction {
    /// Show or change the node configuration.
    Config {
        #[command(subcommand)]
        action: node::ConfigAction,
    },
}

/// `flash …`
#[derive(Subcommand, Debug)]
pub enum FlashAction {
    /// Update a tube (RDM bootloader).
    Tube {
        /// Tube UID.
        uid: String,
        /// Tube image (`tube_fw-<version>.hex`).
        hex: PathBuf,
        /// Allow an older version.
        #[arg(long)]
        allow_downgrade: bool,
        /// Rescue a tube without a working app (BOOT_CATCH; power-cycle the tube when asked).
        #[arg(long)]
        rescue: bool,
        /// Wait after BOOT_FINISH before checking the app (ms).
        #[arg(long, hide = true)]
        finish_wait_ms: Option<u64>,
    },
    /// Update the node.
    Node {
        /// Node image (.bin or .alf).
        image: PathBuf,
        /// Transfer method.
        #[arg(long, value_enum, default_value_t = NodeMethod::Rdm)]
        method: NodeMethod,
        /// Allow an older version.
        #[arg(long)]
        allow_downgrade: bool,
    },
    /// Show image information (.hex, .bin, .alf) without network.
    Info {
        /// Image file.
        file: PathBuf,
    },
    /// Wrap a node .bin into an Art-Net .alf file.
    MakeAlf {
        /// Node binary.
        bin: PathBuf,
        /// Output .alf.
        out: PathBuf,
    },
}

/// Node update method.
#[derive(ValueEnum, Debug, Clone, Copy, PartialEq, Eq)]
pub enum NodeMethod {
    /// ArtFirmwareMaster with the .alf.
    Artnet,
    /// RDM FW_BEGIN / FW_WRITE / FW_FINISH.
    Rdm,
}

async fn run(cli: Cli) -> Result<(), Error> {
    let g = cli.global;
    let o = out::Out::new(g.json);
    match cli.command {
        Command::Poll { watch } => poll::poll(&g, &o, watch).await,
        Command::Tod { flush } => poll::tod(&g, &o, flush).await,
        Command::Rdm { action } => match action {
            RdmAction::Get(c) => rdm::get_set(&g, &o, c, false).await,
            RdmAction::Set(c) => rdm::get_set(&g, &o, c, true).await,
            RdmAction::Describe { uid } => rdm::describe(&g, &o, &uid).await,
        },
        Command::Node {
            action: NodeAction::Config { action },
        } => node::config(&g, &o, action).await,
        Command::Dmx(a) => dmx::dmx(&g, &o, a).await,
        Command::Flash { action } => match action {
            FlashAction::Tube {
                uid,
                hex,
                allow_downgrade,
                rescue,
                finish_wait_ms,
            } => {
                let opts = flash::TubeArgs {
                    allow_downgrade,
                    rescue,
                    finish_wait_ms,
                };
                flash::tube(&g, &o, &uid, &hex, opts).await
            }
            FlashAction::Node {
                image,
                method,
                allow_downgrade,
            } => flash::node(&g, &o, &image, method, allow_downgrade).await,
            FlashAction::Info { file } => flash::info(&o, &file),
            FlashAction::MakeAlf { bin, out } => flash::make_alf(&o, &bin, &out),
        },
        Command::Sim(a) => sim::run(&o, a).await,
    }
}

#[tokio::main]
async fn main() {
    let cli = Cli::parse();
    let json = cli.global.json;
    if let Err(e) = run(cli).await {
        out::error(json, &e);
        std::process::exit(1);
    }
}
