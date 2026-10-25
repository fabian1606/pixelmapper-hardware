//! A scriptable in-memory backend: tests put nodes, TODs and RDM answers in, emit events, and read back every
//! call and every DMX frame set the server handed over. `MockBackend::demo()` is a small fake installation for
//! `pmnet-bridge serve --mock` (app development without hardware).

use super::{
    BackendError, DmxFrame, DmxTarget, FlashRequest, NetBackend, RdmAddress, RdmPayload,
    RdmRequest, RdmResponse, Snapshot,
};
use crate::protocol::{
    ArtAddressArgs, BridgeStatus, Event, Failsafe, FirmwareImageInfo, FlashJob, FlashPhase,
    FlashResult, Indicator, IpProgArgs, IpProgReply, MacAddress, MergeMode, NetInterface, NodeId,
    NodeState, NodeStatus, NodeStyle, OutputStyle, PortAddressAuthority, PortDirection,
    PortInputState, PortOutputState, PortProtocol, PortRef, PortState, RawStatus,
    RdmDeviceDescription, RdmDeviceInfo, RdmPersonality, RdmUid, RdmValue, SetDirection,
    SetIndicator, Tod, TodAction, UdpState,
};
use std::collections::{BTreeMap, HashMap};
use std::net::Ipv4Addr;
use std::sync::{Arc, Mutex, MutexGuard};
use std::time::{Duration, SystemTime, UNIX_EPOCH};
use tokio::sync::broadcast;

// ── Recorded calls ────────────────────────────────────────────────────────────────────────────────────────────────

/// One call the server made on the mock (DMX and poll targets are recorded separately).
#[derive(Debug, Clone, PartialEq)]
#[allow(missing_docs)]
pub enum MockCall {
    Poll,
    TodRequest(NodeId, PortRef),
    TodControl(NodeId, PortRef, TodAction),
    RdmGet(RdmRequest),
    RdmSet(RdmRequest),
    RdmDescribe(RdmAddress, bool),
    ArtAddress(ArtAddressArgs),
    IpProg(IpProgArgs),
    ArtInput(NodeId, u8, Vec<bool>),
    Flash(FlashRequest),
    FlashCancel(String),
}

/// One `send_dmx` call.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SentDmx {
    /// Frames that reached a known destination.
    pub frames: Vec<DmxFrame>,
    /// ArtSync requested.
    pub sync: bool,
    /// `Physical` of the sending connection.
    pub physical: u8,
}

/// Scripted answer of a device to one PID.
#[derive(Debug, Clone, PartialEq)]
pub enum MockRdm {
    /// ACK with parameter data and optional decoded value.
    Ack {
        /// Raw parameter data.
        data: Vec<u8>,
        /// Decoded value.
        value: Option<RdmValue>,
    },
    /// NACK with reason.
    Nack(u16),
    /// No answer.
    Timeout,
}

// ── State ─────────────────────────────────────────────────────────────────────────────────────────────────────────

/// Everything the mock knows; guarded by one mutex that is never held across an `.await`.
#[derive(Debug)]
struct MockState {
    status: BridgeStatus,
    nodes: BTreeMap<NodeId, NodeState>,
    tods: BTreeMap<(NodeId, PortRef), Tod>,
    rdm: HashMap<(RdmUid, u16), MockRdm>,
    descriptions: HashMap<RdmUid, RdmDeviceDescription>,
    rdm_delay: Duration,
    flash_step: Duration,
    calls: Vec<MockCall>,
    dmx: Vec<SentDmx>,
    poll_targets: Vec<Ipv4Addr>,
    jobs: BTreeMap<String, FlashJob>,
    next_job: u32,
}

/// Shared part, so flash jobs can run in their own task.
#[derive(Debug)]
struct Inner {
    state: Mutex<MockState>,
    events: broadcast::Sender<Event>,
}

