//! The narrow seam between the WebSocket server and the Art-Net 4 / RDM engine.
//!
//! The server never builds a packet. Everything that touches the network goes through [`NetBackend`]; the
//! server only handles sessions (handshake, per-connection DMX routes and refresh, poll-target reference counts,
//! event fan-out, argument validation against the node table it mirrors from the backend's events).
//!
//! Implementations:
//! - [`pixelmapper_net::PixelmapperNetBackend`]: the real Art-Net 4 / RDM backend on `crates/pixelmapper-net`;
//!   what `pmnet-bridge serve` runs by default.
//! - [`mock::MockBackend`]: scriptable, records every call; used by the tests and by `pmnet-bridge serve --mock`.
//! - [`offline::OfflineBackend`]: no network (`pmnet-bridge serve --offline`).

use crate::protocol::{
    ArtAddressArgs, BridgeStatus, CommandError, ErrorCode, Event, FirmwareImageInfo, FlashJob,
    IpProgArgs, IpProgReply, NodeId, NodeState, PortRef, RdmDeviceDescription, RdmUid, RdmValue,
    Tod, TodAction,
};
use bytes::Bytes;
use serde_json::Value;
use std::future::Future;
use std::net::Ipv4Addr;
use tokio::sync::broadcast;

pub mod mock;
pub mod offline;
pub mod pixelmapper_net;

// ── Requests and responses ────────────────────────────────────────────────────────────────────────────────────────

/// Addressing of an RDM request: node, one of its ports, device UID, sub-device (0 = root, 65535 = all).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RdmAddress {
    /// Node that forwards the request (ArtRdm to its IP, Port-Address of `port`).
    pub node_id: NodeId,
    /// Port of that node.
    pub port: PortRef,
    /// Destination device (may be the node's own UID, or a broadcast UID).
    pub uid: RdmUid,
    /// Sub-device.
    pub sub_device: u16,
}

/// Parameter data of an RDM request.
#[derive(Debug, Clone, PartialEq)]
pub enum RdmPayload {
    /// No parameter data.
    Empty,
    /// Raw parameter data.
    Data(Vec<u8>),
    /// Decoded value; the backend encodes it per the PID (contract §4 `RdmValue`) or fails with
    /// [`BackendError::InvalidArgument`] (`field: "value"`).
    Value(RdmValue),
}

/// One RDM GET or SET.
#[derive(Debug, Clone, PartialEq)]
pub struct RdmRequest {
    /// Destination.
    pub address: RdmAddress,
    /// Parameter id.
    pub pid: u16,
    /// Parameter data.
    pub payload: RdmPayload,
}

/// Final answer to an RDM request (after ACK_TIMER / ACK_OVERFLOW handling).
#[derive(Debug, Clone, PartialEq)]
pub struct RdmResponse {
    /// Raw parameter data of the ACK (empty for broadcasts).
    pub data: Vec<u8>,
    /// Decoded value when the PID is known to the backend.
    pub value: Option<RdmValue>,
    /// True when the request went to a broadcast UID and no response was awaited.
    pub broadcast: bool,
}

/// Start of a tube firmware job.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FlashRequest {
    /// Tube to update.
    pub address: RdmAddress,
    /// Intel HEX file content.
    pub hex: String,
    /// Allow an image older than the running firmware.
    pub allow_downgrade: bool,
}

/// Destination of DMX frames.
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub enum DmxTarget {
    /// A node from the node table; the backend sends to its current IP and drops frames while it is unknown.
    Node(NodeId),
    /// A fixed IP (manual target).
    Ip(Ipv4Addr),
}

/// One universe to send as ArtDmx.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DmxFrame {
    /// Destination.
    pub target: DmxTarget,
    /// 15-bit Port-Address.
    pub port_address: u16,
    /// 1–512 slot values, slot 1 first. Odd lengths are padded by the backend.
    pub data: Bytes,
}

/// Node table and complete TODs at one point in time.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct Snapshot {
    /// Every known node.
    pub nodes: Vec<NodeState>,
    /// The last complete TOD of every port that has one.
    pub tods: Vec<Tod>,
}

// ── Errors ────────────────────────────────────────────────────────────────────────────────────────────────────────

