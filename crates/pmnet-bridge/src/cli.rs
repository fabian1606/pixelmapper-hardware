//! The `serve` command, reusable by `pmnet` (`pmnet serve` = these arguments + [`run_serve`]).
//!
//! By default `serve` talks to the network through [`PixelmapperNetBackend`] (UDP 6454 on all interfaces).
//! `--mock` serves a simulated installation, `--offline` no network at all. A socket that cannot be bound is not
//! fatal: the app sees `udp.bound = false` with the reason in `udp.error`, and the bridge keeps retrying.

use crate::backend::mock::MockBackend;
use crate::backend::offline::OfflineBackend;
use crate::backend::pixelmapper_net::{BackendConfig, PixelmapperNetBackend};
use crate::backend::NetBackend;
use crate::server::origin::OriginPolicy;
use crate::server::{serve, ServerConfig, DEFAULT_PORT};
use pixelmapper_net::artnet;
use std::error::Error;
use std::net::{Ipv4Addr, SocketAddr};
use std::sync::Arc;

/// Arguments of `serve`.
#[derive(Debug, Clone, clap::Args)]
pub struct ServeArgs {
    /// TCP port on 127.0.0.1 (and [::1]).
    #[arg(long, default_value_t = DEFAULT_PORT)]
    pub port: u16,
    /// Additional allowed WebSocket Origin (`scheme://host`, `scheme://host:port` or `scheme://host:*`).
    /// Repeatable. localhost, 127.0.0.1 and the Tauri origins are always allowed.
    #[arg(long = "allow-origin", value_name = "ORIGIN")]
    pub allow_origin: Vec<String>,
    /// Serve a simulated installation (one node, three tubes) instead of the network, for app development.
    #[arg(long, conflicts_with = "offline")]
    pub mock: bool,
    /// Serve without network access (no Art-Net socket; the app sees `udp.bound = false`).
    #[arg(long)]
    pub offline: bool,
    /// Local IPv4 address of the Art-Net socket. 0.0.0.0 = all interfaces; a specific address restricts the
    /// ArtPoll/ArtSync broadcasts to its interface.
    #[arg(long, value_name = "IP", default_value_t = Ipv4Addr::UNSPECIFIED)]
    pub bind: Ipv4Addr,
    /// Local UDP port of the Art-Net socket (0 = any free port, e.g. next to a simulator).
    #[arg(long, value_name = "PORT", default_value_t = artnet::PORT)]
    pub udp_port: u16,
    /// UDP port of the nodes: destination of the ArtPoll broadcasts, poll targets and manual DMX targets.
    #[arg(long, value_name = "PORT", default_value_t = artnet::PORT)]
    pub node_port: u16,
    /// ArtPoll/ArtSync destination `ip:port` instead of the interface broadcasts; repeatable
    /// (e.g. `--broadcast 127.0.0.1:6455` for `pmnet sim --listen 127.0.0.1:6455`).
    #[arg(long = "broadcast", value_name = "ADDR")]
    pub broadcast: Vec<SocketAddr>,
}

impl ServeArgs {
    /// The backend configuration these arguments describe.
    pub fn backend_config(&self) -> BackendConfig {
        let mut cfg = BackendConfig::default();
        let c = &mut cfg.controller;
        c.bind = SocketAddr::from((self.bind, self.udp_port));
        c.node_port = self.node_port;
        c.broadcast = self.broadcast.clone();
        // Only a socket on the Art-Net port has to share it with other Art-Net programs on this host.
        c.reuse_port = self.udp_port == artnet::PORT;
        cfg
    }
}

/// Runs the bridge until Ctrl-C.
pub async fn run_serve(args: ServeArgs) -> Result<(), Box<dyn Error>> {
    let origins = OriginPolicy::with_defaults(&args.allow_origin)?;
    let config = ServerConfig {
        port: args.port,
        origins,
        ..ServerConfig::default()
    };
    if args.mock {
        run(
            config,
            Arc::new(MockBackend::demo()),
            "simulated network (--mock)",
        )
        .await
    } else if args.offline {
        run(
            config,
            Arc::new(OfflineBackend::new()),
            crate::backend::offline::OFFLINE_REASON,
        )
        .await
    } else {
        let cfg = args.backend_config();
        let mode = match cfg.controller.broadcast.as_slice() {
            [] => format!("Art-Net 4 / RDM on udp {}", cfg.controller.bind),
            targets => format!(
                "Art-Net 4 / RDM on udp {}, polling {}",
                cfg.controller.bind,
                targets
                    .iter()
                    .map(ToString::to_string)
                    .collect::<Vec<_>>()
                    .join(", ")
            ),
        };
        let backend = Arc::new(PixelmapperNetBackend::start(cfg).await);
        let udp = backend.status().udp;
        if !udp.bound {
            tracing::error!(
                "Art-Net UDP socket not bound ({}); serving anyway (the app sees udp.bound = false), retrying",
                udp.error.as_deref().unwrap_or("unknown error")
            );
        }
        let result = run(config, backend.clone(), &mode).await;
        backend.shutdown();
        result
    }
}

/// Serves `backend` until Ctrl-C, then closes all connections (1001).
async fn run<B: NetBackend>(
    config: ServerConfig,
    backend: Arc<B>,
    mode: &str,
) -> Result<(), Box<dyn Error>> {
    let origins: Vec<String> = config
        .origins
        .patterns()
        .iter()
        .map(ToString::to_string)
        .collect();
    let handle = serve(config, backend).await?;
    for addr in handle.local_addrs() {
        tracing::info!("listening on ws://{addr}/");
    }
    tracing::info!("backend: {mode}");
    tracing::info!("allowed origins: {}", origins.join(", "));
    tokio::signal::ctrl_c().await?;
    tracing::info!("shutting down");
    handle.shutdown().await;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use clap::Parser;

    #[derive(Parser)]
    struct Cli {
        #[command(flatten)]
        serve: ServeArgs,
    }

    #[test]
    fn defaults_and_simulator_flags() {
        let cfg = Cli::parse_from(["x"]).serve.backend_config();
        assert_eq!(cfg.controller.bind, "0.0.0.0:6454".parse().unwrap());
        assert!(cfg.controller.reuse_port && cfg.controller.broadcast.is_empty());
        assert_eq!(cfg.controller.reply_timeout.as_millis(), 1000);
        let cfg = Cli::parse_from([
            "x",
            "--bind",
            "127.0.0.1",
            "--udp-port",
            "0",
            "--broadcast",
            "127.0.0.1:6455",
        ])
        .serve
        .backend_config();
        assert_eq!(cfg.controller.bind, "127.0.0.1:0".parse().unwrap());
        assert!(!cfg.controller.reuse_port);
        assert_eq!(
            cfg.controller.broadcast,
            vec!["127.0.0.1:6455".parse::<SocketAddr>().unwrap()]
        );
        assert!(Cli::try_parse_from(["x", "--mock", "--offline"]).is_err());
    }
}
