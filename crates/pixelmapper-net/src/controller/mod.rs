//! The async Art-Net 4 / RDM controller: discovery, node state, TOD, RDM with queueing/retry, node
//! configuration, DMX and firmware jobs (`protocol/contract.md` §2–§10).
//!
//! ```no_run
//! # async fn demo() -> pixelmapper_net::Result<()> {
//! use pixelmapper_net::controller::{Controller, ControllerConfig};
//! let ctrl = Controller::bind(ControllerConfig::default()).await?;
//! let mut events = ctrl.subscribe();
//! ctrl.poll_now().await?;
//! while let Ok(ev) = events.recv().await {
//!     println!("{}", serde_json::to_string(&ev).unwrap());
//! }
//! # Ok(()) }
//! ```
//!
//! One UDP socket (default `0.0.0.0:6454`, SO_REUSEADDR/SO_REUSEPORT) is used for everything. Nodes are
//! addressed by the UDP source address of their ArtPollReply (always port 6454 for real nodes; the simulator
//! uses ephemeral ports), merged per node over its RDM UID and addressed per port via Port-Address/BindIndex.

mod describe;
mod link;
mod net;
mod rdm_engine;

pub use describe::{DeviceDescription, SensorDescription, SlotDescriptionEntry};
pub use link::{ControllerNodeLink, ControllerPortLink};
pub use net::{default_broadcast_targets, interfaces, Interface};

use crate::artnet::{
    ArtAddress, ArtDmx, ArtFirmwareMaster, ArtFirmwareReply, ArtInput, ArtIpProg, ArtIpProgReply,
    ArtPacket, ArtPoll, ArtPollReply, ArtSync, ArtTodControl, ArtTodData, ArtTodRequest,
    IpProgChange, PortAddress, TOD_NAK,
};
use crate::error::{Error, Result};
use crate::event::Event;
use crate::flash::node::{flash_node, NodeFlashOptions};
use crate::flash::tube::{flash_tube, RescueTarget, TubeFlashOptions};
use crate::flash::{FlashJob, FlashProgress};
use crate::image::{NodeImage, TubeImage};
use crate::link::RdmOptions;
use crate::pids;
use crate::rdm::{cc, Uid};
use crate::state::{
    merge_replies, reply_node_id, report_counter, report_counter_restarted, NodeId, NodeState,
    PortDirection,
};
use rdm_engine::RdmEngine;
use std::collections::{BTreeMap, BTreeSet, HashMap};
use std::net::{Ipv4Addr, SocketAddr, SocketAddrV4};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex, MutexGuard, Weak};
use std::time::{Duration, Instant};
use tokio::net::UdpSocket;
use tokio::sync::{broadcast, oneshot};
use tokio::task::JoinHandle;

/// RDM transport settings.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RdmConfig {
    /// Timeout per attempt (contract §6.2: 500 ms).
    pub timeout: Duration,
    /// Repetitions after the first attempt without answer (2).
    pub retries: u32,
    /// Maximum requests in flight per node port (4); further limited by the node's FifoMax/FifoAvail.
    pub max_inflight_per_port: usize,
    /// Give up resolving an ACK_TIMER after this time.
    pub ack_timer_limit: Duration,
    /// Maximum ACK_OVERFLOW continuations of one request.
    pub max_overflow: u32,
    /// Fetch queued messages automatically when a response reports Message Count > 0.
    pub auto_fetch_queued: bool,
    /// Interval for fetching the queued messages of every node's own responder even without other RDM traffic
    /// (changes of RDM-only settings by other clients or at the panel, contract §7). `ZERO` disables.
    pub node_queue_interval: Duration,
}

impl Default for RdmConfig {
    fn default() -> Self {
        Self {
            timeout: Duration::from_millis(500),
            retries: 2,
            max_inflight_per_port: 4,
            ack_timer_limit: Duration::from_secs(10),
            max_overflow: 64,
            auto_fetch_queued: true,
            node_queue_interval: Duration::from_secs(2),
        }
    }
}

/// Controller settings. All ports/addresses are configurable so tests (and several programs) can run in
/// parallel on one machine.
#[derive(Debug, Clone)]
pub struct ControllerConfig {
    /// Local address to bind (default `0.0.0.0:6454`; `127.0.0.1:0` in tests).
    pub bind: SocketAddr,
    /// Set SO_REUSEPORT (default true; on macOS unicast then goes to only one of the sockets).
    pub reuse_port: bool,
    /// ArtPoll/ArtSync destinations; empty = [`default_broadcast_targets`] with port `node_port`.
    pub broadcast: Vec<SocketAddr>,
    /// Destination port for broadcasts (6454).
    pub node_port: u16,
    /// ArtPoll interval (2.5 s; spec: 2.5–3 s). `Duration::ZERO` disables periodic polling.
    pub poll_interval: Duration,
    /// A node (or one of its BindIndex pages) is removed after this time without ArtPollReply (10 s).
    pub node_timeout: Duration,
    /// Ask nodes for diagnostics (ArtPoll flags bit 2, unicast).
    pub diagnostics: bool,
    /// Lowest diagnostics priority wanted.
    pub diag_priority: u8,
    /// Answer ArtPoll with an own ArtPollReply (spec requirement for controllers).
    pub reply_to_polls: bool,
    /// Short name in the own ArtPollReply.
    pub short_name: String,
    /// Long name in the own ArtPollReply.
    pub long_name: String,
    /// Our RDM controller UID (source UID of requests).
    pub uid: Uid,
    /// Send ArtTodRequest for every RDM output port when a node is discovered (subscribes to unsolicited
    /// ArtTodData, contract §6.1).
    pub auto_tod_request: bool,
    /// ArtDmx `Physical` (kept constant, contract §4).
    pub physical: u8,
    /// How long configuration calls wait for the node's answer (ArtPollReply / ArtIpProgReply).
    pub reply_timeout: Duration,
    /// RDM settings.
    pub rdm: RdmConfig,
}

