//! The hub: one task that mirrors the backend's state from its events and fans every event out to the
//! handshaken sessions. Snapshot-like results (`hello`, `nodes`, `artAddress`/`artInput` node results) are
//! produced inside this task as well, so they are ordered with the events on every connection's queue
//! (contract §1 Ordering: a result reflects every event sent before it).

use super::outbound::{ConnTx, Outgoing};
use crate::backend::{NetBackend, Snapshot};
use crate::protocol::messages::{result_json, Empty, NodeResult, NodesResult};
use crate::protocol::{BridgeStatus, Event, LogEntry, LogLevel, NodeId, NodeState, PortRef, Tod};
use std::collections::{BTreeMap, HashMap};
use std::sync::{Arc, RwLock, RwLockReadGuard, RwLockWriteGuard};
use tokio::sync::broadcast::error::RecvError;
use tokio::sync::{mpsc, oneshot};

// ── Mirror ────────────────────────────────────────────────────────────────────────────────────────────────────────

/// The server's copy of the backend state, built from `snapshot()` and kept current from events.
#[derive(Debug, Clone)]
pub struct Mirror {
    /// Interfaces and UDP state (for `hello`).
    pub status: BridgeStatus,
    /// Node table.
    pub nodes: BTreeMap<NodeId, NodeState>,
    /// Last complete TOD per port.
    pub tods: BTreeMap<(NodeId, PortRef), Tod>,
}

impl Mirror {
    /// Mirror of a backend snapshot.
    pub fn new(status: BridgeStatus, snapshot: Snapshot) -> Self {
        let mut m = Self {
            status,
            nodes: BTreeMap::new(),
            tods: BTreeMap::new(),
        };
        for node in snapshot.nodes {
            m.nodes.insert(node.node_id.clone(), node);
        }
        for tod in snapshot.tods {
            m.tods.insert((tod.node_id.clone(), tod.port), tod);
        }
        m
    }

    /// Applies an event; returns false when it must not be forwarded (a `node` that only changed `lastSeen` or the
    /// NodeReport counter, a `nodeGone` for a node nobody was told about).
    pub fn apply(&mut self, event: &Event) -> bool {
        match event {
            Event::BridgeStatus(status) => {
                self.status = status.clone();
                true
            }
            Event::Node { node } => match self.nodes.get_mut(&node.node_id) {
                Some(old) if old.same_state(node) => {
                    old.refresh_volatile(node);
                    false
                }
                _ => {
                    self.nodes.insert(node.node_id.clone(), node.clone());
                    true
                }
            },
            Event::NodeGone { node_id, .. } => {
                self.tods.retain(|(n, _), _| n != node_id);
                self.nodes.remove(node_id).is_some()
            }
            Event::Tod(tod) => {
                self.tods
                    .insert((tod.node_id.clone(), tod.port), tod.clone());
                true
            }
            Event::RdmQueued(_) | Event::Diag(_) | Event::FlashProgress { .. } | Event::Log(_) => {
                true
            }
        }
    }

    /// Events that turn this mirror into the given backend state (after the event stream lagged).
    pub fn resync_events(&self, status: BridgeStatus, snapshot: Snapshot) -> Vec<Event> {
        let mut events = Vec::new();
        if status != self.status {
            events.push(Event::BridgeStatus(status));
        }
        let fresh: BTreeMap<&NodeId, &NodeState> =
            snapshot.nodes.iter().map(|n| (&n.node_id, n)).collect();
        for (id, old) in &self.nodes {
            if !fresh.contains_key(id) {
                events.push(Event::NodeGone {
                    node_id: id.clone(),
                    last_seen: old.last_seen,
                });
            }
        }
        for node in &snapshot.nodes {
            if self
                .nodes
                .get(&node.node_id)
                .is_none_or(|old| !old.same_state(node))
            {
                events.push(Event::Node { node: node.clone() });
            }
        }
        for tod in snapshot.tods {
            if self.tods.get(&(tod.node_id.clone(), tod.port)) != Some(&tod) {
                events.push(Event::Tod(tod));
            }
        }
        events
    }

