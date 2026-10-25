//! [`PixelmapperNetBackend`]: the [`NetBackend`] on top of `crates/pixelmapper-net` (its async `Controller`).
//!
//! The library does every packet, timer and procedure (discovery, node merge, TOD assembly, RDM transport with
//! queueing/retries/ACK_TIMER/ACK_OVERFLOW, configuration packets, DMX, tube flash). This adapter keeps the
//! contract's view of it: a node table and TODs mirrored from the library's events (state only from the network),
//! the contract's ids and shapes, RDM value decoding, firmware jobs that outlive connections, and the UDP socket
//! state (an unbound socket is reported in `udp`, never fatal; binding is retried).
//!
//! Everything that depends on the library's data model is in this module tree: `convert` (ids, node state,
//! errors), `values` (RDM values), `address` (ArtAddress packets), `describe` (`rdmDescribe`) and `flash`
//! (jobs).
//!
//! # Mapping
//!
//! | `NetBackend` | pixelmapper-net | Notes |
//! |---|---|---|
//! | `status` | `controller::interfaces()` (re-read every 5 s), `Controller::bind` / `local_addr` | bind failure → `udp.bound = false` + `udp.error`, retried every 5 s; `bridgeStatus` on change |
//! | `snapshot`, `subscribe` | `Event::NodeUpdated` / `NodeGone` / `TodChanged` → own table; `Controller::nodes()` / `tod()` after (re)attach and on lag | node id `UID` / `mac:` / `ip:`; ports `{bind, index}` ↔ Port-Address via the node's ports; one ArtTodRequest per RDM output port on appearance (`auto_tod_request`) |
//! | `poll_now`, `set_poll_targets` | `poll_now()` (spawned), `set_poll_targets()` | targets survive a later bind |
//! | `tod_request` | `tod_refresh()` (spawned) | the library reports only *changed* TODs; an unchanged answer is emitted by the adapter |
//! | `tod_control` | `tod_control(ATC_*)` | after a flush that leaves the TOD unchanged, the TOD is re-requested and emitted after `tod_settle` |
//! | `rdm_get`, `rdm_set` | `rdm(&id, pa, RdmRequest { options.sub_device })` | value codec in `values` (contract §4 table, PARAMETER_DESCRIPTION cache, `pids.toml`) |
//! | `rdm_describe` | `rdm()` + `rdm::pd` codecs | per-PID `errors`; personalities/parameter descriptions cached per (`uid`, `subDevice`, `softwareVersionId`) |
//! | `art_address` | `art_address(&id, ArtAddress)` per planned packet | 1 s + one resend each; `node` event before returning |
//! | `ip_prog` | `ip_prog_change(&id, IpProgChange)` (`Command = 0` for a read) | 1 s + one resend |
//! | `art_input` | `art_input(&id, bind, disabled)` | 1 s + one resend; `node` event before returning |
//! | `image_info`, `flash_*` | `TubeImage::parse_hex`, `flash_tube_with()` → `FlashJob` | checks before the job; progress ≤ 10/s |
//! | `send_dmx` | `try_send_dmx(dest, pa, data, physical)`, `try_send_sync()` | non-blocking; frames for unknown nodes dropped |
//! | events | `Event::Diag` → `diag`, `QueuedMessage` → `rdmQueued`, `FlashProgress` → `flashProgress` | bind errors and lag → `log` |

mod address;
pub(crate) mod convert;
mod describe;
mod flash;
mod values;

use super::{
    BackendError, DmxFrame, DmxTarget, FlashRequest, NetBackend, RdmAddress, RdmPayload,
    RdmRequest, RdmResponse, Snapshot,
};
use crate::protocol::{
    ArtAddressArgs, BridgeStatus, Diag, Event, FirmwareImageInfo, FlashJob, FlashPhase, HexData,
    IpProgArgs, IpProgReply, LogEntry, LogLevel, NodeId, NodeState, PortDirection, PortRef,
    RdmDeviceDescription, RdmQueued, RdmUid, Tod, TodAction, UdpState,
};
use convert::{backend_error, node_id, uid_to_bridge, uid_to_net};
use pixelmapper_net as net;
use pixelmapper_net::artnet::{ArtTodControl, IpProgChange, PortAddress};
use pixelmapper_net::controller::Interface;
use pixelmapper_net::flash::tube::TubeFlashOptions;
use pixelmapper_net::link::RdmOptions;
use pixelmapper_net::rdm::pd::ParameterDescription;
use pixelmapper_net::rdm::{cc, pid, Uid};
use pixelmapper_net::{Controller, ControllerConfig};
use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};
use std::future::Future;
use std::net::{Ipv4Addr, SocketAddr};
use std::sync::{Arc, Mutex, MutexGuard, RwLock, Weak};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};
use tokio::sync::broadcast;
use tokio::sync::broadcast::error::RecvError;
use tokio::task::JoinHandle;

// ── Configuration ─────────────────────────────────────────────────────────────────────────────────────────────────