impl Default for ControllerConfig {
    fn default() -> Self {
        Self {
            bind: SocketAddr::V4(SocketAddrV4::new(
                Ipv4Addr::UNSPECIFIED,
                crate::artnet::PORT,
            )),
            reuse_port: true,
            broadcast: Vec::new(),
            node_port: crate::artnet::PORT,
            poll_interval: Duration::from_millis(2500),
            node_timeout: Duration::from_secs(10),
            diagnostics: false,
            diag_priority: ArtPoll::DP_LOW,
            reply_to_polls: true,
            short_name: "Pixelmapper".into(),
            long_name: "Pixelmapper Art-Net controller".into(),
            uid: Uid::new(pids::ESTA_MANUFACTURER, 0x0000_0002),
            auto_tod_request: true,
            physical: 0,
            reply_timeout: Duration::from_millis(1500),
            rdm: RdmConfig::default(),
        }
    }
}

impl ControllerConfig {
    /// Config for talking to nodes on localhost (simulator): bind `127.0.0.1:0`, poll `targets`.
    pub fn local(targets: Vec<SocketAddr>) -> Self {
        Self {
            bind: "127.0.0.1:0"
                .parse()
                .unwrap_or(SocketAddr::V4(SocketAddrV4::new(Ipv4Addr::LOCALHOST, 0))),
            reuse_port: false,
            broadcast: targets,
            ..Self::default()
        }
    }
}

/// An RDM request with all options.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RdmRequest {
    /// Destination UID.
    pub uid: Uid,
    /// `cc::GET` or `cc::SET`.
    pub command_class: u8,
    /// PID.
    pub pid: u16,
    /// Parameter data.
    pub data: Vec<u8>,
    /// Options (sub-device, timeout, retries, BUFFER_FULL handling).
    pub options: RdmOptions,
}

struct BindEntry {
    id: NodeId,
    reply: ArtPollReply,
    last_seen: Instant,
}

struct NodeEntry {
    state: NodeState,
}

#[derive(Default)]
struct TodAssembly {
    total: u16,
    blocks: BTreeMap<u8, Vec<Uid>>,
}

type TodKey = (NodeId, u16);

#[derive(Default)]
struct State {
    binds: HashMap<(SocketAddr, u8), BindEntry>,
    nodes: HashMap<NodeId, NodeEntry>,
    tods: HashMap<(NodeId, u16), Vec<Uid>>,
    tod_assembly: HashMap<(SocketAddr, u16), TodAssembly>,
    dmx_seq: HashMap<u16, u8>,
    /// Sequence per (destination, Port-Address) for [`Controller::try_send_dmx`].
    dmx_seq_to: HashMap<(SocketAddr, u16), u8>,
    /// Extra unicast ArtPoll destinations ([`Controller::set_poll_targets`]).
    poll_targets: Vec<SocketAddr>,
    reply_waiters: Vec<(SocketAddr, oneshot::Sender<()>)>,
    ipprog_waiters: Vec<(SocketAddr, oneshot::Sender<ArtIpProgReply>)>,
    fw_waiters: Vec<(SocketAddr, oneshot::Sender<ArtFirmwareReply>)>,
    tod_waiters: Vec<(TodKey, oneshot::Sender<Vec<Uid>>)>,
    /// Last NodeReport counter per node (restart detection, contract §6.1).
    report_counts: HashMap<NodeId, u16>,
}

pub(crate) struct Inner {
    cfg: ControllerConfig,
    socket: Arc<UdpSocket>,
    local: SocketAddr,
    broadcast: Vec<SocketAddr>,
    events: broadcast::Sender<Event>,
    state: Mutex<State>,
    rdm: RdmEngine,
    jobs: AtomicU64,
    tasks: Mutex<Vec<JoinHandle<()>>>,
}

/// Handle to a running controller. Cheap to clone; all clones share one socket and state.
/// Call [`Controller::shutdown`] to stop the background tasks.
#[derive(Clone)]
pub struct Controller {
    inner: Arc<Inner>,
}

impl Controller {
    /// Bind the socket and start the receive and poll tasks.
    pub async fn bind(cfg: ControllerConfig) -> Result<Self> {
        let socket = Arc::new(net::bind_socket(cfg.bind, cfg.reuse_port)?);
        let local = socket.local_addr()?;
        let broadcast = if cfg.broadcast.is_empty() {
            let ip = match cfg.bind {
                SocketAddr::V4(a) => *a.ip(),
                SocketAddr::V6(_) => Ipv4Addr::UNSPECIFIED,
            };
            default_broadcast_targets(ip, cfg.node_port)
        } else {
            cfg.broadcast.clone()
        };
        let (events, _) = broadcast::channel(4096);
        let inner = Arc::new(Inner {
            rdm: RdmEngine::new(cfg.uid, cfg.rdm.clone()),
            cfg,
            socket: socket.clone(),
            local,
            broadcast,
            events,
            state: Mutex::new(State::default()),
            jobs: AtomicU64::new(1),
            tasks: Mutex::new(Vec::new()),
        });
        let weak = Arc::downgrade(&inner);
        let rx = tokio::spawn(recv_loop(socket, weak.clone()));
        let poll = tokio::spawn(poll_loop(weak));
        lock(&inner.tasks).extend([rx, poll]);
        Ok(Self { inner })
    }