    /// The `nodes` result.
    pub fn nodes_result(&self) -> NodesResult {
        NodesResult {
            nodes: self.nodes.values().cloned().collect(),
            tods: self.tods.values().cloned().collect(),
        }
    }
}

/// Mirror shared between the hub (writer) and the connections (readers, for argument checks).
#[derive(Debug, Clone)]
pub struct SharedMirror(Arc<RwLock<Mirror>>);

impl SharedMirror {
    /// Read access; a poisoned lock is still usable (the mirror is replaced field by field, never left torn).
    pub fn read(&self) -> RwLockReadGuard<'_, Mirror> {
        self.0.read().unwrap_or_else(|e| e.into_inner())
    }

    /// Write access (hub only).
    fn write(&self) -> RwLockWriteGuard<'_, Mirror> {
        self.0.write().unwrap_or_else(|e| e.into_inner())
    }
}

// ── Messages ──────────────────────────────────────────────────────────────────────────────────────────────────────

/// Requests from connections to the hub.
#[derive(Debug)]
pub enum HubMsg {
    /// Client `hello` accepted: answer `result {}` for `id`, then start forwarding events; `ack` when queued.
    Register {
        /// The connection.
        conn: ConnTx,
        /// Id of the `hello` command.
        id: u32,
        /// Signalled once the result is queued.
        ack: oneshot::Sender<()>,
    },
    /// Connection closed.
    Unregister(u64),
    /// Answer `nodes` from the mirror.
    Snapshot {
        /// The connection.
        conn: ConnTx,
        /// Command id.
        id: u32,
    },
    /// Answer `artAddress`/`artInput` with the node state: the newer of the backend's answer and the mirror.
    NodeResult {
        /// The connection.
        conn: ConnTx,
        /// Command id.
        id: u32,
        /// State from the node's reply.
        node: NodeState,
    },
}

/// Handle to send requests to the hub.
pub type HubTx = mpsc::UnboundedSender<HubMsg>;

// ── Hub task ──────────────────────────────────────────────────────────────────────────────────────────────────────

/// The hub's state.
struct Hub<B> {
    backend: Arc<B>,
    mirror: SharedMirror,
    conns: HashMap<u64, ConnTx>,
}

/// Subscribes to the backend, builds the mirror and starts the hub task.
pub fn start<B: NetBackend>(backend: Arc<B>) -> (HubTx, SharedMirror) {
    // Subscribe first, then snapshot: events in between are applied twice, which is harmless (full states).
    let events = backend.subscribe();
    let mirror = SharedMirror(Arc::new(RwLock::new(Mirror::new(
        backend.status(),
        backend.snapshot(),
    ))));
    let (tx, rx) = mpsc::unbounded_channel();
    let hub = Hub {
        backend,
        mirror: mirror.clone(),
        conns: HashMap::new(),
    };
    tokio::spawn(hub.run(events, rx));
    (tx, mirror)
}

impl<B: NetBackend> Hub<B> {
    /// Runs until every `HubTx` is dropped (bridge and all its sessions gone).
    async fn run(
        mut self,
        mut events: tokio::sync::broadcast::Receiver<Event>,
        mut rx: mpsc::UnboundedReceiver<HubMsg>,
    ) {
        let mut events_open = true;
        loop {
            tokio::select! {
                biased;
                ev = events.recv(), if events_open => match ev {
                    Ok(ev) => self.on_event(ev),
                    Err(RecvError::Lagged(n)) => self.resync(n),
                    Err(RecvError::Closed) => {
                        tracing::warn!("backend event stream closed");
                        events_open = false;
                    }
                },
                msg = rx.recv() => match msg {
                    Some(msg) => self.on_msg(msg),
                    None => break,
                },
            }
        }
    }