/// Settings of [`PixelmapperNetBackend`].
#[derive(Debug, Clone)]
pub struct BackendConfig {
    /// The library controller (socket, broadcast targets, poll interval, RDM timing).
    pub controller: ControllerConfig,
    /// Options of tube updates (`allow_downgrade` is set per job).
    pub tube_flash: TubeFlashOptions,
    /// Pause between attempts to bind the Art-Net socket after a failure.
    pub bind_retry: Duration,
    /// How often the local interfaces are re-read (`bridgeStatus` on change).
    pub interface_poll: Duration,
    /// After `todControl flush`: how long to wait for the node's new TOD before re-requesting it.
    pub tod_settle: Duration,
    /// How long a TOD re-request waits for ArtTodData.
    pub tod_timeout: Duration,
}

impl BackendConfig {
    /// Settings around a controller configuration.
    pub fn new(controller: ControllerConfig) -> Self {
        Self {
            controller,
            tube_flash: TubeFlashOptions::default(),
            bind_retry: Duration::from_secs(5),
            interface_poll: Duration::from_secs(5),
            tod_settle: Duration::from_secs(3),
            tod_timeout: Duration::from_secs(2),
        }
    }

    /// Talks to nodes on localhost (simulators): socket `127.0.0.1:0`, ArtPoll/ArtSync to `targets`.
    pub fn local(targets: Vec<SocketAddr>) -> Self {
        Self::new(contract_timing(ControllerConfig::local(targets)))
    }
}

impl Default for BackendConfig {
    /// UDP 6454 on all interfaces, contract timing.
    fn default() -> Self {
        Self::new(contract_timing(ControllerConfig::default()))
    }
}

/// Applies the bridge contract's timing (§9) to a controller configuration: configuration answers within 1 s
/// (the adapter resends once), a node is gone after 3 poll cycles (+ 1 s for the nodes' random reply delay).
pub fn contract_timing(mut c: ControllerConfig) -> ControllerConfig {
    c.reply_timeout = Duration::from_secs(1);
    if !c.poll_interval.is_zero() {
        c.node_timeout = c.poll_interval * 3 + Duration::from_secs(1);
    }
    c
}

// ── State ─────────────────────────────────────────────────────────────────────────────────────────────────────────

/// One node of the table.
struct NodeEntry {
    /// Contract state (what the server mirrors).
    state: NodeState,
    /// The library's id.
    lib_id: net::NodeId,
    /// Where the node's replies come from (ArtDmx destination).
    address: SocketAddr,
    /// Ports (with their Port-Address) an ArtTodRequest was sent for since the node appeared or rebooted into
    /// other firmware: the node sends unsolicited ArtTodData only to IPs that requested a TOD (node contract
    /// §6.1).
    tod_requested: BTreeSet<(PortRef, u16)>,
}

/// RDM-capable output ports of a node with their Port-Address (the ports to subscribe to with ArtTodRequest).
fn tod_ports(node: &NodeState) -> BTreeSet<(PortRef, u16)> {
    node.ports
        .iter()
        .filter(|p| p.direction != PortDirection::Input && p.can_output && p.output.rdm)
        .map(|p| (p.port, p.output_port_address))
        .collect()
}

/// Node table, TODs and bridge status; one lock, never held across `.await`.
struct NetState {
    status: BridgeStatus,
    /// Every IPv4 interface (incl. loopback), for `NodeState.interface`.
    all_interfaces: Vec<Interface>,
    nodes: BTreeMap<NodeId, NodeEntry>,
    tods: BTreeMap<(NodeId, PortRef), Tod>,
    poll_targets: Vec<Ipv4Addr>,
}

/// What the adapter learned about RDM devices.
#[derive(Default)]
struct RdmState {
    descriptions: values::Descriptions,
    described: HashMap<(RdmUid, u16), describe::Cached>,
}

/// Shared part of the backend.
struct Inner {
    cfg: BackendConfig,
    ctrl: RwLock<Option<Controller>>,
    events: broadcast::Sender<Event>,
    net: Mutex<NetState>,
    rdm: Mutex<RdmState>,
    jobs: Mutex<flash::Jobs>,
    tasks: Mutex<Vec<JoinHandle<()>>>,
}

/// Locks a mutex; a poisoned lock is still usable (state is replaced field by field).
fn lock<T>(m: &Mutex<T>) -> MutexGuard<'_, T> {
    m.lock().unwrap_or_else(|e| e.into_inner())
}

/// Milliseconds since the Unix epoch.
fn now_ms() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0)
}

/// Text of a bind error for `udp.error`.
fn bind_error_text(e: &net::Error) -> String {
    match e {
        net::Error::Io(io) => io.to_string(),
        e => e.to_string(),
    }
}