    /// Local socket address.
    pub fn local_addr(&self) -> SocketAddr {
        self.inner.local
    }

    /// Configuration in use.
    pub fn config(&self) -> &ControllerConfig {
        &self.inner.cfg
    }

    /// ArtPoll/ArtSync destinations in use.
    pub fn broadcast_targets(&self) -> &[SocketAddr] {
        &self.inner.broadcast
    }

    /// Subscribe to events. Lagging receivers lose the oldest events (`RecvError::Lagged`).
    pub fn subscribe(&self) -> broadcast::Receiver<Event> {
        self.inner.events.subscribe()
    }

    /// Stop the background tasks. Pending requests fail with [`Error::Closed`] or time out.
    pub fn shutdown(&self) {
        for t in lock(&self.inner.tasks).drain(..) {
            t.abort();
        }
        self.inner.rdm.close();
    }

    /// Send an ArtPoll now (in addition to the periodic ones).
    pub async fn poll_now(&self) -> Result<()> {
        self.inner.send_poll().await
    }

    /// Replace the set of extra unicast ArtPoll destinations (port [`ControllerConfig::node_port`]). Every ArtPoll
    /// (periodic and [`poll_now`](Self::poll_now)) also goes to each of them, e.g. nodes in other subnets.
    pub fn set_poll_targets(&self, targets: &[Ipv4Addr]) {
        let port = self.inner.cfg.node_port;
        let mut t: Vec<SocketAddr> = targets
            .iter()
            .map(|ip| SocketAddr::V4(SocketAddrV4::new(*ip, port)))
            .collect();
        t.sort();
        t.dedup();
        lock(&self.inner.state).poll_targets = t;
    }

    /// The extra unicast ArtPoll destinations.
    pub fn poll_targets(&self) -> Vec<SocketAddr> {
        lock(&self.inner.state).poll_targets.clone()
    }

    /// All known nodes, sorted by id.
    pub fn nodes(&self) -> Vec<NodeState> {
        let st = lock(&self.inner.state);
        let mut v: Vec<NodeState> = st.nodes.values().map(|n| n.state.clone()).collect();
        v.sort_by_key(|n| n.id);
        v
    }

    /// One node.
    pub fn node(&self, id: &NodeId) -> Option<NodeState> {
        lock(&self.inner.state)
            .nodes
            .get(id)
            .map(|n| n.state.clone())
    }

    /// Wait until a node with this id is known (polling meanwhile).
    pub async fn wait_for_node(&self, id: &NodeId, timeout: Duration) -> Result<NodeState> {
        let deadline = Instant::now() + timeout;
        loop {
            if let Some(n) = self.node(id) {
                return Ok(n);
            }
            if Instant::now() >= deadline {
                return Err(Error::WaitTimeout(timeout, "node"));
            }
            self.poll_now().await?;
            tokio::time::sleep(Duration::from_millis(100)).await;
        }
    }

    /// Last complete TOD of a node port (sorted), if one was received.
    pub fn tod(&self, node: &NodeId, port_address: PortAddress) -> Option<Vec<Uid>> {
        lock(&self.inner.state)
            .tods
            .get(&(*node, port_address.raw()))
            .cloned()
    }

    fn node_or_err(&self, id: &NodeId) -> Result<NodeState> {
        self.node(id)
            .ok_or_else(|| Error::UnknownNode(id.to_string()))
    }

    fn port_check(&self, node: &NodeState, pa: PortAddress) -> Result<()> {
        if node.port(pa).is_none() {
            return Err(Error::UnknownPort {
                node: node.id.to_string(),
                port_address: pa.to_string(),
            });
        }
        Ok(())
    }

    // ---------------------------------------------------------------- RDM

    /// RDM GET; returns the parameter data of the ACK (ACK_TIMER/ACK_OVERFLOW resolved).
    pub async fn rdm_get(
        &self,
        node: &NodeId,
        port_address: PortAddress,
        uid: Uid,
        pid: u16,
        data: &[u8],
    ) -> Result<Vec<u8>> {
        self.rdm(
            node,
            port_address,
            RdmRequest {
                uid,
                command_class: cc::GET,
                pid,
                data: data.to_vec(),
                options: RdmOptions::default(),
            },
        )
        .await
    }

    /// RDM SET; returns the parameter data of the ACK. To a broadcast/vendorcast UID: sent once, `Ok(vec![])`.
    pub async fn rdm_set(
        &self,
        node: &NodeId,
        port_address: PortAddress,
        uid: Uid,
        pid: u16,
        data: &[u8],
    ) -> Result<Vec<u8>> {
        self.rdm(
            node,
            port_address,
            RdmRequest {
                uid,
                command_class: cc::SET,
                pid,
                data: data.to_vec(),
                options: RdmOptions::default(),
            },
        )
        .await
    }