    /// Updates the mirror and forwards the event to every registered connection.
    fn on_event(&mut self, event: Event) {
        if !self.mirror.write().apply(&event) {
            return;
        }
        let text = match serde_json::to_string(&event) {
            Ok(t) => t,
            Err(e) => {
                tracing::error!("event not serializable: {e}");
                return;
            }
        };
        let msg = Outgoing::Text(text);
        // A full queue closes that connection (slow-client policy); it is dropped from the fan-out list.
        self.conns.retain(|_, conn| conn.send(msg.clone()));
    }

    /// The backend's event stream overflowed: re-read its state and send the differences as events.
    fn resync(&mut self, lost: u64) {
        tracing::warn!("event stream lagged by {lost} events, resynchronizing");
        let (status, snapshot) = (self.backend.status(), self.backend.snapshot());
        let events = self.mirror.read().resync_events(status, snapshot);
        for event in events {
            self.on_event(event);
        }
        let text = format!(
            "bridge event stream overflowed ({lost} events lost); node state resynchronized"
        );
        self.on_event(Event::Log(LogEntry::new(LogLevel::Warn, text)));
    }

    /// Handles a request from a connection.
    fn on_msg(&mut self, msg: HubMsg) {
        match msg {
            HubMsg::Register { conn, id, ack } => {
                if conn.send_text(result_json(id, &Empty {})) {
                    self.conns.insert(conn.id(), conn);
                }
                let _ = ack.send(());
            }
            HubMsg::Unregister(id) => {
                self.conns.remove(&id);
            }
            HubMsg::Snapshot { conn, id } => {
                let result = self.mirror.read().nodes_result();
                conn.send_text(result_json(id, &result));
            }
            HubMsg::NodeResult { conn, id, node } => {
                let node = match self.mirror.read().nodes.get(&node.node_id) {
                    Some(mirrored) if mirrored.last_seen >= node.last_seen => mirrored.clone(),
                    _ => node,
                };
                conn.send_text(result_json(id, &NodeResult { node }));
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::backend::mock::{sample_node, MockBackend};
    use std::net::Ipv4Addr;

    #[test]
    fn mirror_filters_and_resyncs() {
        let status = MockBackend::new().status();
        let mut m = Mirror::new(status.clone(), Snapshot::default());
        let node = sample_node("7FF0:00000001", Ipv4Addr::LOCALHOST, 1);
        assert!(m.apply(&Event::Node { node: node.clone() }));
        let mut later = node.clone();
        later.last_seen += 1000;
        assert!(
            !m.apply(&Event::Node {
                node: later.clone()
            }),
            "lastSeen-only change is not forwarded"
        );
        assert_eq!(m.nodes[&node.node_id].last_seen, later.last_seen);
        let mut counted = later.clone();
        counted.node_report = "#0001 [0002] OK".into();
        assert!(
            !m.apply(&Event::Node {
                node: counted.clone()
            }),
            "a NodeReport counter-only change is not forwarded"
        );
        assert_eq!(m.nodes[&node.node_id].node_report, "#0001 [0002] OK");
        let mut report = counted.clone();
        report.node_report = "#000e [0003] Firmware update failed".into();
        assert!(
            m.apply(&Event::Node { node: report }),
            "a new report text is"
        );
        let mut renamed = later.clone();
        renamed.short_name = "x".into();
        assert!(m.apply(&Event::Node {
            node: renamed.clone()
        }));

        let other = sample_node("7FF0:00000002", Ipv4Addr::LOCALHOST, 1);
        let snapshot = Snapshot {
            nodes: vec![other.clone()],
            tods: vec![],
        };
        let events = m.resync_events(status, snapshot);
        assert_eq!(events.len(), 2);
        assert!(matches!(&events[0], Event::NodeGone { node_id, .. } if *node_id == node.node_id));
        assert!(matches!(&events[1], Event::Node { node } if node.node_id == other.node_id));

        assert!(m.apply(&Event::NodeGone {
            node_id: node.node_id.clone(),
            last_seen: 0
        }));
        assert!(!m.apply(&Event::NodeGone {
            node_id: node.node_id.clone(),
            last_seen: 0
        }));
    }
}