impl Inner {
    fn net(&self) -> MutexGuard<'_, NetState> {
        lock(&self.net)
    }

    /// Emits an event (no receivers is fine).
    fn emit(&self, event: Event) {
        let _ = self.events.send(event);
    }

    /// Emits a `log` event and traces it.
    fn log(&self, level: LogLevel, text: String) {
        match level {
            LogLevel::Error => tracing::error!("{text}"),
            LogLevel::Warn => tracing::warn!("{text}"),
            LogLevel::Info => tracing::info!("{text}"),
            LogLevel::Debug => tracing::debug!("{text}"),
        }
        self.emit(Event::Log(LogEntry::new(level, text)));
    }

    /// Spawns a background task that is aborted on shutdown.
    fn spawn(&self, fut: impl Future<Output = ()> + Send + 'static) {
        let mut tasks = lock(&self.tasks);
        tasks.retain(|t| !t.is_finished());
        tasks.push(tokio::spawn(fut));
    }

    /// Local address the socket is (to be) bound to.
    fn bind_ip(&self) -> Ipv4Addr {
        match self.cfg.controller.bind {
            SocketAddr::V4(a) => *a.ip(),
            SocketAddr::V6(_) => Ipv4Addr::UNSPECIFIED,
        }
    }

    /// The controller, or `udpUnavailable` while the socket is not bound.
    fn ctrl(&self) -> Result<Controller, BackendError> {
        self.ctrl
            .read()
            .unwrap_or_else(|e| e.into_inner())
            .clone()
            .ok_or(BackendError::UdpUnavailable)
    }

    /// Controller and library id of a node of the table.
    fn resolve(&self, node: &NodeId) -> Result<(Controller, net::NodeId), BackendError> {
        let ctrl = self.ctrl()?;
        let lib_id = self
            .net()
            .nodes
            .get(node)
            .map(|e| e.lib_id)
            .ok_or_else(|| BackendError::UnknownNode(node.clone()))?;
        Ok((ctrl, lib_id))
    }

    /// Controller, library id and the Port-Address of `port` (from the library's current node state).
    fn resolve_port(
        &self,
        node: &NodeId,
        port: PortRef,
    ) -> Result<(Controller, net::NodeId, PortAddress), BackendError> {
        let (ctrl, lib_id) = self.resolve(node)?;
        let n = ctrl
            .node(&lib_id)
            .ok_or_else(|| BackendError::UnknownNode(node.clone()))?;
        let pa = n
            .ports
            .iter()
            .find(|p| p.bind_index == port.bind && p.index == port.index)
            .map(|p| p.port_address)
            .ok_or(BackendError::UnknownPort(port))?;
        Ok((ctrl, lib_id, pa))
    }

    // ── Node table ────────────────────────────────────────────────────────────────────────────────────────────

    /// Stores a node state from the library; emits `node` when anything but `lastSeen` changed. Returns the
    /// stored contract state.
    ///
    /// Also subscribes to the TOD of every RDM output port that was not requested yet (a node appearing or
    /// reappearing, BindIndex pages arriving one by one, a changed Port-Address, RDM switched on, a reboot into
    /// other firmware): one ArtTodRequest per port.
    fn apply_node(&self, n: &net::NodeState) -> NodeState {
        let (state, requests) = {
            let mut st = self.net();
            let state = convert::node_state(n, &st.all_interfaces, now_ms());
            let wanted = tod_ports(&state);
            match st.nodes.get_mut(&state.node_id) {
                Some(old) => {
                    old.lib_id = n.id;
                    old.address = n.address;
                    // A restart (other firmware, or the NodeReport reply counter starting over) forgets the
                    // node's TOD subscribers.
                    let restarted = convert::restarted(
                        convert::report_counter(&old.state.node_report),
                        convert::report_counter(&state.node_report),
                    );
                    if old.state.firmware != state.firmware || restarted {
                        old.tod_requested.clear();
                    }
                    let requests: Vec<(PortRef, u16)> =
                        wanted.difference(&old.tod_requested).copied().collect();
                    old.tod_requested.extend(requests.iter().copied());
                    if old.state.same_except_last_seen(&state) {
                        old.state.last_seen = state.last_seen;
                        (old.state.clone(), requests)
                    } else {
                        old.state = state.clone();
                        self.emit(Event::Node {
                            node: state.clone(),
                        });
                        (state, requests)
                    }
                }
                None => {
                    st.nodes.insert(
                        state.node_id.clone(),
                        NodeEntry {
                            state: state.clone(),
                            lib_id: n.id,
                            address: n.address,
                            tod_requested: wanted.clone(),
                        },
                    );
                    self.emit(Event::Node {
                        node: state.clone(),
                    });
                    (state, wanted.into_iter().collect())
                }
            }
        };
        self.request_tods(n.id, requests);
        state
    }

    /// Sends one ArtTodRequest per listed port (in the background).
    fn request_tods(&self, lib_id: net::NodeId, ports: Vec<(PortRef, u16)>) {
        if ports.is_empty() {
            return;
        }
        let Ok(ctrl) = self.ctrl() else {
            return;
        };
        self.spawn(async move {
            for (port, raw) in ports {
                let Some(pa) = PortAddress::new(raw) else {
                    continue;
                };
                if let Err(e) = ctrl.tod_request(&lib_id, pa).await {
                    tracing::debug!("ArtTodRequest for port {port} of {lib_id} not sent: {e}");
                }
            }
        });
    }

    /// Removes a node and its TODs; emits `nodeGone`.
    fn remove_node(&self, lib_id: &net::NodeId) {
        let id = node_id(lib_id);
        let mut st = self.net();
        let Some(entry) = st.nodes.remove(&id) else {
            return;
        };
        st.tods.retain(|(n, _), _| *n != id);
        self.emit(Event::NodeGone {
            node_id: id,
            last_seen: entry.state.last_seen,
        });
    }

    /// Stores and emits a complete TOD of every port of the node that uses Port-Address `pa`.
    fn apply_tod(&self, lib_id: &net::NodeId, pa: PortAddress, uids: &[Uid], nak: bool) {
        let id = node_id(lib_id);
        let mut st = self.net();
        let Some(entry) = st.nodes.get(&id) else {
            tracing::debug!("TOD from {id}, which is not in the node table");
            return;
        };
        let ports = ports_for(&entry.state, pa);
        let mut sorted: Vec<RdmUid> = uids.iter().map(|u| uid_to_bridge(*u)).collect();
        sorted.sort();
        for port in ports {
            let tod = Tod {
                node_id: id.clone(),
                port,
                port_address: pa.raw(),
                available: !nak,
                uids: if nak { Vec::new() } else { sorted.clone() },
            };
            st.tods.insert((id.clone(), port), tod.clone());
            self.emit(Event::Tod(tod));
        }
    }

    /// Re-reads the library's node table and TODs (after attaching or when the event stream lagged).
    fn sync_from(&self, ctrl: &Controller) {
        let nodes = ctrl.nodes();
        let present: HashSet<NodeId> = nodes.iter().map(|n| node_id(&n.id)).collect();
        for n in &nodes {
            self.apply_node(n);
        }
        let gone: Vec<net::NodeId> = self
            .net()
            .nodes
            .values()
            .filter(|e| !present.contains(&e.state.node_id))
            .map(|e| e.lib_id)
            .collect();
        for g in &gone {
            self.remove_node(g);
        }
        for n in &nodes {
            let mut seen = HashSet::new();
            for p in n
                .ports
                .iter()
                .filter(|p| p.direction != net::state::PortDirection::Input)
            {
                if !seen.insert(p.port_address) {
                    continue;
                }
                let Some(uids) = ctrl.tod(&n.id, p.port_address) else {
                    continue;
                };
                let mut sorted: Vec<RdmUid> = uids.iter().map(|u| uid_to_bridge(*u)).collect();
                sorted.sort();
                let key = (node_id(&n.id), PortRef::new(p.bind_index, p.index));
                let known = self
                    .net()
                    .tods
                    .get(&key)
                    .is_some_and(|t| t.available && t.uids == sorted);
                if !known {
                    self.apply_tod(&n.id, p.port_address, &uids, false);
                }
            }
        }
    }

    // ── Library events ────────────────────────────────────────────────────────────────────────────────────────

    /// One event of the library.
    fn on_net_event(&self, event: net::Event) {
        match event {
            net::Event::NodeUpdated { node } => {
                self.apply_node(&node);
            }
            net::Event::NodeGone { id } => self.remove_node(&id),
            net::Event::TodChanged {
                node,
                port_address,
                uids,
                nak,
                ..
            } => self.apply_tod(&node, port_address, &uids, nak),
            net::Event::QueuedMessage {
                node,
                port_address,
                uid,
                message,
            } => self.on_queued(&node, port_address, uid, message),
            net::Event::Diag {
                node,
                address,
                priority,
                logical_port,
                text,
            } => self.emit(Event::Diag(Diag {
                node_id: node.as_ref().map(node_id),
                ip: match address {
                    SocketAddr::V4(a) => *a.ip(),
                    SocketAddr::V6(_) => Ipv4Addr::UNSPECIFIED,
                },
                priority: convert::diag_priority(priority),
                logical_port,
                text,
            })),
            net::Event::FlashProgress { progress } => {
                let mut jobs = lock(&self.jobs);
                if let Some(job) = jobs.on_progress(&progress, Instant::now()) {
                    self.emit(Event::FlashProgress { job });
                }
            }
        }
    }

    /// A queued message fetched by the library → `rdmQueued` with the decoded value.
    fn on_queued(
        &self,
        lib_id: &net::NodeId,
        pa: PortAddress,
        uid: Uid,
        message: net::QueuedMessage,
    ) {
        let id = node_id(lib_id);
        let port = self
            .net()
            .nodes
            .get(&id)
            .and_then(|e| ports_for(&e.state, pa).into_iter().next());
        let Some(port) = port else {
            tracing::debug!("queued message from {id} for unknown Port-Address {pa}");
            return;
        };
        let uid = uid_to_bridge(uid);
        let value = if message.command_class == cc::GET_RESPONSE || !message.data.is_empty() {
            let desc = lock(&self.rdm).descriptions.get(uid, message.pid);
            values::decode(message.pid, &message.data, uid, desc.as_ref())
        } else {
            None
        };
        self.emit(Event::RdmQueued(RdmQueued {
            node_id: id,
            port,
            uid,
            sub_device: message.sub_device,
            pid: message.pid,
            data: HexData(message.data),
            value,
        }));
    }

    /// The library's event stream lagged: re-read its state.
    fn resync(&self, ctrl: &Controller, lost: u64) {
        self.sync_from(ctrl);
        let jobs = lock(&self.jobs).resync(Instant::now());
        for job in jobs {
            self.emit(Event::FlashProgress { job });
        }
        self.log(
            LogLevel::Warn,
            format!("Art-Net event stream overflowed ({lost} events lost); state re-read"),
        );
    }

    // ── Socket and interfaces ─────────────────────────────────────────────────────────────────────────────────

    /// Re-reads the interfaces; emits `bridgeStatus` when the shown list changed.
    fn refresh_interfaces(&self) {
        let all = net::controller::interfaces();
        let shown = convert::visible_interfaces(&all, self.bind_ip());
        let mut st = self.net();
        st.all_interfaces = all;
        if st.status.interfaces != shown {
            st.status.interfaces = shown;
            self.emit(Event::BridgeStatus(st.status.clone()));
        }
    }

    /// The socket could not be bound.
    fn set_unbound(&self, e: &net::Error) {
        let error = Some(bind_error_text(e));
        let changed = {
            let mut st = self.net();
            let changed = st.status.udp.error != error;
            st.status.udp = UdpState {
                bound: false,
                port: self.cfg.controller.bind.port(),
                error,
            };
            if changed {
                self.emit(Event::BridgeStatus(st.status.clone()));
            }
            changed
        };
        if changed {
            self.log(
                LogLevel::Error,
                format!(
                    "Art-Net socket {} not bound: {} (retrying every {} s)",
                    self.cfg.controller.bind,
                    bind_error_text(e),
                    self.cfg.bind_retry.as_secs()
                ),
            );
        }
    }

    /// Takes a bound controller into use.
    fn attach(self: &Arc<Self>, ctrl: Controller) {
        let events = ctrl.subscribe();
        let targets = self.net().poll_targets.clone();
        ctrl.set_poll_targets(&targets);
        *self.ctrl.write().unwrap_or_else(|e| e.into_inner()) = Some(ctrl.clone());
        {
            let mut st = self.net();
            st.status.udp = UdpState {
                bound: true,
                port: ctrl.local_addr().port(),
                error: None,
            };
            self.emit(Event::BridgeStatus(st.status.clone()));
        }
        tracing::info!("Art-Net socket bound to {}", ctrl.local_addr());
        self.spawn(pump(Arc::downgrade(self), events, ctrl.clone()));
        // Replies that arrived before the subscription.
        self.sync_from(&ctrl);
    }
}