impl Inner {
    /// Locks the state; a poisoned lock (a panicking test) is still usable.
    fn lock(&self) -> MutexGuard<'_, MockState> {
        self.state.lock().unwrap_or_else(|e| e.into_inner())
    }

    /// Emits an event (no receivers is fine).
    fn emit(&self, event: Event) {
        let _ = self.events.send(event);
    }
}

/// Scriptable backend for tests and `serve --mock`.
#[derive(Debug, Clone)]
pub struct MockBackend {
    inner: Arc<Inner>,
}

impl Default for MockBackend {
    fn default() -> Self {
        Self::new()
    }
}

/// Milliseconds since the Unix epoch.
fn now_ms() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0)
}

impl MockBackend {
    /// Empty network: one interface, UDP bound, no nodes.
    pub fn new() -> Self {
        let status = BridgeStatus {
            interfaces: vec![NetInterface {
                name: "mock0".into(),
                ip: Ipv4Addr::new(2, 0, 0, 1),
                mask: Ipv4Addr::new(255, 0, 0, 0),
                mac: None,
            }],
            udp: UdpState {
                bound: true,
                port: 6454,
                error: None,
            },
        };
        let state = MockState {
            status,
            nodes: BTreeMap::new(),
            tods: BTreeMap::new(),
            rdm: HashMap::new(),
            descriptions: HashMap::new(),
            rdm_delay: Duration::ZERO,
            flash_step: Duration::from_millis(50),
            calls: Vec::new(),
            dmx: Vec::new(),
            poll_targets: Vec::new(),
            jobs: BTreeMap::new(),
            next_job: 1,
        };
        Self {
            inner: Arc::new(Inner {
                state: Mutex::new(state),
                events: broadcast::channel(1024).0,
            }),
        }
    }

    // ── Scripting ─────────────────────────────────────────────────────────────────────────────────────────────

    /// Adds or replaces a node and emits `node`.
    pub fn upsert_node(&self, node: NodeState) {
        self.inner
            .lock()
            .nodes
            .insert(node.node_id.clone(), node.clone());
        self.inner.emit(Event::Node { node });
    }

    /// Removes a node and its TODs and emits `nodeGone`.
    pub fn remove_node(&self, node_id: &NodeId) {
        let removed = {
            let mut s = self.inner.lock();
            s.tods.retain(|(n, _), _| n != node_id);
            s.nodes.remove(node_id)
        };
        if let Some(node) = removed {
            self.inner.emit(Event::NodeGone {
                node_id: node.node_id,
                last_seen: node.last_seen,
            });
        }
    }

    /// Stores a TOD (answer to later `todRequest`s) and emits `tod`.
    pub fn set_tod(&self, tod: Tod) {
        self.inner
            .lock()
            .tods
            .insert((tod.node_id.clone(), tod.port), tod.clone());
        self.inner.emit(Event::Tod(tod));
    }

    /// Replaces interfaces/UDP state and emits `bridgeStatus`.
    pub fn set_status(&self, status: BridgeStatus) {
        self.inner.lock().status = status.clone();
        self.inner.emit(Event::BridgeStatus(status));
    }

    /// Emits any event as-is.
    pub fn emit(&self, event: Event) {
        self.inner.emit(event);
    }

    /// Scripts the answer of `uid` to `pid` (GET and SET).
    pub fn set_rdm(&self, uid: RdmUid, pid: u16, answer: MockRdm) {
        self.inner.lock().rdm.insert((uid, pid), answer);
    }

    /// Scripts the `rdmDescribe` result of a device.
    pub fn set_description(&self, description: RdmDeviceDescription) {
        self.inner
            .lock()
            .descriptions
            .insert(description.uid, description);
    }

    /// Delay before every RDM answer (to test concurrency).
    pub fn set_rdm_delay(&self, delay: Duration) {
        self.inner.lock().rdm_delay = delay;
    }

    /// Duration of one simulated flash step.
    pub fn set_flash_step(&self, step: Duration) {
        self.inner.lock().flash_step = step;
    }

