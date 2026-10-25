//! [`crate::link`] implementations on top of the [`Controller`].

use super::{Controller, RdmRequest};
use crate::artnet::{ArtFirmwareMaster, ArtFirmwareReply, PortAddress};
use crate::error::{Error, Result};
use crate::link::{NodeLink, RdmLink, RdmOptions};
use crate::rdm::Uid;
use crate::state::{NodeId, NodeState};
use std::future::Future;
use std::time::{Duration, Instant};

/// RDM to devices behind one node port.
#[derive(Clone)]
pub struct ControllerPortLink {
    ctrl: Controller,
    node: NodeId,
    pa: PortAddress,
}

impl ControllerPortLink {
    pub(crate) fn new(ctrl: Controller, node: NodeId, pa: PortAddress) -> Self {
        Self { ctrl, node, pa }
    }
}

impl RdmLink for ControllerPortLink {
    fn rdm(
        &self,
        uid: Uid,
        command_class: u8,
        pid: u16,
        data: Vec<u8>,
        opts: RdmOptions,
    ) -> impl Future<Output = Result<Vec<u8>>> + Send {
        let req = RdmRequest {
            uid,
            command_class,
            pid,
            data,
            options: opts,
        };
        async move { self.ctrl.rdm(&self.node, self.pa, req).await }
    }
}

/// Access to one node for its firmware update.
#[derive(Clone)]
pub struct ControllerNodeLink {
    port: ControllerPortLink,
    uid: Uid,
}

impl ControllerNodeLink {
    pub(crate) fn new(ctrl: Controller, node: NodeId, pa: PortAddress) -> Result<Self> {
        let uid = ctrl
            .node(&node)
            .and_then(|n| n.uid)
            .ok_or_else(|| Error::InvalidArgument(format!("node {node} reports no RDM UID")))?;
        Ok(Self {
            port: ControllerPortLink::new(ctrl, node, pa),
            uid,
        })
    }
}

impl RdmLink for ControllerNodeLink {
    fn rdm(
        &self,
        uid: Uid,
        command_class: u8,
        pid: u16,
        data: Vec<u8>,
        opts: RdmOptions,
    ) -> impl Future<Output = Result<Vec<u8>>> + Send {
        self.port.rdm(uid, command_class, pid, data, opts)
    }
}

impl NodeLink for ControllerNodeLink {
    fn node_uid(&self) -> Uid {
        self.uid
    }

    fn node_state(&self) -> Option<NodeState> {
        self.port.ctrl.node(&self.port.node)
    }

    async fn firmware_block(
        &self,
        block: ArtFirmwareMaster,
        timeout: Duration,
    ) -> Result<ArtFirmwareReply> {
        self.port
            .ctrl
            .firmware_block(&self.port.node, block, timeout)
            .await
    }

    async fn wait_for_version(&self, vers_info: u16, timeout: Duration) -> Result<NodeState> {
        let deadline = Instant::now() + timeout;
        loop {
            if let Some(n) = self.node_state() {
                if n.firmware_version == vers_info {
                    return Ok(n);
                }
            }
            if Instant::now() >= deadline {
                return Err(Error::WaitTimeout(timeout, "node firmware version"));
            }
            let _ = self.port.ctrl.poll_now().await;
            tokio::time::sleep(Duration::from_millis(250)).await;
        }
    }
}