/// Contract ports of `node` that use Port-Address `pa` (outputs first; inputs only when no output has it).
fn ports_for(node: &NodeState, pa: PortAddress) -> Vec<PortRef> {
    let outputs: Vec<PortRef> = node
        .ports
        .iter()
        .filter(|p| p.output_port_address == pa.raw() && p.direction != PortDirection::Input)
        .map(|p| p.port)
        .collect();
    if !outputs.is_empty() {
        return outputs;
    }
    node.ports
        .iter()
        .filter(|p| p.output_port_address == pa.raw())
        .map(|p| p.port)
        .collect()
}

// ── Background tasks ──────────────────────────────────────────────────────────────────────────────────────────────

/// Forwards the library's events into the adapter until the controller or the backend is gone.
async fn pump(weak: Weak<Inner>, mut events: broadcast::Receiver<net::Event>, ctrl: Controller) {
    loop {
        let ev = events.recv().await;
        let Some(inner) = weak.upgrade() else {
            return;
        };
        match ev {
            Ok(e) => inner.on_net_event(e),
            Err(RecvError::Lagged(n)) => inner.resync(&ctrl, n),
            Err(RecvError::Closed) => return,
        }
    }
}

/// Re-reads the interfaces periodically.
async fn watch_interfaces(weak: Weak<Inner>, period: Duration) {
    loop {
        tokio::time::sleep(period).await;
        let Some(inner) = weak.upgrade() else {
            return;
        };
        inner.refresh_interfaces();
    }
}

