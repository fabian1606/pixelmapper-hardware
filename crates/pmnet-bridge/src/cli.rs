//! The `serve` command, reusable by `pmnet` (`pmnet serve` = these arguments + [`run_serve`]).

use crate::backend::mock::MockBackend;
use crate::backend::offline::OfflineBackend;
use crate::backend::NetBackend;
use crate::server::origin::OriginPolicy;
use crate::server::{serve, ServerConfig, DEFAULT_PORT};
use std::error::Error;
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
    #[arg(long)]
    pub mock: bool,
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
    } else {
        // TODO: PixelmapperNetBackend once the pixelmapper-net API is final (see backend::pixelmapper_net).
        run(
            config,
            Arc::new(OfflineBackend::new()),
            crate::backend::offline::OFFLINE_REASON,
        )
        .await
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