    // ── Inspection ────────────────────────────────────────────────────────────────────────────────────────────

    /// Calls so far, in order.
    pub fn calls(&self) -> Vec<MockCall> {
        self.inner.lock().calls.clone()
    }

    /// Frame sets so far, in order.
    pub fn dmx_sent(&self) -> Vec<SentDmx> {
        self.inner.lock().dmx.clone()
    }

    /// Current extra poll targets.
    pub fn poll_targets(&self) -> Vec<Ipv4Addr> {
        self.inner.lock().poll_targets.clone()
    }

    /// Records a call.
    fn record(&self, call: MockCall) {
        self.inner.lock().calls.push(call);
    }

    /// Looks up a node or fails with `UnknownNode`.
    fn node(&self, id: &NodeId) -> Result<NodeState, BackendError> {
        self.inner
            .lock()
            .nodes
            .get(id)
            .cloned()
            .ok_or_else(|| BackendError::UnknownNode(id.clone()))
    }

    /// Stores a changed node (fresh `lastSeen`) and emits `node`, then returns it.
    fn commit_node(&self, mut node: NodeState) -> NodeState {
        node.last_seen = now_ms().max(node.last_seen + 1);
        self.inner
            .lock()
            .nodes
            .insert(node.node_id.clone(), node.clone());
        self.inner.emit(Event::Node { node: node.clone() });
        node
    }

    /// Answer of a device to an RDM request after the configured delay.
    async fn rdm(&self, req: &RdmRequest, set: bool) -> Result<RdmResponse, BackendError> {
        let delay = self.inner.lock().rdm_delay;
        if !delay.is_zero() {
            tokio::time::sleep(delay).await;
        }
        if req.address.uid.is_broadcast() {
            return Ok(RdmResponse {
                data: Vec::new(),
                value: None,
                broadcast: true,
            });
        }
        let mut s = self.inner.lock();
        let key = (req.address.uid, req.pid);
        let known_device =
            s.descriptions.contains_key(&req.address.uid) || s.rdm.keys().any(|(u, _)| *u == key.0);
        match s.rdm.get(&key).cloned() {
            Some(MockRdm::Nack(reason)) => Err(BackendError::Nack(reason)),
            Some(MockRdm::Timeout) => Err(BackendError::Timeout),
            Some(MockRdm::Ack { data, value }) if !set => Ok(RdmResponse {
                data,
                value,
                broadcast: false,
            }),
            None if !set => Err(BackendError::Timeout),
            // SET on a known device: ACK, and the device now reports the written data (last writer wins).
            _ if known_device => {
                let (data, value) = match &req.payload {
                    RdmPayload::Empty => (Vec::new(), None),
                    RdmPayload::Data(d) => (d.clone(), None),
                    RdmPayload::Value(v) => (Vec::new(), Some(v.clone())),
                };
                s.rdm.insert(key, MockRdm::Ack { data, value });
                Ok(RdmResponse {
                    data: Vec::new(),
                    value: None,
                    broadcast: false,
                })
            }
            _ => Err(BackendError::Timeout),
        }
    }