/// Retries binding the socket until it works.
async fn bind_loop(weak: Weak<Inner>, period: Duration) {
    loop {
        tokio::time::sleep(period).await;
        let Some(inner) = weak.upgrade() else {
            return;
        };
        match Controller::bind(inner.cfg.controller.clone()).await {
            Ok(ctrl) => {
                inner.attach(ctrl);
                return;
            }
            Err(e) => inner.set_unbound(&e),
        }
    }
}

/// Re-requests a TOD and emits it when the library will not (an answer equal to the TOD it already had).
async fn refresh_tod(
    weak: Weak<Inner>,
    ctrl: Controller,
    lib_id: net::NodeId,
    pa: PortAddress,
    timeout: Duration,
) {
    let before = ctrl.tod(&lib_id, pa);
    match ctrl.tod_refresh(&lib_id, pa, timeout).await {
        // An empty answer may be a TodNak, which the library reports itself (`nak: true`).
        Ok(uids) if !uids.is_empty() && before.as_ref() == Some(&uids) => {
            if let Some(inner) = weak.upgrade() {
                inner.apply_tod(&lib_id, pa, &uids, false);
            }
        }
        Ok(_) => {}
        Err(e) => tracing::debug!("ArtTodData for {pa} of {lib_id} not received: {e}"),
    }
}

/// After AtcFlush: waits for the node's new TOD; when the library did not report one (unchanged), re-requests it.
async fn settle_flush(
    weak: Weak<Inner>,
    ctrl: Controller,
    mut events: broadcast::Receiver<net::Event>,
    lib_id: net::NodeId,
    pa: PortAddress,
    settle: Duration,
    timeout: Duration,
) {
    let reported = tokio::time::timeout(settle, async {
        loop {
            match events.recv().await {
                Ok(net::Event::TodChanged {
                    node, port_address, ..
                }) if node == lib_id && port_address == pa => return true,
                Ok(_) | Err(RecvError::Lagged(_)) => {}
                Err(RecvError::Closed) => return false,
            }
        }
    })
    .await
    .unwrap_or(false);
    if !reported {
        refresh_tod(weak, ctrl, lib_id, pa, timeout).await;
    }
}

// ── Backend ───────────────────────────────────────────────────────────────────────────────────────────────────────