    /// RDM request with all options. Requests to one node port are queued (at most
    /// [`RdmConfig::max_inflight_per_port`] in flight, fewer if the node reports a smaller FifoMax), requests to
    /// different ports run in parallel.
    pub async fn rdm(
        &self,
        node: &NodeId,
        port_address: PortAddress,
        req: RdmRequest,
    ) -> Result<Vec<u8>> {
        let n = self.node_or_err(node)?;
        self.port_check(&n, port_address)?;
        rdm_engine::request(&self.inner, n.id, n.address, port_address, req).await
    }

    /// Read everything needed to show a device: DEVICE_INFO, labels, personalities, slots, sensors,
    /// SUPPORTED_PARAMETERS and the PARAMETER_DESCRIPTION of every manufacturer PID.
    pub async fn describe_device(
        &self,
        node: &NodeId,
        port_address: PortAddress,
        uid: Uid,
    ) -> Result<DeviceDescription> {
        describe::describe(self, node, port_address, uid).await
    }

    /// Fetch the queued messages of a device now (also done automatically on Message Count > 0).
    pub async fn fetch_queued_messages(
        &self,
        node: &NodeId,
        port_address: PortAddress,
        uid: Uid,
    ) -> Result<()> {
        let n = self.node_or_err(node)?;
        rdm_engine::drain_queue(&self.inner, n.id, n.address, port_address, uid).await;
        Ok(())
    }

    /// A [`crate::link::RdmLink`] bound to one node port.
    pub fn port_link(&self, node: &NodeId, port_address: PortAddress) -> ControllerPortLink {
        ControllerPortLink::new(self.clone(), *node, port_address)
    }

    // ---------------------------------------------------------------- TOD

    /// ArtTodRequest for one port (answer: ArtTodData → [`Event::TodChanged`]; also subscribes to unsolicited
    /// TOD updates).
    pub async fn tod_request(&self, node: &NodeId, port_address: PortAddress) -> Result<()> {
        let n = self.node_or_err(node)?;
        let p = ArtTodRequest::for_port_addresses(port_address.net(), &[port_address]);
        self.inner
            .send_to(&ArtPacket::TodRequest(p), n.address)
            .await
    }

    /// ArtTodRequest and wait for the complete answer (all ArtTodData blocks), even if the TOD did not change.
    /// A TodNak gives an empty list.
    pub async fn tod_refresh(
        &self,
        node: &NodeId,
        port_address: PortAddress,
        timeout: Duration,
    ) -> Result<Vec<Uid>> {
        let (tx, rx) = oneshot::channel();
        lock(&self.inner.state)
            .tod_waiters
            .push(((*node, port_address.raw()), tx));
        self.tod_request(node, port_address).await?;
        match tokio::time::timeout(timeout, rx).await {
            Ok(Ok(u)) => Ok(u),
            Ok(Err(_)) => Err(Error::Closed),
            Err(_) => Err(Error::WaitTimeout(timeout, "ArtTodData")),
        }
    }

    /// ArtTodControl with any command (`ArtTodControl::ATC_*`).
    pub async fn tod_control(
        &self,
        node: &NodeId,
        port_address: PortAddress,
        command: u8,
    ) -> Result<()> {
        let n = self.node_or_err(node)?;
        let p = ArtTodControl::new(port_address, command);
        self.inner
            .send_to(&ArtPacket::TodControl(p), n.address)
            .await
    }

    /// AtcFlush: clear the TOD and run a full discovery.
    pub async fn tod_flush(&self, node: &NodeId, port_address: PortAddress) -> Result<()> {
        self.tod_control(node, port_address, ArtTodControl::ATC_FLUSH)
            .await
    }

    // ---------------------------------------------------------------- configuration

    /// Send ArtAddress (BindIndex in the packet selects the port page) and wait for the node's ArtPollReply.
    /// Returns the updated state (`None` if no reply came within [`ControllerConfig::reply_timeout`]).
    pub async fn art_address(
        &self,
        node: &NodeId,
        packet: ArtAddress,
    ) -> Result<Option<NodeState>> {
        let n = self.node_or_err(node)?;
        self.send_and_wait_reply(&n, ArtPacket::Address(packet))
            .await
    }

    /// ArtAddress with only a command (`artnet::address_command::*`) for one BindIndex.
    pub async fn art_address_command(
        &self,
        node: &NodeId,
        bind_index: u8,
        command: u8,
    ) -> Result<Option<NodeState>> {
        self.art_address(node, ArtAddress::command(bind_index, command))
            .await
    }

    /// ArtInput: `disabled[i]` disables DMX input i of that BindIndex page. Waits for the ArtPollReply.
    pub async fn art_input(
        &self,
        node: &NodeId,
        bind_index: u8,
        disabled: &[bool],
    ) -> Result<Option<NodeState>> {
        let n = self.node_or_err(node)?;
        let mut input = [0u8; 4];
        for (i, d) in disabled.iter().take(4).enumerate() {
            input[i] = u8::from(*d);
        }
        let ports = n.bind_ports(bind_index).count().max(disabled.len().min(4)) as u16;
        let p = ArtInput {
            bind_index,
            num_ports: ports.max(1),
            input,
        };
        self.send_and_wait_reply(&n, ArtPacket::Input(p)).await
    }

    /// Read the node's IP configuration without changing it: ArtIpProg with Command = 0 → ArtIpProgReply
    /// (IP, mask, gateway, DHCP state; ArtPollReply carries no mask/gateway).
    pub async fn ip_prog_query(&self, node: &NodeId) -> Result<ArtIpProgReply> {
        self.ip_prog(node, ArtIpProg::query()).await
    }

