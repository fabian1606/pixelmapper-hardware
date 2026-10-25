//! One WebSocket connection: upgrade with `Origin` check, `hello`, the ordered read loop (commands, binary DMX,
//! deadlines), command dispatch and cleanup.
//!
//! Messages of a connection are processed in order. Commands that only touch session state (`hello`, `nodes`,
//! `poll`, poll targets, `dmxRoutes`, `imageInfo`, `flashCancel`, `flashJobs`) complete before the next message
//! is read, so e.g. `dmxRoutes` applies to every later binary message. Commands that wait for the network run as
//! tasks (at most `max_inflight` per connection) and answer whenever they finish; `id` correlates.

use super::conn::{
    spawn_writer, CloseReason, ConnTx, CLOSE_GOING_AWAY, CLOSE_INCOMPATIBLE, CLOSE_NO_HELLO,
};
use super::dmx::{interval_for_rate, Batch, DmxRouter};
use super::hub::HubMsg;
use super::validate::{
    check_node_port, check_port, check_rdm_payload, check_sub_device, check_unicast, unknown_node,
    validate_art_address, validate_art_input, validate_routes,
};
use super::{Shared, BRIDGE_VERSION};
use crate::backend::{
    BackendError, DmxTarget, FlashRequest, NetBackend, RdmAddress, RdmPayload, RdmRequest,
};
use crate::protocol::frame::decode_frame_set;
use crate::protocol::messages::{
    error_json, parse_envelope, result_json, semver_major, Command, Empty, FlashJobsResult,
    FlashStartResult, HelloArgs, RdmArgs, RdmResult,
};
use crate::protocol::{
    CommandError, ErrorCode, Event, HelloEvent, HexData, LogEntry, LogLevel, NodeId, NodeState,
    PortRef, RdmUid, PROTOCOL_VERSION,
};
use futures_util::stream::SplitStream;
use futures_util::{FutureExt, StreamExt};
use serde::Serialize;
use std::collections::BTreeSet;
use std::future::Future;
use std::net::Ipv4Addr;
use std::panic::AssertUnwindSafe;
use std::sync::Arc;
use std::time::{Duration, Instant};
use tokio::net::TcpStream;
use tokio::sync::{oneshot, Semaphore};
use tokio_tungstenite::tungstenite::handshake::server::{ErrorResponse, Request, Response};
use tokio_tungstenite::tungstenite::http::{header, StatusCode};
use tokio_tungstenite::tungstenite::protocol::WebSocketConfig;
use tokio_tungstenite::tungstenite::{Bytes, Message};
use tokio_tungstenite::WebSocketStream;

/// At most one `log` warning per connection and interval (malformed binary messages can arrive at 60/s).
const WARN_INTERVAL: Duration = Duration::from_secs(1);

// ── Upgrade ───────────────────────────────────────────────────────────────────────────────────────────────────────

/// Performs the WebSocket upgrade; answers 403 when the `Origin` is not allowed (contract §1).
// The callback's large `Err` type is dictated by tungstenite's handshake API.
#[allow(clippy::result_large_err)]
async fn accept<B: NetBackend>(
    shared: &Shared<B>,
    stream: TcpStream,
) -> Option<WebSocketStream<TcpStream>> {
    let policy = shared.config.origins.clone();
    let callback = move |req: &Request, resp: Response| -> Result<Response, ErrorResponse> {
        // A non-UTF-8 Origin never matches.
        let origin = req
            .headers()
            .get(header::ORIGIN)
            .map(|v| v.to_str().unwrap_or(""));
        if policy.allows(origin) {
            Ok(resp)
        } else {
            tracing::info!(origin = ?origin, "rejected WebSocket upgrade from a foreign origin");
            let mut reject = ErrorResponse::new(Some("origin not allowed".into()));
            *reject.status_mut() = StatusCode::FORBIDDEN;
            Err(reject)
        }
    };
    let ws_config = WebSocketConfig::default()
        .max_message_size(Some(shared.config.max_message_size))
        .max_frame_size(Some(shared.config.max_message_size));
    let upgrade =
        tokio_tungstenite::accept_hdr_async_with_config(stream, callback, Some(ws_config));
    match tokio::time::timeout(shared.config.handshake_timeout, upgrade).await {
        Ok(Ok(ws)) => Some(ws),
        Ok(Err(e)) => {
            tracing::debug!("WebSocket upgrade failed: {e}");
            None
        }
        Err(_) => {
            tracing::debug!("WebSocket upgrade timed out");
            None
        }
    }
}

