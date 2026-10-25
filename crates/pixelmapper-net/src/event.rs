//! Events published by the [`crate::controller::Controller`] (one broadcast stream for all subscribers).

use crate::artnet::PortAddress;
use crate::flash::FlashProgress;
use crate::rdm::pd::StatusMessage;
use crate::rdm::Uid;
use crate::state::{NodeId, NodeState};
use serde::{Serialize, Serializer};
use std::net::SocketAddr;

pub(crate) fn ser_hex<S: Serializer>(b: &[u8], s: S) -> Result<S::Ok, S::Error> {
    s.collect_str(&crate::to_hex(b))
}

/// One message fetched with GET QUEUED_MESSAGE: the original response (PID, CC) of a changed parameter, or a
/// STATUS_MESSAGES list.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct QueuedMessage {
    /// PID of the queued response (e.g. PANEL_LOCK, BOOT_CATCH_STATUS, STATUS_MESSAGES).
    pub pid: u16,
    /// Standard or manufacturer PID name, if known.
    pub pid_name: Option<&'static str>,
    /// Command class of the queued response (GET_RESPONSE / SET_RESPONSE).
    pub command_class: u8,
    /// Sub-device.
    pub sub_device: u16,
    /// Parameter data (serialized as hex).
    #[serde(serialize_with = "ser_hex")]
    pub data: Vec<u8>,
    /// Decoded list if `pid` is STATUS_MESSAGES.
    pub status_messages: Option<Vec<StatusMessage>>,
}

/// Something changed. Serialized with a `"type"` tag, fields in camelCase.
#[derive(Debug, Clone, Serialize)]
#[serde(
    tag = "type",
    rename_all = "camelCase",
    rename_all_fields = "camelCase"
)]
pub enum Event {
    /// A node appeared or its state changed (ArtPollReply content differs from before).
    NodeUpdated {
        /// New state.
        node: NodeState,
    },
    /// A node did not answer polls for the configured timeout.
    NodeGone {
        /// Its id.
        id: NodeId,
    },
    /// The TOD of a port changed (complete set after all ArtTodData blocks arrived).
    TodChanged {
        /// Node.
        node: NodeId,
        /// Port-Address.
        port_address: PortAddress,
        /// All UIDs (sorted).
        uids: Vec<Uid>,
        /// UIDs that are new.
        added: Vec<Uid>,
        /// UIDs that disappeared.
        removed: Vec<Uid>,
        /// The node answered TodNak (TOD not available / discovery incomplete).
        nak: bool,
    },
    /// A queued message was fetched (automatic when a response had Message Count > 0).
    QueuedMessage {
        /// Node the device hangs on.
        node: NodeId,
        /// Port-Address used.
        port_address: PortAddress,
        /// Device UID (the node's own UID for node settings).
        uid: Uid,
        /// The message.
        message: QueuedMessage,
    },
    /// ArtDiagData from a node.
    Diag {
        /// Node, if the sender is known.
        node: Option<NodeId>,
        /// Sender address.
        address: SocketAddr,
        /// DiagPriority.
        priority: u8,
        /// Logical port (0 = general).
        logical_port: u8,
        /// Text.
        text: String,
    },
    /// Progress of a firmware job.
    FlashProgress {
        /// Snapshot.
        progress: FlashProgress,
    },
}