    /// Runs a simulated flash job until it is done or cancelled.
    async fn run_job(inner: Arc<Inner>, job_id: String) {
        // Advances the job by one step; returns false when it ended (or was cancelled meanwhile).
        let step = |advance: &dyn Fn(&mut FlashJob)| -> bool {
            let mut s = inner.lock();
            let Some(job) = s.jobs.get_mut(&job_id) else {
                return false;
            };
            if job.phase.is_terminal() {
                return false;
            }
            advance(job);
            let job = job.clone();
            drop(s);
            inner.emit(Event::FlashProgress { job: job.clone() });
            !job.phase.is_terminal()
        };
        let pause = inner.lock().flash_step;
        let total = inner
            .lock()
            .jobs
            .get(&job_id)
            .map(|j| j.pages_total)
            .unwrap_or(0);
        tokio::time::sleep(pause).await;
        if !step(&|j| {
            j.phase = FlashPhase::Writing;
            j.in_bootloader = true;
        }) {
            return;
        }
        for _ in 0..total {
            tokio::time::sleep(pause).await;
            if !step(&|j| j.pages_done += 1) {
                return;
            }
        }
        for phase in [
            FlashPhase::Verifying,
            FlashPhase::Finishing,
            FlashPhase::Confirming,
        ] {
            tokio::time::sleep(pause).await;
            if !step(&|j| j.phase = phase) {
                return;
            }
        }
        tokio::time::sleep(pause).await;
        step(&|j| {
            j.phase = FlashPhase::Done;
            j.in_bootloader = false;
            j.result = Some(FlashResult {
                software_version_id: j.image.version_id,
                software_version_label: Some(j.image.version.clone()),
            });
        });
    }
}

// ── NetBackend ────────────────────────────────────────────────────────────────────────────────────────────────────

impl NetBackend for MockBackend {
    fn status(&self) -> BridgeStatus {
        self.inner.lock().status.clone()
    }

    fn snapshot(&self) -> Snapshot {
        let s = self.inner.lock();
        Snapshot {
            nodes: s.nodes.values().cloned().collect(),
            tods: s.tods.values().cloned().collect(),
        }
    }

    fn subscribe(&self) -> broadcast::Receiver<Event> {
        self.inner.events.subscribe()
    }

    fn poll_now(&self) -> Result<(), BackendError> {
        if !self.inner.lock().status.udp.bound {
            return Err(BackendError::UdpUnavailable);
        }
        self.record(MockCall::Poll);
        Ok(())
    }

    fn set_poll_targets(&self, targets: &[Ipv4Addr]) {
        self.inner.lock().poll_targets = targets.to_vec();
    }

    async fn tod_request(&self, node: NodeId, port: PortRef) -> Result<(), BackendError> {
        self.record(MockCall::TodRequest(node.clone(), port));
        self.node(&node)?;
        let tod = self.inner.lock().tods.get(&(node, port)).cloned();
        if let Some(tod) = tod {
            self.inner.emit(Event::Tod(tod));
        }
        Ok(())
    }

    async fn tod_control(
        &self,
        node: NodeId,
        port: PortRef,
        action: TodAction,
    ) -> Result<(), BackendError> {
        self.record(MockCall::TodControl(node.clone(), port, action));
        self.node(&node)?;
        // A flush ends with a fresh TOD; the other actions only switch discovery modes.
        if action == TodAction::Flush {
            let tod = self.inner.lock().tods.get(&(node, port)).cloned();
            if let Some(tod) = tod {
                self.inner.emit(Event::Tod(tod));
            }
        }
        Ok(())
    }

    async fn rdm_get(&self, req: RdmRequest) -> Result<RdmResponse, BackendError> {
        self.record(MockCall::RdmGet(req.clone()));
        self.rdm(&req, false).await
    }

    async fn rdm_set(&self, req: RdmRequest) -> Result<RdmResponse, BackendError> {
        self.record(MockCall::RdmSet(req.clone()));
        self.rdm(&req, true).await
    }

    async fn rdm_describe(
        &self,
        address: RdmAddress,
        refresh: bool,
    ) -> Result<RdmDeviceDescription, BackendError> {
        self.record(MockCall::RdmDescribe(address.clone(), refresh));
        let delay = self.inner.lock().rdm_delay;
        if !delay.is_zero() {
            tokio::time::sleep(delay).await;
        }
        self.inner
            .lock()
            .descriptions
            .get(&address.uid)
            .cloned()
            .ok_or(BackendError::Timeout)
    }