/// Serves one TCP connection until it closes.
pub(super) async fn run_connection<B: NetBackend>(
    shared: Arc<Shared<B>>,
    stream: TcpStream,
    id: u64,
) {
    let Some(ws) = accept(&shared, stream).await else {
        return;
    };
    let (sink, mut source) = ws.split();
    let (conn, writer) = spawn_writer(
        id,
        sink,
        shared.config.outbound_capacity,
        shared.config.ping_interval,
    );
    tracing::debug!(conn = id, "connection open");

    let status = shared.mirror.read().status.clone();
    let hello = HelloEvent {
        bridge: BRIDGE_VERSION.into(),
        protocol: PROTOCOL_VERSION.into(),
        interfaces: status.interfaces,
        udp: status.udp,
    };
    if let Ok(text) = serde_json::to_string(&hello) {
        conn.send_text(text);
    }

    let mut session = Session {
        inflight: Arc::new(Semaphore::new(shared.config.max_inflight.max(1))),
        router: DmxRouter::new(shared.config.dmx_refresh),
        shared,
        physical: (id % 256) as u8,
        conn,
        handshaken: false,
        poll_targets: BTreeSet::new(),
        last_warning: None,
    };
    session.run(&mut source).await;
    session.cleanup();
    drop(session);
    let _ = writer.await;
    tracing::debug!(conn = id, "connection closed");
}

// ── Session ───────────────────────────────────────────────────────────────────────────────────────────────────────

/// Whether the read loop continues after a message.
enum Flow {
    Continue,
    Stop,
}

/// Per-connection state owned by the read loop.
struct Session<B: NetBackend> {
    shared: Arc<Shared<B>>,
    conn: ConnTx,
    /// `Physical` of this connection's ArtDmx (connection number mod 256).
    physical: u8,
    /// Client `hello` accepted.
    handshaken: bool,
    router: DmxRouter,
    /// Poll targets added by this connection.
    poll_targets: BTreeSet<Ipv4Addr>,
    /// Limits concurrently running network commands.
    inflight: Arc<Semaphore>,
    last_warning: Option<Instant>,
}

impl<B: NetBackend> Session<B> {
    /// The read loop: messages in order, plus the hello deadline, DMX deadlines, shutdown and close signals.
    async fn run(&mut self, source: &mut SplitStream<WebSocketStream<TcpStream>>) {
        let mut closed = self.conn.closed();
        let mut shutdown = self.shared.shutdown.clone();
        let hello_deadline = tokio::time::sleep(self.shared.config.hello_timeout);
        tokio::pin!(hello_deadline);
        loop {
            let dmx_deadline = self.router.next_deadline();
            let dmx_sleep = tokio::time::sleep_until(tokio::time::Instant::from_std(
                dmx_deadline.unwrap_or_else(|| Instant::now() + Duration::from_secs(3600)),
            ));
            tokio::select! {
                biased;
                // The guards returned by `wait_for` are dropped inside the async blocks (they are not `Send`).
                _ = async { let _ = closed.wait_for(Option::is_some).await; } => break,
                _ = async { let _ = shutdown.wait_for(|down| *down).await; } => {
                    self.conn.close_after_queue(CLOSE_GOING_AWAY, "bridge shutting down");
                    break;
                }
                _ = &mut hello_deadline, if !self.handshaken => {
                    self.conn.close_after_queue(CLOSE_NO_HELLO, "no hello command received");
                    break;
                }
                _ = dmx_sleep, if dmx_deadline.is_some() => {
                    if let Some(batch) = self.router.poll_due(Instant::now()) {
                        self.send_batch(batch);
                    }
                }
                msg = source.next() => match msg {
                    Some(Ok(Message::Text(text))) => {
                        if let Flow::Stop = self.on_text(text.as_str()).await {
                            break;
                        }
                    }
                    Some(Ok(Message::Binary(data))) => self.on_binary(data),
                    Some(Ok(Message::Close(_))) | Some(Err(_)) | None => {
                        self.conn.close_now(CloseReason::Gone);
                        break;
                    }
                    // Ping/pong are answered by tungstenite.
                    Some(Ok(_)) => {}
                },
            }
        }
    }