/// The real Art-Net 4 / RDM backend. Cheap to clone; clones share the controller and all state.
#[derive(Clone)]
pub struct PixelmapperNetBackend {
    inner: Arc<Inner>,
}

impl std::fmt::Debug for PixelmapperNetBackend {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("PixelmapperNetBackend")
            .field("bind", &self.inner.cfg.controller.bind)
            .field("status", &self.inner.net().status)
            .finish()
    }
}

impl PixelmapperNetBackend {
    /// Binds the Art-Net socket and starts discovery. Never fails: when the socket cannot be bound, the backend
    /// reports `udp.bound = false` with the reason and retries every [`BackendConfig::bind_retry`].
    ///
    /// `controller.auto_tod_request` is switched off: the library requests TODs only for the ports of a node's
    /// first ArtPollReply page, so the adapter sends the ArtTodRequests per port itself.
    pub async fn start(mut cfg: BackendConfig) -> Self {
        cfg.controller.auto_tod_request = false;
        let inner = Arc::new(Inner {
            ctrl: RwLock::new(None),
            events: broadcast::channel(1024).0,
            net: Mutex::new(NetState {
                status: BridgeStatus {
                    interfaces: Vec::new(),
                    udp: UdpState {
                        bound: false,
                        port: cfg.controller.bind.port(),
                        error: None,
                    },
                },
                all_interfaces: Vec::new(),
                nodes: BTreeMap::new(),
                tods: BTreeMap::new(),
                poll_targets: Vec::new(),
            }),
            rdm: Mutex::new(RdmState::default()),
            jobs: Mutex::new(flash::Jobs::default()),
            tasks: Mutex::new(Vec::new()),
            cfg,
        });
        inner.refresh_interfaces();
        match Controller::bind(inner.cfg.controller.clone()).await {
            Ok(ctrl) => inner.attach(ctrl),
            Err(e) => {
                inner.set_unbound(&e);
                inner.spawn(bind_loop(Arc::downgrade(&inner), inner.cfg.bind_retry));
            }
        }
        inner.spawn(watch_interfaces(
            Arc::downgrade(&inner),
            inner.cfg.interface_poll,
        ));
        Self { inner }
    }

    /// Stops the background tasks and the controller (running flash jobs end).
    pub fn shutdown(&self) {
        for t in lock(&self.inner.tasks).drain(..) {
            t.abort();
        }
        if let Some(ctrl) = self
            .inner
            .ctrl
            .write()
            .unwrap_or_else(|e| e.into_inner())
            .take()
        {
            ctrl.shutdown();
        }
    }

    /// Local address of the Art-Net socket, `None` while unbound.
    pub fn local_addr(&self) -> Option<SocketAddr> {
        self.inner.ctrl().ok().map(|c| c.local_addr())
    }

    /// RDM GET/SET.
    async fn rdm(&self, req: RdmRequest, set: bool) -> Result<RdmResponse, BackendError> {
        let a = &req.address;
        let (ctrl, lib_id, pa) = self.inner.resolve_port(&a.node_id, a.port)?;
        let desc = lock(&self.inner.rdm).descriptions.get(a.uid, req.pid);
        let data = match &req.payload {
            RdmPayload::Empty => Vec::new(),
            RdmPayload::Data(d) => d.clone(),
            RdmPayload::Value(v) => values::encode(req.pid, set, v, desc.as_ref())?,
        };
        let request = net::RdmRequest {
            uid: uid_to_net(a.uid),
            command_class: if set { cc::SET } else { cc::GET },
            pid: req.pid,
            data,
            options: RdmOptions {
                sub_device: a.sub_device,
                ..RdmOptions::default()
            },
        };
        let data = ctrl
            .rdm(&lib_id, pa, request)
            .await
            .map_err(|e| backend_error(e, &a.node_id, Some(a.port)))?;
        if a.uid.is_broadcast() {
            return Ok(RdmResponse {
                data: Vec::new(),
                value: None,
                broadcast: true,
            });
        }
        if !set && req.pid == pid::PARAMETER_DESCRIPTION {
            // The bridge now knows this parameter of the device (value decoding).
            if let Ok(d) = ParameterDescription::decode(&data) {
                lock(&self.inner.rdm).descriptions.insert(a.uid, d);
            }
        }
        let value = if set && data.is_empty() {
            None
        } else {
            values::decode(req.pid, &data, a.uid, desc.as_ref())
        };
        Ok(RdmResponse {
            data,
            value,
            broadcast: false,
        })
    }

    /// Sends a configuration packet and waits for the node's reply: 1 s, one resend, then `timeout`.
    async fn send_with_resend<F, Fut>(
        &self,
        node: &NodeId,
        mut send: F,
    ) -> Result<net::NodeState, BackendError>
    where
        F: FnMut() -> Fut,
        Fut: Future<Output = net::Result<Option<net::NodeState>>>,
    {
        for _ in 0..2 {
            match send().await {
                Ok(Some(state)) => return Ok(state),
                Ok(None) => continue,
                Err(e) => return Err(backend_error(e, node, None)),
            }
        }
        Err(BackendError::Timeout)
    }
}

impl NetBackend for PixelmapperNetBackend {
    fn status(&self) -> BridgeStatus {
        self.inner.net().status.clone()
    }