    async fn art_address(&self, req: ArtAddressArgs) -> Result<NodeState, BackendError> {
        self.record(MockCall::ArtAddress(req.clone()));
        let mut node = self.node(&req.node_id)?;
        if let Some(name) = &req.short_name {
            node.short_name = name.chars().take(17).collect();
        }
        if let Some(name) = &req.long_name {
            node.long_name = name.chars().take(63).collect();
        }
        for change in &req.ports {
            let port = node
                .ports
                .iter_mut()
                .find(|p| p.port == change.port)
                .ok_or(BackendError::UnknownPort(change.port))?;
            if let Some(a) = change.output_port_address {
                port.output_port_address = a;
            }
            if let Some(a) = change.input_port_address {
                port.input_port_address = a;
            }
            if let Some(d) = change.direction {
                port.direction = match d {
                    SetDirection::Output => PortDirection::Output,
                    SetDirection::Input => PortDirection::Input,
                };
            }
            if let Some(m) = change.merge {
                port.output.merge = m;
            }
            if let Some(r) = change.rdm {
                port.output.rdm = r;
            }
            if let Some(s) = change.output_style {
                port.output.style = s;
            }
        }
        if let Some(f) = req.failsafe {
            node.status.failsafe = f;
        }
        if let Some(i) = req.indicator {
            node.status.indicator = match i {
                SetIndicator::Normal => Indicator::Normal,
                SetIndicator::Mute => Indicator::Mute,
                SetIndicator::Locate => Indicator::Locate,
            };
        }
        Ok(self.commit_node(node))
    }

    async fn ip_prog(&self, req: IpProgArgs) -> Result<IpProgReply, BackendError> {
        self.record(MockCall::IpProg(req.clone()));
        let mut node = self.node(&req.node_id)?;
        let reply = IpProgReply {
            ip: req.ip.unwrap_or(node.ip),
            mask: req.mask.unwrap_or(Ipv4Addr::new(255, 0, 0, 0)),
            gateway: req.gateway.unwrap_or(Ipv4Addr::UNSPECIFIED),
            dhcp: req.dhcp.unwrap_or(node.status.dhcp_configured),
        };
        if reply.ip != node.ip || reply.dhcp != node.status.dhcp_configured {
            node.ip = reply.ip;
            node.status.dhcp_configured = reply.dhcp;
            self.commit_node(node);
        }
        Ok(reply)
    }

    async fn art_input(
        &self,
        node: NodeId,
        bind: u8,
        disabled: Vec<bool>,
    ) -> Result<NodeState, BackendError> {
        self.record(MockCall::ArtInput(node.clone(), bind, disabled.clone()));
        let mut state = self.node(&node)?;
        for port in state.ports.iter_mut().filter(|p| p.port.bind == bind) {
            if let Some(&d) = disabled.get(usize::from(port.port.index)) {
                port.input.disabled = d;
            }
        }
        Ok(self.commit_node(state))
    }

    fn image_info(&self, hex: &str) -> Result<FirmwareImageInfo, BackendError> {
        // Mock only: any text starting with an Intel HEX record counts as the sample image.
        if !hex.trim_start().starts_with(':') {
            return Err(BackendError::InvalidImage("not an Intel HEX file".into()));
        }
        Ok(sample_image())
    }

    async fn flash_start(&self, req: FlashRequest) -> Result<FlashJob, BackendError> {
        self.record(MockCall::Flash(req.clone()));
        let image = self.image_info(&req.hex)?;
        self.node(&req.address.node_id)?;
        let job = {
            let mut s = self.inner.lock();
            let busy = s.jobs.values().any(|j| {
                j.node_id == req.address.node_id
                    && j.port == req.address.port
                    && !j.phase.is_terminal()
            });
            if busy {
                return Err(BackendError::Busy);
            }
            if let Some(d) = s.descriptions.get(&req.address.uid) {
                let current = d.device_info.software_version_id;
                if current > image.version_id && !req.allow_downgrade {
                    return Err(BackendError::Downgrade {
                        current,
                        image: image.version_id,
                    });
                }
            }
            let job = FlashJob {
                job_id: format!("f{}", s.next_job),
                node_id: req.address.node_id.clone(),
                port: req.address.port,
                uid: req.address.uid,
                phase: FlashPhase::Entering,
                pages_done: 0,
                pages_total: 6,
                rewrites: 0,
                in_bootloader: false,
                image,
                error: None,
                result: None,
            };
            s.next_job += 1;
            s.jobs.insert(job.job_id.clone(), job.clone());
            job
        };
        self.inner.emit(Event::FlashProgress { job: job.clone() });
        tokio::spawn(Self::run_job(self.inner.clone(), job.job_id.clone()));
        Ok(job)
    }