    /// Drops this connection's poll targets and fan-out registration. Routes die with the session.
    fn cleanup(&mut self) {
        for ip in std::mem::take(&mut self.poll_targets) {
            self.release_poll_target(ip);
        }
        let _ = self.shared.hub.send(HubMsg::Unregister(self.conn.id()));
    }

    // ── Replies ───────────────────────────────────────────────────────────────────────────────────────────────

    /// Queues a `result`.
    fn reply_ok<T: Serialize>(&self, id: u32, result: &T) {
        self.conn.send_text(result_json(id, result));
    }

    /// Queues an `error`.
    fn reply_error(&self, id: Option<u32>, error: &CommandError) {
        self.conn.send_text(error_json(id, error));
    }

    /// Queues the outcome of a synchronous command.
    fn reply<T: Serialize>(&self, id: u32, outcome: Result<T, CommandError>) {
        match outcome {
            Ok(r) => self.reply_ok(id, &r),
            Err(e) => self.reply_error(Some(id), &e),
        }
    }

    /// Sends a rate-limited `log` warning to this connection only.
    fn warn(&mut self, text: String) {
        let now = Instant::now();
        if self.last_warning.is_some_and(|t| now < t + WARN_INTERVAL) {
            return;
        }
        self.last_warning = Some(now);
        tracing::debug!(conn = self.conn.id(), "{text}");
        if let Ok(json) = serde_json::to_string(&Event::Log(LogEntry::new(LogLevel::Warn, text))) {
            self.conn.send_text(json);
        }
    }

    /// Runs a network command as a task (bounded per connection) and replies when it finishes. A panicking
    /// backend becomes an `internal` error instead of a missing reply.
    async fn spawn_command<T, F>(&self, id: u32, fut: F)
    where
        T: Serialize + Send + 'static,
        F: Future<Output = Result<T, CommandError>> + Send + 'static,
    {
        let permit = self.inflight.clone().acquire_owned().await.ok();
        let conn = self.conn.clone();
        tokio::spawn(async move {
            let _permit = permit;
            let text = match guarded(fut).await {
                Ok(r) => result_json(id, &r),
                Err(e) => error_json(Some(id), &e),
            };
            conn.send_text(text);
        });
    }

    /// Like `spawn_command` for commands answering `{ node }`: the result goes through the hub so it is ordered
    /// with the `node` events (contract §1 Ordering).
    async fn spawn_node_command<F>(&self, id: u32, fut: F)
    where
        F: Future<Output = Result<NodeState, CommandError>> + Send + 'static,
    {
        let permit = self.inflight.clone().acquire_owned().await.ok();
        let conn = self.conn.clone();
        let hub = self.shared.hub.clone();
        tokio::spawn(async move {
            let _permit = permit;
            match guarded(fut).await {
                Ok(node) => {
                    let _ = hub.send(HubMsg::NodeResult { conn, id, node });
                }
                Err(e) => {
                    conn.send_text(error_json(Some(id), &e));
                }
            }
        });
    }

    // ── Checks against the mirrored node table ────────────────────────────────────────────────────────────────

    /// Runs `check` on the node, or fails with `unknownNode`.
    fn with_node<T>(
        &self,
        node_id: &NodeId,
        check: impl FnOnce(&NodeState) -> Result<T, CommandError>,
    ) -> Result<T, CommandError> {
        let mirror = self.shared.mirror.read();
        let node = mirror
            .nodes
            .get(node_id)
            .ok_or_else(|| unknown_node(node_id))?;
        check(node)
    }

    /// Port range + node + port existence.
    fn check_node_port(&self, node_id: &NodeId, port: PortRef) -> Result<(), CommandError> {
        check_port("port", port)?;
        self.with_node(node_id, |n| check_node_port(n, port))
    }