    /// Program the node's IP configuration (a default `change` is a query). The reply still comes from the
    /// old IP and carries the new values (contract §5).
    pub async fn ip_prog_change(
        &self,
        node: &NodeId,
        change: &IpProgChange,
    ) -> Result<ArtIpProgReply> {
        self.ip_prog(node, ArtIpProg::from_change(change)).await
    }

    /// ArtIpProg → ArtIpProgReply.
    pub async fn ip_prog(&self, node: &NodeId, packet: ArtIpProg) -> Result<ArtIpProgReply> {
        let n = self.node_or_err(node)?;
        let (tx, rx) = oneshot::channel();
        lock(&self.inner.state).ipprog_waiters.push((n.address, tx));
        self.inner
            .send_to(&ArtPacket::IpProg(packet), n.address)
            .await?;
        let t = self.inner.cfg.reply_timeout;
        match tokio::time::timeout(t, rx).await {
            Ok(Ok(r)) => Ok(r),
            Ok(Err(_)) => Err(Error::Closed),
            Err(_) => Err(Error::WaitTimeout(t, "ArtIpProgReply")),
        }
    }

    async fn send_and_wait_reply(
        &self,
        n: &NodeState,
        packet: ArtPacket,
    ) -> Result<Option<NodeState>> {
        let (tx, rx) = oneshot::channel();
        lock(&self.inner.state).reply_waiters.push((n.address, tx));
        self.inner.send_to(&packet, n.address).await?;
        let got = tokio::time::timeout(self.inner.cfg.reply_timeout, rx).await;
        Ok(match got {
            Ok(Ok(())) => self.node(&n.id),
            _ => None,
        })
    }

    // ---------------------------------------------------------------- DMX

    /// Send one universe as ArtDmx (unicast) to every node that has an output port with this Port-Address.
    /// Sequence counts 1..255 per Port-Address; data is padded to an even length ≥ 2. Returns the number of
    /// nodes it was sent to.
    pub async fn send_dmx(&self, port_address: PortAddress, data: &[u8]) -> Result<usize> {
        let targets: Vec<SocketAddr> = {
            let st = lock(&self.inner.state);
            let mut t: Vec<SocketAddr> = st
                .nodes
                .values()
                .filter(|n| {
                    n.state.ports.iter().any(|p| {
                        p.port_address == port_address && p.direction == PortDirection::Output
                    })
                })
                .map(|n| n.state.address)
                .collect();
            t.sort();
            t.dedup();
            t
        };
        if targets.is_empty() {
            return Ok(0);
        }
        let pkt = self.dmx_packet(port_address, data)?;
        for t in &targets {
            self.inner.send_raw(&pkt, *t).await?;
        }
        Ok(targets.len())
    }

    /// Send one universe as ArtDmx to one node (even if it does not report this Port-Address).
    pub async fn send_dmx_to(
        &self,
        node: &NodeId,
        port_address: PortAddress,
        data: &[u8],
    ) -> Result<()> {
        let n = self.node_or_err(node)?;
        let pkt = self.dmx_packet(port_address, data)?;
        self.inner.send_raw(&pkt, n.address).await
    }

    /// Non-blocking ArtDmx to `dest` (any IP, also one that is not a known node) with an explicit `physical`
    /// (one merge source per caller, contract §4). Sequence 1..255 counts per (destination, Port-Address); data
    /// is padded to an even length ≥ 2. Fails with an I/O error (`WouldBlock`) instead of waiting when the socket
    /// buffer is full – the frame is then dropped, the next one follows anyway.
    pub fn try_send_dmx(
        &self,
        dest: SocketAddr,
        port_address: PortAddress,
        data: &[u8],
        physical: u8,
    ) -> Result<()> {
        let d = dmx_data(data)?;
        let seq = {
            let mut st = lock(&self.inner.state);
            next_seq(st.dmx_seq_to.entry((dest, port_address.raw())).or_insert(0))
        };
        let pkt = ArtDmx::new(seq, physical, port_address, d)?.encode();
        self.inner.socket.try_send_to(&pkt, dest)?;
        Ok(())
    }

    /// Non-blocking ArtSync to every broadcast target (contract §4: never unicast). Errors of single targets are
    /// ignored like in [`send_sync`](Self::send_sync).
    pub fn try_send_sync(&self) {
        let pkt = ArtPacket::Sync(ArtSync::default()).encode();
        for t in &self.inner.broadcast {
            let _ = self.inner.socket.try_send_to(&pkt, *t);
        }
    }

    fn dmx_packet(&self, pa: PortAddress, data: &[u8]) -> Result<Vec<u8>> {
        let d = dmx_data(data)?;
        let seq = next_seq(lock(&self.inner.state).dmx_seq.entry(pa.raw()).or_insert(0));
        Ok(ArtDmx::new(seq, self.inner.cfg.physical, pa, d)?.encode())
    }

    /// ArtSync as broadcast (contract §4: never unicast).
    pub async fn send_sync(&self) -> Result<()> {
        let pkt = ArtPacket::Sync(ArtSync::default()).encode();
        for t in &self.inner.broadcast {
            let _ = self.inner.socket.send_to(&pkt, t).await;
        }
        Ok(())
    }

    // ---------------------------------------------------------------- firmware

    fn job_observer(&self) -> crate::flash::Observer {
        let ev = self.inner.events.clone();
        Arc::new(move |p: &FlashProgress| {
            let _ = ev.send(Event::FlashProgress {
                progress: p.clone(),
            });
        })
    }