    fn flash_cancel(&self, job_id: &str) -> Result<(), BackendError> {
        self.record(MockCall::FlashCancel(job_id.to_owned()));
        let job = {
            let mut s = self.inner.lock();
            let job = s
                .jobs
                .get_mut(job_id)
                .ok_or_else(|| BackendError::UnknownJob(job_id.to_owned()))?;
            if job.phase.is_terminal() {
                return Ok(());
            }
            job.phase = FlashPhase::Cancelled;
            job.clone()
        };
        self.inner.emit(Event::FlashProgress { job });
        Ok(())
    }

    fn flash_jobs(&self) -> Vec<FlashJob> {
        self.inner.lock().jobs.values().cloned().collect()
    }

    fn send_dmx(&self, frames: &[DmxFrame], sync: bool, physical: u8) -> Result<(), BackendError> {
        let mut s = self.inner.lock();
        if !s.status.udp.bound {
            return Err(BackendError::UdpUnavailable);
        }
        let frames: Vec<DmxFrame> = frames
            .iter()
            .filter(|f| match &f.target {
                DmxTarget::Node(id) => s.nodes.contains_key(id),
                DmxTarget::Ip(_) => true,
            })
            .cloned()
            .collect();
        s.dmx.push(SentDmx {
            frames,
            sync,
            physical,
        });
        Ok(())
    }
}

// ── Sample data ───────────────────────────────────────────────────────────────────────────────────────────────────

/// A Pixelmapper Node as it would be merged from its ArtPollReplies: one port per BindIndex (`ports` of them),
/// output, Port-Address = BindIndex (factory default, node contract §4).
pub fn sample_node(node_id: &str, ip: Ipv4Addr, ports: u8) -> NodeState {
    let uid: Option<RdmUid> = node_id.parse().ok();
    NodeState {
        node_id: NodeId::from(node_id),
        uid,
        mac: Some(MacAddress([0x02, 0x7f, 0xf0, 0x1b, 0x2c, 0x3d])),
        ip,
        interface: Some("mock0".into()),
        short_name: "PM Node".into(),
        long_name: "Pixelmapper Node".into(),
        node_report: "#0001 [0001] OK".into(),
        firmware: 0x0100,
        esta_man: 0x7FF0,
        oem: 0x00FF,
        ubea_version: 0,
        style: NodeStyle::Node,
        refresh_rate: 44,
        status: NodeStatus {
            indicator: Indicator::Normal,
            port_address_authority: PortAddressAuthority::Network,
            booted_from_rom: false,
            rdm_capable: true,
            ubea_present: false,
            web_config: false,
            dhcp_configured: false,
            dhcp_capable: true,
            port_address_15_bit: true,
            sacn_switchable: false,
            squawking: false,
            output_style_switchable: true,
            rdm_switchable: true,
            failsafe: Failsafe::Hold,
            failsafe_programmable: true,
            llrp: false,
            port_direction_switchable: true,
            rdmnet: false,
            background_queue: false,
            background_discovery_control: true,
        },
        raw: RawStatus {
            status1: 0xE2,
            status2: 0xCE,
            status3: 0x28,
        },
        ports: (1..=ports)
            .map(|bind| PortState {
                port: PortRef::new(bind, 0),
                protocol: PortProtocol::Dmx512,
                can_output: true,
                can_input: true,
                direction: PortDirection::Output,
                output_port_address: u16::from(bind),
                input_port_address: u16::from(bind),
                output: PortOutputState {
                    data: false,
                    merging: false,
                    merge: MergeMode::Htp,
                    short_circuit: false,
                    sacn: false,
                    rdm: true,
                    style: OutputStyle::Continuous,
                    discovery_running: false,
                    background_discovery: true,
                },
                input: PortInputState {
                    data: false,
                    disabled: false,
                    errors: false,
                    sacn: false,
                },
            })
            .collect(),
        last_seen: now_ms(),
    }
}