    fn snapshot(&self) -> Snapshot {
        let st = self.inner.net();
        Snapshot {
            nodes: st.nodes.values().map(|e| e.state.clone()).collect(),
            tods: st.tods.values().cloned().collect(),
        }
    }

    fn subscribe(&self) -> broadcast::Receiver<Event> {
        self.inner.events.subscribe()
    }

    fn poll_now(&self) -> Result<(), BackendError> {
        let ctrl = self.inner.ctrl()?;
        tokio::spawn(async move {
            if let Err(e) = ctrl.poll_now().await {
                tracing::warn!("ArtPoll not sent: {e}");
            }
        });
        Ok(())
    }

    fn set_poll_targets(&self, targets: &[Ipv4Addr]) {
        self.inner.net().poll_targets = targets.to_vec();
        if let Ok(ctrl) = self.inner.ctrl() {
            ctrl.set_poll_targets(targets);
        }
    }

    async fn tod_request(&self, node: NodeId, port: PortRef) -> Result<(), BackendError> {
        let (ctrl, lib_id, pa) = self.inner.resolve_port(&node, port)?;
        let timeout = self.inner.cfg.tod_timeout;
        self.inner.spawn(refresh_tod(
            Arc::downgrade(&self.inner),
            ctrl,
            lib_id,
            pa,
            timeout,
        ));
        Ok(())
    }

    async fn tod_control(
        &self,
        node: NodeId,
        port: PortRef,
        action: TodAction,
    ) -> Result<(), BackendError> {
        let (ctrl, lib_id, pa) = self.inner.resolve_port(&node, port)?;
        let command = match action {
            TodAction::Flush => ArtTodControl::ATC_FLUSH,
            TodAction::End => ArtTodControl::ATC_END,
            TodAction::IncOn => ArtTodControl::ATC_INC_ON,
            TodAction::IncOff => ArtTodControl::ATC_INC_OFF,
        };
        let events = ctrl.subscribe();
        ctrl.tod_control(&lib_id, pa, command)
            .await
            .map_err(|e| backend_error(e, &node, Some(port)))?;
        if action == TodAction::Flush {
            let cfg = &self.inner.cfg;
            self.inner.spawn(settle_flush(
                Arc::downgrade(&self.inner),
                ctrl,
                events,
                lib_id,
                pa,
                cfg.tod_settle,
                cfg.tod_timeout,
            ));
        }
        Ok(())
    }

    async fn rdm_get(&self, req: RdmRequest) -> Result<RdmResponse, BackendError> {
        self.rdm(req, false).await
    }

    async fn rdm_set(&self, req: RdmRequest) -> Result<RdmResponse, BackendError> {
        self.rdm(req, true).await
    }

    async fn rdm_describe(
        &self,
        address: RdmAddress,
        refresh: bool,
    ) -> Result<RdmDeviceDescription, BackendError> {
        let (ctrl, lib_id, pa) = self.inner.resolve_port(&address.node_id, address.port)?;
        let key = (address.uid, address.sub_device);
        let cached = if refresh {
            None
        } else {
            lock(&self.inner.rdm).described.get(&key).cloned()
        };
        let target = describe::Target {
            ctrl: &ctrl,
            node: lib_id,
            pa,
            uid: address.uid,
            sub_device: address.sub_device,
            node_id: &address.node_id,
            port: address.port,
        };
        let (description, keep) = describe::describe(&target, cached).await?;
        let mut rdm = lock(&self.inner.rdm);
        for d in &keep.parameter_descriptions {
            rdm.descriptions.insert(address.uid, d.clone());
        }
        rdm.described.insert(key, keep);
        Ok(description)
    }

    async fn art_address(&self, req: ArtAddressArgs) -> Result<NodeState, BackendError> {
        let (ctrl, lib_id) = self.inner.resolve(&req.node_id)?;
        let node = self
            .inner
            .net()
            .nodes
            .get(&req.node_id)
            .map(|e| e.state.clone())
            .ok_or_else(|| BackendError::UnknownNode(req.node_id.clone()))?;
        let mut last = None;
        for packet in address::plan(&req, &node) {
            let state = self
                .send_with_resend(&req.node_id, || ctrl.art_address(&lib_id, packet.clone()))
                .await?;
            last = Some(state);
        }
        // Nothing to change: the current state. Else the library's latest state (at least as new as the reply;
        // the event pump may have seen newer replies meanwhile).
        Ok(match last {
            Some(state) => self
                .inner
                .apply_node(&ctrl.node(&lib_id).unwrap_or(state)),
            None => node,
        })
    }

    async fn ip_prog(&self, req: IpProgArgs) -> Result<IpProgReply, BackendError> {
        let (ctrl, lib_id) = self.inner.resolve(&req.node_id)?;
        let has_static = req.ip.is_some() || req.mask.is_some() || req.gateway.is_some();
        if req.dhcp == Some(true) && (has_static || req.reset_to_default == Some(true)) {
            return Err(BackendError::InvalidArgument {
                field: "dhcp".into(),
                message: "dhcp: true cannot be combined with ip, mask, gateway or resetToDefault (Art-Net \
                          ignores them when DHCP is switched on)"
                    .into(),
            });
        }
        if req.dhcp == Some(false) && req.ip.is_none() {
            return Err(BackendError::InvalidArgument {
                field: "dhcp".into(),
                message:
                    "dhcp: false needs a static ip (Art-Net switches DHCP off by programming an IP)"
                        .into(),
            });
        }
        let change = IpProgChange {
            dhcp: req.dhcp == Some(true),
            ip: req.ip,
            mask: req.mask,
            gateway: req.gateway,
            reset: req.reset_to_default == Some(true),
        };
        let mut result = Err(BackendError::Timeout);
        for _ in 0..2 {
            result = ctrl
                .ip_prog_change(&lib_id, &change)
                .await
                .map_err(|e| backend_error(e, &req.node_id, None));
            if result != Err(BackendError::Timeout) {
                break;
            }
        }
        let r = result?;
        Ok(IpProgReply {
            ip: r.prog_ip,
            mask: r.prog_sm,
            gateway: r.prog_dg,
            dhcp: r.dhcp_enabled(),
        })
    }