    /// Start a tube update as a background job (contract §9.1). `rescue` uses BOOT_CATCH on the node
    /// (the user power-cycles the tube while the job is in phase `Catching`).
    pub fn flash_tube(
        &self,
        node: &NodeId,
        port_address: PortAddress,
        uid: Uid,
        image: TubeImage,
        allow_downgrade: bool,
        rescue: bool,
    ) -> Result<FlashJob> {
        let mut opts = TubeFlashOptions {
            allow_downgrade,
            ..TubeFlashOptions::default()
        };
        if rescue {
            let n = self.node_or_err(node)?;
            let port = n.port(port_address).ok_or_else(|| Error::UnknownPort {
                node: n.id.to_string(),
                port_address: port_address.to_string(),
            })?;
            let node_uid = n
                .uid
                .ok_or_else(|| Error::InvalidArgument("node reports no RDM UID".into()))?;
            opts.rescue = Some(RescueTarget {
                node_uid,
                port: port.port_number,
            });
        }
        self.flash_tube_with(node, port_address, uid, image, opts)
    }

    /// Tube update with explicit options.
    pub fn flash_tube_with(
        &self,
        node: &NodeId,
        port_address: PortAddress,
        uid: Uid,
        image: TubeImage,
        opts: TubeFlashOptions,
    ) -> Result<FlashJob> {
        let n = self.node_or_err(node)?;
        self.port_check(&n, port_address)?;
        let link = self.port_link(node, port_address);
        let id = self.inner.jobs.fetch_add(1, Ordering::Relaxed);
        Ok(FlashJob::spawn(
            id,
            Some(self.job_observer()),
            move |p| async move { flash_tube(&link, uid, &image, &opts, &p).await },
        ))
    }

    /// Start a node update as a background job (contract §9.2).
    pub fn flash_node(
        &self,
        node: &NodeId,
        image: NodeImage,
        opts: NodeFlashOptions,
    ) -> Result<FlashJob> {
        let n = self.node_or_err(node)?;
        let pa = n
            .ports
            .first()
            .map(|p| p.port_address)
            .ok_or_else(|| Error::InvalidArgument("node has no ports".into()))?;
        let link = ControllerNodeLink::new(self.clone(), n.id, pa)?;
        let id = self.inner.jobs.fetch_add(1, Ordering::Relaxed);
        Ok(FlashJob::spawn(
            id,
            Some(self.job_observer()),
            move |p| async move { flash_node(&link, &image, &opts, &p).await },
        ))
    }

    pub(crate) async fn firmware_block(
        &self,
        node: &NodeId,
        block: ArtFirmwareMaster,
        timeout: Duration,
    ) -> Result<ArtFirmwareReply> {
        let n = self.node_or_err(node)?;
        let (tx, rx) = oneshot::channel();
        lock(&self.inner.state).fw_waiters.push((n.address, tx));
        self.inner
            .send_to(&ArtPacket::FirmwareMaster(block), n.address)
            .await?;
        match tokio::time::timeout(timeout, rx).await {
            Ok(Ok(r)) => Ok(r),
            Ok(Err(_)) => Err(Error::Closed),
            Err(_) => Err(Error::WaitTimeout(timeout, "ArtFirmwareReply")),
        }
    }
}

pub(crate) fn lock<T>(m: &Mutex<T>) -> MutexGuard<'_, T> {
    m.lock().unwrap_or_else(|e| e.into_inner())
}

/// DMX slots padded to an even length ≥ 2 (ArtDmx rule), at most 512.
fn dmx_data(data: &[u8]) -> Result<Vec<u8>> {
    if data.len() > ArtDmx::MAX_DATA {
        return Err(Error::InvalidArgument(format!(
            "DMX data {} > 512 slots",
            data.len()
        )));
    }
    let mut d = data.to_vec();
    if d.len() % 2 == 1 || d.is_empty() {
        d.push(0);
    }
    if d.len() < 2 {
        d.push(0);
    }
    Ok(d)
}

/// ArtDmx Sequence 1..255 (0 = off is never sent).
fn next_seq(s: &mut u8) -> u8 {
    *s = if *s == 255 { 1 } else { *s + 1 };
    *s
}

impl Inner {
    pub(crate) async fn send_raw(&self, pkt: &[u8], to: SocketAddr) -> Result<()> {
        self.socket.send_to(pkt, to).await?;
        Ok(())
    }

    pub(crate) async fn send_to(&self, p: &ArtPacket, to: SocketAddr) -> Result<()> {
        self.send_raw(&p.encode(), to).await
    }

    pub(crate) fn emit(&self, e: Event) {
        let _ = self.events.send(e);
    }

    async fn send_poll(&self) -> Result<()> {
        let mut p = ArtPoll::new_reply_on_change();
        if self.cfg.diagnostics {
            p.flags |= ArtPoll::FLAG_DIAG | ArtPoll::FLAG_DIAG_UNICAST;
            p.diag_priority = self.cfg.diag_priority;
        }
        let pkt = p.encode();
        let mut last_err = None;
        let mut ok = false;
        let extra = lock(&self.state).poll_targets.clone();
        for t in self.broadcast.iter().chain(extra.iter()) {
            match self.socket.send_to(&pkt, t).await {
                Ok(_) => ok = true,
                Err(e) => last_err = Some(e),
            }
        }
        match (ok, last_err) {
            (false, Some(e)) => Err(e.into()),
            _ => Ok(()),
        }
    }