/// Image info the mock reports for every Intel HEX text.
pub fn sample_image() -> FirmwareImageInfo {
    FirmwareImageInfo {
        format: 1,
        manufacturer_id: 0x7FF0,
        model_id: 0x0001,
        version_id: 0x0001_0200,
        version: "1.2.0".into(),
        length: 6016,
    }
}

/// A tube's description as `rdmDescribe` would return it.
pub fn sample_tube(uid: RdmUid, start_address: u16) -> RdmDeviceDescription {
    RdmDeviceDescription {
        uid,
        device_info: RdmDeviceInfo {
            rdm_protocol_version: 0x0100,
            device_model_id: 0x0001,
            product_category: 0x0101,
            software_version_id: 0x0001_0100,
            dmx_footprint: 30,
            current_personality: 1,
            personality_count: 2,
            dmx_start_address: start_address,
            sub_device_count: 0,
            sensor_count: 0,
        },
        manufacturer_label: Some("Pixelmapper".into()),
        device_model_description: Some("XBAR500 tube".into()),
        device_label: None,
        software_version_label: Some("1.1.0".into()),
        boot_software_version_id: Some(1),
        boot_software_version_label: Some("boot 1".into()),
        personalities: vec![
            RdmPersonality {
                index: 1,
                footprint: 30,
                description: Some("10 px RGB".into()),
            },
            RdmPersonality {
                index: 2,
                footprint: 3,
                description: Some("1 px RGB".into()),
            },
        ],
        slots: Vec::new(),
        supported_parameters: vec![0x0082, 0x00E0, 0x00E1, 0x00F0],
        parameter_descriptions: Vec::new(),
        errors: Vec::new(),
    }
}

impl MockBackend {
    /// A fake installation for app development: one Pixelmapper Node with 4 ports, three tubes on ports 1 and 2.
    pub fn demo() -> Self {
        let mock = Self::new();
        let node = sample_node("7FF0:8A1B2C3D", Ipv4Addr::new(2, 27, 44, 61), 4);
        let node_id = node.node_id.clone();
        mock.upsert_node(node);
        let tubes = [
            (PortRef::new(1, 0), RdmUid::new(0x7FF0, 0x74B6_510C), 1),
            (PortRef::new(1, 0), RdmUid::new(0x7FF0, 0x74B6_510D), 31),
            (PortRef::new(2, 0), RdmUid::new(0x7FF0, 0x74B6_5200), 1),
        ];
        for (port, uid, start) in tubes {
            let description = sample_tube(uid, start);
            mock.set_rdm(
                uid,
                0x0060,
                MockRdm::Ack {
                    data: Vec::new(),
                    value: Some(RdmValue::DeviceInfo(Box::new(
                        description.device_info.clone(),
                    ))),
                },
            );
            mock.set_rdm(
                uid,
                0x00F0,
                MockRdm::Ack {
                    data: Vec::new(),
                    value: Some(RdmValue::Integer(start.into())),
                },
            );
            mock.set_description(description);
            let mut s = mock.inner.lock();
            let tod = s
                .tods
                .entry((node_id.clone(), port))
                .or_insert_with(|| Tod {
                    node_id: node_id.clone(),
                    port,
                    port_address: u16::from(port.bind),
                    available: true,
                    uids: Vec::new(),
                });
            tod.uids.push(uid);
            tod.uids.sort();
        }
        mock
    }
}