    /// Full check of RDM addressing.
    fn check_rdm_address(
        &self,
        node_id: &NodeId,
        port: PortRef,
        sub_device: u16,
        unicast: Option<RdmUid>,
    ) -> Result<(), CommandError> {
        check_sub_device(sub_device)?;
        if let Some(uid) = unicast {
            check_unicast(uid)?;
        }
        self.check_node_port(node_id, port)
    }

    // ── Text messages ─────────────────────────────────────────────────────────────────────────────────────────

    /// Parses and dispatches one text message.
    async fn on_text(&mut self, text: &str) -> Flow {
        let envelope = match parse_envelope(text) {
            Ok(e) => e,
            Err(e) => {
                self.reply_error(e.id, &e.error);
                return Flow::Continue;
            }
        };
        let id = envelope.id;
        if !self.handshaken && envelope.kind != "hello" {
            self.reply_error(
                Some(id),
                &CommandError::new(ErrorCode::HelloRequired, "send the hello command first"),
            );
            return Flow::Continue;
        }
        match Command::parse(&envelope.kind, envelope.body) {
            Ok(Some(cmd)) => self.dispatch(id, cmd).await,
            Ok(None) => {
                let msg = format!("unknown command {:?}", envelope.kind);
                self.reply_error(Some(id), &CommandError::new(ErrorCode::UnknownCommand, msg));
                Flow::Continue
            }
            Err(e) => {
                self.reply_error(Some(id), &e);
                Flow::Continue
            }
        }
    }

    /// Executes one command.
    async fn dispatch(&mut self, id: u32, cmd: Command) -> Flow {
        let backend = self.shared.backend.clone();
        match cmd {
            Command::Hello(args) => return self.hello(id, args).await,
            Command::Nodes => {
                let _ = self.shared.hub.send(HubMsg::Snapshot {
                    conn: self.conn.clone(),
                    id,
                });
            }
            Command::Poll => self.reply(
                id,
                backend
                    .poll_now()
                    .map(|()| Empty {})
                    .map_err(CommandError::from),
            ),
            Command::AddPollTarget(a) => {
                if self.poll_targets.insert(a.ip) {
                    self.acquire_poll_target(a.ip);
                }
                self.reply_ok(id, &Empty {});
            }
            Command::RemovePollTarget(a) => {
                if self.poll_targets.remove(&a.ip) {
                    self.release_poll_target(a.ip);
                }
                self.reply_ok(id, &Empty {});
            }
            Command::TodRequest(a) => match self.check_node_port(&a.node_id, a.port) {
                Ok(()) => {
                    let fut = async move {
                        backend
                            .tod_request(a.node_id, a.port)
                            .await
                            .map(|()| Empty {})
                    };
                    self.spawn_command(id, async move { fut.await.map_err(CommandError::from) })
                        .await;
                }
                Err(e) => self.reply_error(Some(id), &e),
            },
            Command::TodControl(a) => match self.check_node_port(&a.node_id, a.port) {
                Ok(()) => {
                    let fut = async move {
                        backend
                            .tod_control(a.node_id, a.port, a.action)
                            .await
                            .map(|()| Empty {})
                    };
                    self.spawn_command(id, async move { fut.await.map_err(CommandError::from) })
                        .await;
                }
                Err(e) => self.reply_error(Some(id), &e),
            },
            Command::RdmGet(a) => self.rdm(id, a, false).await,
            Command::RdmSet(a) => self.rdm(id, a, true).await,
            Command::RdmDescribe(a) => {
                match self.check_rdm_address(&a.node_id, a.port, a.sub_device, Some(a.uid)) {
                    Ok(()) => {
                        let refresh = a.refresh;
                        let address = RdmAddress {
                            node_id: a.node_id,
                            port: a.port,
                            uid: a.uid,
                            sub_device: a.sub_device,
                        };
                        let fut = async move { backend.rdm_describe(address, refresh).await };
                        self.spawn_command(
                            id,
                            async move { fut.await.map_err(CommandError::from) },
                        )
                        .await;
                    }
                    Err(e) => self.reply_error(Some(id), &e),
                }
            }
            Command::ArtAddress(a) => {
                match self.with_node(&a.node_id, |n| validate_art_address(n, &a)) {
                    Ok(()) => {
                        let fut = async move { backend.art_address(a).await };
                        self.spawn_node_command(id, async move {
                            fut.await.map_err(CommandError::from)
                        })
                        .await;
                    }
                    Err(e) => self.reply_error(Some(id), &e),
                }
            }
            Command::IpProg(a) => match self.with_node(&a.node_id, |_| Ok(())) {
                Ok(()) => {
                    let fut = async move { backend.ip_prog(a).await };
                    self.spawn_command(id, async move { fut.await.map_err(CommandError::from) })
                        .await;
                }
                Err(e) => self.reply_error(Some(id), &e),
            },
            Command::ArtInput(a) => {
                match self.with_node(&a.node_id, |n| validate_art_input(n, a.bind, &a.disabled)) {
                    Ok(()) => {
                        let fut =
                            async move { backend.art_input(a.node_id, a.bind, a.disabled).await };
                        self.spawn_node_command(id, async move {
                            fut.await.map_err(CommandError::from)
                        })
                        .await;
                    }
                    Err(e) => self.reply_error(Some(id), &e),
                }
            }
            Command::ImageInfo(a) => {
                self.reply(id, backend.image_info(&a.hex).map_err(CommandError::from))
            }
            Command::Flash(a) => {
                match self.check_rdm_address(&a.node_id, a.port, a.sub_device, Some(a.uid)) {
                    Ok(()) => {
                        let req = FlashRequest {
                            address: RdmAddress {
                                node_id: a.node_id,
                                port: a.port,
                                uid: a.uid,
                                sub_device: a.sub_device,
                            },
                            hex: a.hex,
                            allow_downgrade: a.allow_downgrade,
                        };
                        let fut = async move {
                            backend
                                .flash_start(req)
                                .await
                                .map(|job| FlashStartResult { job })
                        };
                        self.spawn_command(
                            id,
                            async move { fut.await.map_err(CommandError::from) },
                        )
                        .await;
                    }
                    Err(e) => self.reply_error(Some(id), &e),
                }
            }
            Command::FlashCancel(a) => self.reply(
                id,
                backend
                    .flash_cancel(&a.job_id)
                    .map(|()| Empty {})
                    .map_err(CommandError::from),
            ),
            Command::FlashJobs => self.reply_ok(
                id,
                &FlashJobsResult {
                    jobs: backend.flash_jobs(),
                },
            ),
            Command::DmxRoutes(a) => {
                let interfaces = self.shared.mirror.read().status.interfaces.clone();
                match validate_routes(&a.routes, &interfaces) {
                    Ok(()) => {
                        self.router.set_routes(&a.routes);
                        self.reply_ok(id, &Empty {});
                    }
                    Err(e) => self.reply_error(Some(id), &e),
                }
            }
        }
        Flow::Continue
    }