    fn own_reply(&self) -> ArtPollReply {
        let ip = match self.local {
            SocketAddr::V4(a) => *a.ip(),
            SocketAddr::V6(_) => Ipv4Addr::UNSPECIFIED,
        };
        ArtPollReply {
            ip,
            bind_ip: ip,
            short_name: self.cfg.short_name.clone(),
            long_name: self.cfg.long_name.clone(),
            esta_man: pids::ESTA_MANUFACTURER,
            oem: pids::ARTNET_OEM_NODE,
            style: 1, // StController
            num_ports: 0,
            status2: 0x08,
            bind_index: 1,
            ..ArtPollReply::default()
        }
    }

    async fn handle_packet(self: &Arc<Self>, buf: &[u8], src: SocketAddr) {
        let Ok(p) = ArtPacket::decode(buf) else {
            return;
        };
        match p {
            ArtPacket::PollReply(r) => self.on_poll_reply(r, src),
            ArtPacket::Poll(_) => {
                if self.cfg.reply_to_polls && src != self.local {
                    let _ = self
                        .send_to(&ArtPacket::PollReply(self.own_reply()), src)
                        .await;
                }
            }
            ArtPacket::TodData(t) => self.on_tod_data(t, src),
            ArtPacket::Rdm(r) => self.rdm.on_response(r, src),
            ArtPacket::DiagData(d) => {
                let node = self.node_id_by_addr(src);
                self.emit(Event::Diag {
                    node,
                    address: src,
                    priority: d.diag_priority,
                    logical_port: d.logical_port,
                    text: d.text,
                });
            }
            ArtPacket::IpProgReply(r) => {
                let mut st = lock(&self.state);
                if let Some(i) = st.ipprog_waiters.iter().position(|(a, _)| *a == src) {
                    let (_, tx) = st.ipprog_waiters.remove(i);
                    let _ = tx.send(r);
                }
            }
            ArtPacket::FirmwareReply(r) => {
                let mut st = lock(&self.state);
                if let Some(i) = st.fw_waiters.iter().position(|(a, _)| *a == src) {
                    let (_, tx) = st.fw_waiters.remove(i);
                    let _ = tx.send(r);
                }
            }
            _ => {}
        }
    }

    pub(crate) fn node_id_by_addr(&self, src: SocketAddr) -> Option<NodeId> {
        let st = lock(&self.state);
        st.binds
            .iter()
            .find(|((a, _), _)| *a == src)
            .map(|(_, b)| b.id)
    }

    fn on_poll_reply(self: &Arc<Self>, r: ArtPollReply, src: SocketAddr) {
        // Other controllers (and our own reply to our own poll) are not nodes.
        if r.style == 1 || src == self.local {
            return;
        }
        let id = reply_node_id(&r, src);
        let now = Instant::now();
        let mut st = lock(&self.state);
        // New node, one that rebooted into other firmware, or one that restarted (its NodeReport counter went
        // back; it forgot who requested its TODs, contract §6.1): (re-)subscribe to its TODs.
        let count = report_counter(&r.node_report);
        let restarted = match (count, st.report_counts.get(&id)) {
            (Some(c), Some(&prev)) => report_counter_restarted(prev, c),
            _ => false,
        };
        if let Some(c) = count {
            st.report_counts.insert(id, c);
        }
        let is_new = restarted
            || st
                .nodes
                .get(&id)
                .is_none_or(|n| n.state.firmware_version != r.vers_info);
        // IP/port change: drop pages of this node from other addresses.
        st.binds.retain(|(a, _), b| !(b.id == id && *a != src));
        // A different node at this address (e.g. replaced device): drop its pages.
        st.binds.retain(|(a, _), b| !(*a == src && b.id != id));
        st.binds.insert(
            (src, r.bind_index.max(1)),
            BindEntry {
                id,
                reply: r,
                last_seen: now,
            },
        );
        let changed = rebuild_node(&mut st, id);
        let waiters: Vec<_> = {
            let (mine, rest): (Vec<_>, Vec<_>) = std::mem::take(&mut st.reply_waiters)
                .into_iter()
                .partition(|(a, _)| *a == src);
            st.reply_waiters = rest;
            mine
        };
        let state = st.nodes.get(&id).map(|n| n.state.clone());
        drop(st);
        if let (Some(s), true) = (&state, changed) {
            self.emit(Event::NodeUpdated { node: s.clone() });
        }
        for (_, tx) in waiters {
            let _ = tx.send(());
        }
        if let (Some(s), true) = (state, is_new && self.cfg.auto_tod_request) {
            let me = self.clone();
            tokio::spawn(async move { me.request_tods(&s).await });
        }
    }

    async fn request_tods(&self, n: &NodeState) {
        let mut by_net: BTreeMap<u8, Vec<PortAddress>> = BTreeMap::new();
        for p in &n.ports {
            if p.direction == PortDirection::Output && p.rdm_enabled {
                by_net
                    .entry(p.port_address.net())
                    .or_default()
                    .push(p.port_address);
            }
        }
        for (net, pas) in by_net {
            let p = ArtTodRequest::for_port_addresses(net, &pas);
            let _ = self.send_to(&ArtPacket::TodRequest(p), n.address).await;
        }
    }