/// Failure of a backend operation; maps 1:1 onto the contract's error codes (§8).
#[derive(Debug, Clone, PartialEq, thiserror::Error)]
pub enum BackendError {
    /// `unknownNode`.
    #[error("unknown node {0}")]
    UnknownNode(NodeId),
    /// `unknownPort`.
    #[error("the node has no port {0}")]
    UnknownPort(PortRef),
    /// `timeout`: no answer after the retries.
    #[error("no answer from the network")]
    Timeout,
    /// `nack` with the E1.20 NACK reason.
    #[error("RDM NACK 0x{0:04X}")]
    Nack(u16),
    /// `badResponse`.
    #[error("bad response: {0}")]
    BadResponse(String),
    /// `invalidPortAddress`.
    #[error("invalid port-address: {0}")]
    InvalidPortAddress(String),
    /// `invalidImage`.
    #[error("invalid image: {0}")]
    InvalidImage(String),
    /// `deviceMismatch`.
    #[error("device does not match the image: {0}")]
    DeviceMismatch(String),
    /// `downgrade` with both version ids.
    #[error("image version 0x{image:08X} is older than the device firmware 0x{current:08X}")]
    Downgrade {
        /// Running firmware (`softwareVersionId`).
        current: u32,
        /// Image `versionId`.
        image: u32,
    },
    /// `busy`: a firmware job already runs on that port.
    #[error("a firmware job already runs on that port")]
    Busy,
    /// `unknownJob`.
    #[error("unknown job {0}")]
    UnknownJob(String),
    /// `udpUnavailable`.
    #[error("the Art-Net UDP socket is not bound")]
    UdpUnavailable,
    /// `badRequest` for arguments only the backend can check (e.g. a `value` that does not fit the PID).
    #[error("invalid {field}: {message}")]
    InvalidArgument {
        /// Argument name.
        field: String,
        /// Why.
        message: String,
    },
    /// `internal`.
    #[error("{0}")]
    Internal(String),
}

impl From<BackendError> for CommandError {
    fn from(e: BackendError) -> Self {
        let message = e.to_string();
        match e {
            BackendError::UnknownNode(_) => CommandError::new(ErrorCode::UnknownNode, message),
            BackendError::UnknownPort(_) => CommandError::new(ErrorCode::UnknownPort, message),
            BackendError::Timeout => CommandError::new(ErrorCode::Timeout, message),
            BackendError::Nack(reason) => CommandError::nack(reason),
            BackendError::BadResponse(_) => CommandError::new(ErrorCode::BadResponse, message),
            BackendError::InvalidPortAddress(_) => {
                CommandError::new(ErrorCode::InvalidPortAddress, message)
            }
            BackendError::InvalidImage(_) => CommandError::new(ErrorCode::InvalidImage, message),
            BackendError::DeviceMismatch(_) => {
                CommandError::new(ErrorCode::DeviceMismatch, message)
            }
            BackendError::Downgrade { current, image } => {
                CommandError::new(ErrorCode::Downgrade, message)
                    .with_detail("current", Value::from(current))
                    .with_detail("image", Value::from(image))
            }
            BackendError::Busy => CommandError::new(ErrorCode::Busy, message),
            BackendError::UnknownJob(_) => CommandError::new(ErrorCode::UnknownJob, message),
            BackendError::UdpUnavailable => CommandError::new(ErrorCode::UdpUnavailable, message),
            BackendError::InvalidArgument { field, message } => {
                CommandError::bad_request(field, message)
            }
            BackendError::Internal(_) => CommandError::new(ErrorCode::Internal, message),
        }
    }
}

// ── NetBackend ────────────────────────────────────────────────────────────────────────────────────────────────────

