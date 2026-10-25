//! A backend without network access: no interfaces, the UDP socket reported as unbound, no nodes. `serve` runs
//! with it until the `pixelmapper-net` adapter exists, so the app can already connect and handshake.

use super::{
    BackendError, DmxFrame, FlashRequest, NetBackend, RdmAddress, RdmRequest, RdmResponse, Snapshot,
};
use crate::protocol::{
    ArtAddressArgs, BridgeStatus, Event, FirmwareImageInfo, FlashJob, IpProgArgs, IpProgReply,
    NodeId, NodeState, PortRef, RdmDeviceDescription, TodAction, UdpState,
};
use std::net::Ipv4Addr;
use tokio::sync::broadcast;

/// Reason reported in `udp.error`.
pub const OFFLINE_REASON: &str =
    "no Art-Net backend in this build (pixelmapper-net adapter pending)";

/// Backend that never talks to the network.
#[derive(Debug)]
pub struct OfflineBackend {
    /// Kept so receivers stay open (they just never get an event).
    events: broadcast::Sender<Event>,
}

impl OfflineBackend {
    /// Creates the backend.
    pub fn new() -> Self {
        Self {
            events: broadcast::channel(1).0,
        }
    }
}

impl Default for OfflineBackend {
    fn default() -> Self {
        Self::new()
    }
}

impl NetBackend for OfflineBackend {
    fn status(&self) -> BridgeStatus {
        BridgeStatus {
            interfaces: Vec::new(),
            udp: UdpState {
                bound: false,
                port: 6454,
                error: Some(OFFLINE_REASON.to_owned()),
            },
        }
    }

    fn snapshot(&self) -> Snapshot {
        Snapshot::default()
    }

    fn subscribe(&self) -> broadcast::Receiver<Event> {
        self.events.subscribe()
    }

    fn poll_now(&self) -> Result<(), BackendError> {
        Err(BackendError::UdpUnavailable)
    }

    fn set_poll_targets(&self, _targets: &[Ipv4Addr]) {}

    async fn tod_request(&self, node: NodeId, _port: PortRef) -> Result<(), BackendError> {
        Err(BackendError::UnknownNode(node))
    }

    async fn tod_control(
        &self,
        node: NodeId,
        _port: PortRef,
        _action: TodAction,
    ) -> Result<(), BackendError> {
        Err(BackendError::UnknownNode(node))
    }

    async fn rdm_get(&self, req: RdmRequest) -> Result<RdmResponse, BackendError> {
        Err(BackendError::UnknownNode(req.address.node_id))
    }

    async fn rdm_set(&self, req: RdmRequest) -> Result<RdmResponse, BackendError> {
        Err(BackendError::UnknownNode(req.address.node_id))
    }

    async fn rdm_describe(
        &self,
        address: RdmAddress,
        _refresh: bool,
    ) -> Result<RdmDeviceDescription, BackendError> {
        Err(BackendError::UnknownNode(address.node_id))
    }

    async fn art_address(&self, req: ArtAddressArgs) -> Result<NodeState, BackendError> {
        Err(BackendError::UnknownNode(req.node_id))
    }

    async fn ip_prog(&self, req: IpProgArgs) -> Result<IpProgReply, BackendError> {
        Err(BackendError::UnknownNode(req.node_id))
    }

    async fn art_input(
        &self,
        node: NodeId,
        _bind: u8,
        _disabled: Vec<bool>,
    ) -> Result<NodeState, BackendError> {
        Err(BackendError::UnknownNode(node))
    }

    fn image_info(&self, _hex: &str) -> Result<FirmwareImageInfo, BackendError> {
        Err(BackendError::Internal(OFFLINE_REASON.to_owned()))
    }

    async fn flash_start(&self, req: FlashRequest) -> Result<FlashJob, BackendError> {
        Err(BackendError::UnknownNode(req.address.node_id))
    }

    fn flash_cancel(&self, job_id: &str) -> Result<(), BackendError> {
        Err(BackendError::UnknownJob(job_id.to_owned()))
    }

    fn flash_jobs(&self) -> Vec<FlashJob> {
        Vec::new()
    }

    fn send_dmx(
        &self,
        _frames: &[DmxFrame],
        _sync: bool,
        _physical: u8,
    ) -> Result<(), BackendError> {
        Err(BackendError::UdpUnavailable)
    }
}