    /// Client `hello`: version check, then registration for events (the hub queues the `result`).
    async fn hello(&mut self, id: u32, args: HelloArgs) -> Flow {
        let theirs = semver_major(&args.protocol);
        if theirs.is_none() || theirs != semver_major(PROTOCOL_VERSION) {
            let msg = format!(
                "bridge speaks protocol {PROTOCOL_VERSION}, client {}",
                args.protocol
            );
            let err = CommandError::new(ErrorCode::IncompatibleProtocol, msg)
                .with_detail("protocol", serde_json::Value::from(PROTOCOL_VERSION));
            self.reply_error(Some(id), &err);
            self.conn
                .close_after_queue(CLOSE_INCOMPATIBLE, "incompatible protocol");
            return Flow::Stop;
        }
        if self.handshaken {
            self.reply_ok(id, &Empty {});
            return Flow::Continue;
        }
        let (ack, registered) = oneshot::channel();
        if self
            .shared
            .hub
            .send(HubMsg::Register {
                conn: self.conn.clone(),
                id,
                ack,
            })
            .is_err()
        {
            self.reply_error(
                Some(id),
                &CommandError::new(ErrorCode::Internal, "event hub stopped"),
            );
            return Flow::Continue;
        }
        let _ = registered.await;
        self.handshaken = true;
        tracing::info!(conn = self.conn.id(), client = %args.client, version = %args.client_version, "client hello");
        Flow::Continue
    }