    async fn art_input(
        &self,
        node: NodeId,
        bind: u8,
        disabled: Vec<bool>,
    ) -> Result<NodeState, BackendError> {
        let (ctrl, lib_id) = self.inner.resolve(&node)?;
        let state = self
            .send_with_resend(&node, || ctrl.art_input(&lib_id, bind, &disabled))
            .await?;
        Ok(self
            .inner
            .apply_node(&ctrl.node(&lib_id).unwrap_or(state)))
    }

    fn image_info(&self, hex: &str) -> Result<FirmwareImageInfo, BackendError> {
        flash::parse_image(hex).map(|image| flash::image_info(&image))
    }

    async fn flash_start(&self, req: FlashRequest) -> Result<FlashJob, BackendError> {
        let a = &req.address;
        let (ctrl, lib_id, pa) = self.inner.resolve_port(&a.node_id, a.port)?;
        let image = flash::parse_image(&req.hex)?;
        lock(&self.inner.jobs).reserve(&a.node_id, a.port)?;
        let _reservation = Reservation(&self.inner, &a.node_id, a.port);
        let target = describe::Target {
            ctrl: &ctrl,
            node: lib_id,
            pa,
            uid: a.uid,
            sub_device: a.sub_device,
            node_id: &a.node_id,
            port: a.port,
        };
        flash::precheck(&target, &image, req.allow_downgrade).await?;
        let opts = TubeFlashOptions {
            allow_downgrade: req.allow_downgrade,
            rescue: None,
            ..self.inner.cfg.tube_flash.clone()
        };
        let template = FlashJob {
            job_id: String::new(),
            node_id: a.node_id.clone(),
            port: a.port,
            uid: a.uid,
            phase: FlashPhase::Entering,
            pages_done: 0,
            pages_total: image.page_plan(true).len() as u32,
            rewrites: 0,
            in_bootloader: false,
            image: flash::image_info(&image),
            error: None,
            result: None,
        };
        let job = ctrl
            .flash_tube_with(&lib_id, pa, uid_to_net(a.uid), image, opts)
            .map_err(|e| backend_error(e, &a.node_id, Some(a.port)))?;
        let (number, contract_job) = {
            let mut jobs = lock(&self.inner.jobs);
            let (n, j) = jobs.insert(template, &job);
            self.inner.emit(Event::FlashProgress { job: j.clone() });
            (n, j)
        };
        let weak = Arc::downgrade(&self.inner);
        self.inner.spawn(async move {
            let result = job.wait().await;
            if let Some(inner) = weak.upgrade() {
                let mut jobs = lock(&inner.jobs);
                if let Some(job) = jobs.finish(number, result, Instant::now()) {
                    inner.emit(Event::FlashProgress { job });
                }
            }
        });
        Ok(contract_job)
    }

    fn flash_cancel(&self, job_id: &str) -> Result<(), BackendError> {
        lock(&self.inner.jobs).cancel(job_id)
    }

    fn flash_jobs(&self) -> Vec<FlashJob> {
        lock(&self.inner.jobs).list(Instant::now())
    }

    fn send_dmx(&self, frames: &[DmxFrame], sync: bool, physical: u8) -> Result<(), BackendError> {
        let ctrl = self.inner.ctrl()?;
        let node_port = self.inner.cfg.controller.node_port;
        let dests: Vec<Option<SocketAddr>> = {
            let st = self.inner.net();
            frames
                .iter()
                .map(|f| match &f.target {
                    DmxTarget::Node(id) => st.nodes.get(id).map(|e| e.address),
                    DmxTarget::Ip(ip) => Some(SocketAddr::from((*ip, node_port))),
                })
                .collect()
        };
        let mut sent = false;
        for (frame, dest) in frames.iter().zip(dests) {
            let (Some(dest), Some(pa)) = (dest, PortAddress::new(frame.port_address)) else {
                continue;
            };
            match ctrl.try_send_dmx(dest, pa, &frame.data, physical) {
                Ok(()) => sent = true,
                // A full socket buffer drops the frame; the next set follows anyway.
                Err(e) => tracing::trace!("ArtDmx to {dest} dropped: {e}"),
            }
        }
        if sync && sent {
            ctrl.try_send_sync();
        }
        Ok(())
    }
}

/// A (node, port) reserved for a firmware job that is being started; released when dropped (a started job keeps
/// the port busy itself).
struct Reservation<'a>(&'a Inner, &'a NodeId, PortRef);

impl Drop for Reservation<'_> {
    fn drop(&mut self) {
        lock(&self.0.jobs).release(self.1, self.2);
    }
}