/// The Art-Net 4 / RDM engine as the bridge server sees it.
///
/// Rules for implementations (they make the contract's guarantees hold):
/// - **State only from the network** (last-writer-wins, plan D5): `snapshot()` and every [`Event`] report what
///   nodes and devices said, never what was requested.
/// - **Events** go to [`subscribe`](Self::subscribe) receivers in the order they happened. `Event::Node` carries
///   the full merged node state; it may also be sent when only `lastSeen` changed (the server filters those).
///   When a command's answer changes a node (`art_address`, `art_input`), emit the `Node` event *before*
///   returning. A node that leaves the table gets `Event::NodeGone`; its TODs are implicitly gone with it.
/// - **Non-blocking sync methods**: the plain `fn`s are called from connection tasks and must return quickly
///   (hand work to the engine's own tasks; never wait on the network).
/// - **Async methods** wait for the network answer including retries (RDM: 500 ms, 2 retries, ≤ 4 in flight
///   per port; ArtAddress/ArtIpProg/ArtInput: 1 s, one resend). They are called concurrently.
/// - **Jobs** (`flash_start`) belong to the backend and outlive the connection that started them.
pub trait NetBackend: Send + Sync + 'static {
    // ── State and events ──────────────────────────────────────────────────────────────────────────────────────

    /// Local interfaces and the Art-Net UDP socket state (`hello`, `bridgeStatus`).
    fn status(&self) -> BridgeStatus;

    /// Current node table and the last complete TOD of every port.
    fn snapshot(&self) -> Snapshot;

    /// A receiver for all future events. The server subscribes once and calls `snapshot()` right after; on lag it
    /// re-reads `snapshot()`/`status()` to resynchronize, so a capacity of a few hundred events is enough.
    fn subscribe(&self) -> broadcast::Receiver<Event>;

    // ── Discovery ─────────────────────────────────────────────────────────────────────────────────────────────

    /// Sends an ArtPoll now, on top of the 2.5–3 s cycle (`poll`). `UdpUnavailable` when the socket is unbound.
    fn poll_now(&self) -> Result<(), BackendError>;

    /// Replaces the set of extra unicast poll targets (`addPollTarget`/`removePollTarget`, reference-counted
    /// across connections by the server). Every ArtPoll is also unicast to each of them.
    fn set_poll_targets(&self, targets: &[Ipv4Addr]);

    // ── TOD ───────────────────────────────────────────────────────────────────────────────────────────────────

    /// ArtTodRequest for the port's Port-Address (this also subscribes the bridge to unsolicited ArtTodData).
    /// Returns once sent; the TOD arrives as `Event::Tod`. Independently of this command the backend sends one
    /// ArtTodRequest per port whenever a node appears or reappears (contract §5 `node`, node contract §6.1).
    fn tod_request(
        &self,
        node: NodeId,
        port: PortRef,
    ) -> impl Future<Output = Result<(), BackendError>> + Send;

    /// ArtTodControl with `AtcFlush` / `AtcEnd` / `AtcIncOn` / `AtcIncOff`. Returns once sent; after a flush the
    /// new TOD arrives as `Event::Tod` when the node's full discovery is done.
    fn tod_control(
        &self,
        node: NodeId,
        port: PortRef,
        action: TodAction,
    ) -> impl Future<Output = Result<(), BackendError>> + Send;

    // ── RDM ───────────────────────────────────────────────────────────────────────────────────────────────────

    /// RDM GET via ArtRdm. Broadcast UIDs return `broadcast: true` right after sending.
    fn rdm_get(
        &self,
        req: RdmRequest,
    ) -> impl Future<Output = Result<RdmResponse, BackendError>> + Send;

    /// RDM SET via ArtRdm (same rules as `rdm_get`).
    fn rdm_set(
        &self,
        req: RdmRequest,
    ) -> impl Future<Output = Result<RdmResponse, BackendError>> + Send;

    /// Reads everything the UI needs about one device (contract §6 `rdmDescribe`). Only a DEVICE_INFO failure is
    /// an error; other failures go into `errors`. `refresh` bypasses the description cache.
    fn rdm_describe(
        &self,
        address: RdmAddress,
        refresh: bool,
    ) -> impl Future<Output = Result<RdmDeviceDescription, BackendError>> + Send;

    // ── Node configuration ────────────────────────────────────────────────────────────────────────────────────

    /// ArtAddress packets for the requested changes; returns the node state from the reply to the last packet.
    /// The server has already checked node, ports and Port-Address rules against its node table.
    fn art_address(
        &self,
        req: ArtAddressArgs,
    ) -> impl Future<Output = Result<NodeState, BackendError>> + Send;

    /// ArtIpProg; returns the ArtIpProgReply.
    fn ip_prog(
        &self,
        req: IpProgArgs,
    ) -> impl Future<Output = Result<IpProgReply, BackendError>> + Send;

    /// ArtInput for one BindIndex (`disabled` has one entry per port of that BindIndex); returns the new state.
    fn art_input(
        &self,
        node: NodeId,
        bind: u8,
        disabled: Vec<bool>,
    ) -> impl Future<Output = Result<NodeState, BackendError>> + Send;

    // ── Firmware ──────────────────────────────────────────────────────────────────────────────────────────────

    /// Parses an Intel HEX tube image and its trailer (`imageInfo`). CPU only, no network.
    fn image_info(&self, hex: &str) -> Result<FirmwareImageInfo, BackendError>;

    /// Checks image and device (`invalidImage`, `deviceMismatch`, `downgrade`, `busy`) and starts the job. Progress
    /// arrives as `Event::FlashProgress`; the job keeps running when the requesting connection closes.
    fn flash_start(
        &self,
        req: FlashRequest,
    ) -> impl Future<Output = Result<FlashJob, BackendError>> + Send;

    /// Cancels a running job (it ends with phase `cancelled`). `UnknownJob` for unknown ids.
    fn flash_cancel(&self, job_id: &str) -> Result<(), BackendError>;

    /// Running jobs and jobs that finished in the last 10 minutes.
    fn flash_jobs(&self) -> Vec<FlashJob>;

    // ── DMX ───────────────────────────────────────────────────────────────────────────────────────────────────

    /// Sends one frame set: ArtDmx unicast per frame (odd lengths padded with one zero byte, sequence 1–255 per
    /// destination IP + Port-Address, `Physical` = `physical`), then one ArtSync — broadcast, as Art-Net 4
    /// requires — on every interface that carried ArtDmx of the set, when `sync` is set. Frames for unknown nodes
    /// are dropped silently. Rate limiting and coalescing already happened in the server. Must not block.
    ///
    /// `physical` is constant per WebSocket connection (its number mod 256): nodes tell merge sources apart by
    /// IP + Physical, so two app windows become two merge sources (contract §7).
    fn send_dmx(&self, frames: &[DmxFrame], sync: bool, physical: u8) -> Result<(), BackendError>;
}