    fn on_tod_data(&self, t: ArtTodData, src: SocketAddr) {
        let pa = t.port_address();
        let key = (src, pa.raw());
        let mut st = lock(&self.state);
        let complete: Option<(Vec<Uid>, bool)> = if t.command_response == TOD_NAK {
            st.tod_assembly.remove(&key);
            Some((Vec::new(), true))
        } else {
            let a = st.tod_assembly.entry(key).or_default();
            if a.total != t.uid_total || (t.block_count == 0 && !a.blocks.is_empty()) {
                a.total = t.uid_total;
                a.blocks.clear();
            }
            a.blocks.insert(t.block_count, t.uids);
            let have: usize = a.blocks.values().map(Vec::len).sum();
            if have >= usize::from(a.total) {
                let set: BTreeSet<Uid> = a.blocks.values().flatten().copied().collect();
                st.tod_assembly.remove(&key);
                Some((set.into_iter().collect(), false))
            } else {
                None
            }
        };
        let Some((uids, nak)) = complete else {
            return;
        };
        let node = st
            .binds
            .iter()
            .find(|((a, _), _)| *a == src)
            .map(|(_, b)| b.id)
            .unwrap_or(NodeId::Addr(src));
        let old = st.tods.insert((node, pa.raw()), uids.clone());
        let (mine, rest): (Vec<_>, Vec<_>) = std::mem::take(&mut st.tod_waiters)
            .into_iter()
            .partition(|(k, _)| *k == (node, pa.raw()));
        st.tod_waiters = rest;
        drop(st);
        for (_, tx) in mine {
            let _ = tx.send(uids.clone());
        }
        if old.as_ref() == Some(&uids) && !nak {
            return;
        }
        let old = old.unwrap_or_default();
        let added = uids.iter().filter(|u| !old.contains(u)).copied().collect();
        let removed = old.iter().filter(|u| !uids.contains(u)).copied().collect();
        self.emit(Event::TodChanged {
            node,
            port_address: pa,
            uids,
            added,
            removed,
            nak,
        });
    }

    fn expire(&self) {
        let now = Instant::now();
        let timeout = self.cfg.node_timeout;
        let mut st = lock(&self.state);
        let ids: BTreeSet<NodeId> = st
            .binds
            .values()
            .filter(|b| now.duration_since(b.last_seen) > timeout)
            .map(|b| b.id)
            .collect();
        if ids.is_empty() {
            return;
        }
        st.binds
            .retain(|_, b| now.duration_since(b.last_seen) <= timeout);
        let mut events = Vec::new();
        for id in ids {
            if rebuild_node(&mut st, id) {
                match st.nodes.get(&id) {
                    Some(n) => events.push(Event::NodeUpdated {
                        node: n.state.clone(),
                    }),
                    None => {
                        st.tods.retain(|(n, _), _| *n != id);
                        events.push(Event::NodeGone { id });
                    }
                }
            }
        }
        drop(st);
        for e in events {
            self.emit(e);
        }
    }
}

/// Recompute a node from its pages; returns true if it changed (or disappeared).
fn rebuild_node(st: &mut State, id: NodeId) -> bool {
    let replies: Vec<(SocketAddr, ArtPollReply)> = st
        .binds
        .iter()
        .filter(|(_, b)| b.id == id)
        .map(|((a, _), b)| (*a, b.reply.clone()))
        .collect();
    match merge_replies(id, &replies) {
        Some(state) => {
            let changed = st.nodes.get(&id).map(|n| &n.state) != Some(&state);
            st.nodes.insert(id, NodeEntry { state });
            changed
        }
        None => st.nodes.remove(&id).is_some(),
    }
}

async fn recv_loop(socket: Arc<UdpSocket>, inner: Weak<Inner>) {
    let mut buf = vec![0u8; 2048];
    loop {
        let (n, src) = match socket.recv_from(&mut buf).await {
            Ok(x) => x,
            Err(_) => {
                // e.g. ICMP port unreachable surfaced on some platforms: keep going
                tokio::time::sleep(Duration::from_millis(5)).await;
                continue;
            }
        };
        let Some(inner) = inner.upgrade() else {
            return;
        };
        inner.handle_packet(&buf[..n], src).await;
    }
}

async fn poll_loop(inner: Weak<Inner>) {
    let mut last_poll: Option<Instant> = None;
    let mut last_queue: Option<Instant> = None;
    loop {
        let Some(me) = inner.upgrade() else {
            return;
        };
        let iv = me.cfg.poll_interval;
        if !iv.is_zero() && last_poll.is_none_or(|t| t.elapsed() >= iv) {
            let _ = me.send_poll().await;
            last_poll = Some(Instant::now());
        }
        me.expire();
        let qv = me.cfg.rdm.node_queue_interval;
        if !qv.is_zero() && last_queue.is_none_or(|t| t.elapsed() >= qv) {
            last_queue = Some(Instant::now());
            // Last-writer-wins for RDM-only node settings (contract §7): look into every node's queue.
            let targets: Vec<(NodeId, SocketAddr, PortAddress, Uid)> = {
                let st = lock(&me.state);
                st.nodes
                    .values()
                    .filter_map(|n| {
                        let pa = n.state.ports.first()?.port_address;
                        Some((n.state.id, n.state.address, pa, n.state.uid?))
                    })
                    .collect()
            };
            for (id, addr, pa, uid) in targets {
                let me2 = me.clone();
                tokio::spawn(async move { rdm_engine::drain_queue(&me2, id, addr, pa, uid).await });
            }
        }
        drop(me);
        tokio::time::sleep(Duration::from_millis(50)).await;
    }
}