    /// `rdmGet` / `rdmSet`.
    async fn rdm(&mut self, id: u32, a: RdmArgs, set: bool) {
        let checked = check_rdm_payload(set, a.data.is_some(), a.value.is_some())
            .and_then(|()| self.check_rdm_address(&a.node_id, a.port, a.sub_device, None));
        if let Err(e) = checked {
            self.reply_error(Some(id), &e);
            return;
        }
        let payload = match (a.data, a.value) {
            (Some(data), _) => RdmPayload::Data(data.0),
            (None, Some(value)) => RdmPayload::Value(value),
            (None, None) => RdmPayload::Empty,
        };
        let req = RdmRequest {
            address: RdmAddress {
                node_id: a.node_id,
                port: a.port,
                uid: a.uid,
                sub_device: a.sub_device,
            },
            pid: a.pid,
            payload,
        };
        let backend = self.shared.backend.clone();
        self.spawn_command(id, async move {
            let response = if set {
                backend.rdm_set(req).await
            } else {
                backend.rdm_get(req).await
            }?;
            Ok(RdmResult {
                data: HexData(response.data),
                value: response.value,
                broadcast: response.broadcast.then_some(true),
            })
        })
        .await;
    }

    // ── Poll targets ──────────────────────────────────────────────────────────────────────────────────────────

    /// Counts one more user of `ip`; pushes the set to the backend when it changed.
    fn acquire_poll_target(&self, ip: Ipv4Addr) {
        let mut targets = self
            .shared
            .poll_targets
            .lock()
            .unwrap_or_else(|e| e.into_inner());
        let count = targets.entry(ip).or_insert(0);
        *count += 1;
        if *count == 1 {
            let all: Vec<Ipv4Addr> = targets.keys().copied().collect();
            self.shared.backend.set_poll_targets(&all);
        }
    }

    /// Counts one user of `ip` less; pushes the set to the backend when it changed.
    fn release_poll_target(&self, ip: Ipv4Addr) {
        let mut targets = self
            .shared
            .poll_targets
            .lock()
            .unwrap_or_else(|e| e.into_inner());
        let Some(count) = targets.get_mut(&ip) else {
            return;
        };
        *count -= 1;
        if *count == 0 {
            targets.remove(&ip);
            let all: Vec<Ipv4Addr> = targets.keys().copied().collect();
            self.shared.backend.set_poll_targets(&all);
        }
    }

    // ── Binary messages ───────────────────────────────────────────────────────────────────────────────────────

    /// A DMX frame set (ignored before the handshake; malformed ones produce a `log` warning).
    fn on_binary(&mut self, data: Bytes) {
        if !self.handshaken {
            return;
        }
        let set = match decode_frame_set(&data) {
            Ok(set) => set,
            Err(e) => {
                self.warn(format!("dropped binary message: {e}"));
                return;
            }
        };
        let batch = {
            let mirror = self.shared.mirror.read();
            self.router
                .on_frame_set(set, Instant::now(), |target| match target {
                    DmxTarget::Node(id) => {
                        interval_for_rate(mirror.nodes.get(id).map_or(0, |n| n.refresh_rate))
                    }
                    DmxTarget::Ip(_) => interval_for_rate(0),
                })
        };
        if let Some(batch) = batch {
            self.send_batch(batch);
        }
    }

    /// Hands a batch to the backend.
    fn send_batch(&mut self, batch: Batch) {
        match self
            .shared
            .backend
            .send_dmx(&batch.frames, batch.sync, self.physical)
        {
            Ok(()) => {}
            // The app learns about an unbound socket from `hello.udp` / `bridgeStatus`.
            Err(BackendError::UdpUnavailable) => {}
            Err(e) => self.warn(format!("DMX not sent: {e}")),
        }
    }
}

/// Awaits a command future; a panic becomes an `internal` error.
async fn guarded<T>(fut: impl Future<Output = Result<T, CommandError>>) -> Result<T, CommandError> {
    AssertUnwindSafe(fut)
        .catch_unwind()
        .await
        .unwrap_or_else(|_| {
            Err(CommandError::new(
                ErrorCode::Internal,
                "the network backend panicked",
            ))
        })
}
