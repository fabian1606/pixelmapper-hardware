//! `sim`: run a simulated node.

use crate::out::Out;
use clap::Args;
use pixelmapper_net::image::tube::build_test_image;
use pixelmapper_net::pids;
use pixelmapper_net::rdm::Uid;
use pixelmapper_net::sim::{SimConfig, SimFixture, SimNode, SimTube};
use pixelmapper_net::Result;
use serde_json::json;
use std::net::SocketAddr;

/// Arguments of `sim`.
#[derive(Args, Debug)]
pub struct SimArgs {
    /// Address the simulated node listens on.
    #[arg(long, default_value = "127.0.0.1:6454")]
    pub listen: SocketAddr,
    /// Simulated tubes on port 1 (valid test app 0.1.0).
    #[arg(long, default_value_t = 0)]
    pub tubes: u32,
    /// Simulated third-party fixtures on port 2.
    #[arg(long, default_value_t = 0)]
    pub fixtures: u32,
    /// Short name of the node.
    #[arg(long)]
    pub name: Option<String>,
}

/// UID of simulated tube `i`.
pub fn tube_uid(i: u32) -> Uid {
    Uid::new(pids::ESTA_MANUFACTURER, 0x0000_1001 + i)
}

/// UID of simulated fixture `i`.
pub fn fixture_uid(i: u32) -> Uid {
    Uid::new(0x7FF1, 0x0000_2001 + i)
}

/// `sim`.
pub async fn run(o: &Out, a: SimArgs) -> Result<()> {
    let mut cfg = SimConfig {
        bind: a.listen,
        ..SimConfig::default()
    };
    if let Some(n) = &a.name {
        cfg.short_name = n.clone();
    }
    let sim = SimNode::start(cfg).await?;
    let app: Vec<u8> = (0..1024u32).map(|i| (i * 7 + 3) as u8).collect();
    let image = build_test_image(&app, pids::ESTA_MANUFACTURER, pids::MODEL_TUBE, 0x0000_0100);
    let tubes: Vec<Uid> = (0..a.tubes).map(tube_uid).collect();
    for u in &tubes {
        sim.add_responder(1, Box::new(SimTube::new(*u, Some(image.clone()))));
    }
    let fixtures: Vec<Uid> = (0..a.fixtures).map(fixture_uid).collect();
    for u in &fixtures {
        sim.add_responder(2, Box::new(SimFixture::default_par(*u)));
    }
    let addr = sim.local_addr();
    o.emit(
        json!({
            "type": "sim",
            "address": addr,
            "uid": sim.uid(),
            "tubes": tubes,
            "fixtures": fixtures,
        }),
        || {
            format!(
                "simulated node {} listening on {addr} ({} tube(s) on port 1, {} fixture(s) on port 2)\n  connect: pmnet --bind 127.0.0.1:0 --broadcast {addr} poll\n  stop with Ctrl-C",
                sim.uid(),
                tubes.len(),
                fixtures.len()
            )
        },
    );
    // Run until Ctrl-C, then stop the simulator cleanly.
    let _ = tokio::signal::ctrl_c().await;
    sim.shutdown();
    Ok(())
}
