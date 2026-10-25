//! Controller setup, discovery, node and port selection.

use crate::Global;
use pixelmapper_net::artnet::PortAddress;
use pixelmapper_net::rdm::Uid;
use pixelmapper_net::state::PortDirection;
use pixelmapper_net::{Controller, ControllerConfig, Error, NodeId, NodeState, Result};
use std::net::{Ipv4Addr, SocketAddr};
use std::time::{Duration, Instant};

/// Bind a controller with the global options.
pub async fn controller(g: &Global) -> Result<Controller> {
    let mut cfg = ControllerConfig {
        bind: g.bind,
        reuse_port: !g.no_reuse_port,
        broadcast: g.broadcast.clone(),
        node_port: g.node_port,
        ..ControllerConfig::default()
    };
    if let Some(ms) = g.timeout_ms {
        let t = Duration::from_millis(ms);
        cfg.reply_timeout = t;
        cfg.rdm.timeout = t;
    }
    let ctrl = Controller::bind(cfg).await?;
    ctrl.poll_now().await?;
    Ok(ctrl)
}

/// How a `--node` argument selects a node.
enum Selector {
    Id(NodeId),
    Ip(Ipv4Addr),
    Addr(SocketAddr),
}

fn selector(s: &str) -> Result<Selector> {
    if let Ok(ip) = s.parse::<Ipv4Addr>() {
        return Ok(Selector::Ip(ip));
    }
    if let Ok(a) = s.parse::<SocketAddr>() {
        return Ok(Selector::Addr(a));
    }
    s.parse::<NodeId>().map(Selector::Id)
}

fn matches(sel: &Selector, n: &NodeState) -> bool {
    match sel {
        Selector::Id(id) => n.id == *id,
        Selector::Ip(ip) => n.ip == *ip || n.address.ip() == std::net::IpAddr::V4(*ip),
        Selector::Addr(a) => n.address == *a,
    }
}

fn wait_dur(g: &Global) -> Duration {
    Duration::from_millis(g.wait)
}

/// Poll for `--wait` ms and return all nodes found.
pub async fn discover_all(ctrl: &Controller, g: &Global) -> Result<Vec<NodeState>> {
    let deadline = Instant::now() + wait_dur(g);
    let mut next_poll = Instant::now() + Duration::from_millis(500);
    while Instant::now() < deadline {
        tokio::time::sleep(Duration::from_millis(50)).await;
        if Instant::now() >= next_poll {
            ctrl.poll_now().await?;
            next_poll = Instant::now() + Duration::from_millis(500);
        }
    }
    Ok(ctrl.nodes())
}

/// Find the node selected by `--node`, or the only node. With `--node` this returns as soon as it is found.
pub async fn select_node(ctrl: &Controller, g: &Global) -> Result<NodeState> {
    let Some(s) = &g.node else {
        let nodes = discover_all(ctrl, g).await?;
        return match nodes.len() {
            0 => Err(Error::UnknownNode(format!(
                "no node found within {} ms (targets: {})",
                g.wait,
                targets(ctrl)
            ))),
            1 => Ok(nodes.into_iter().next().ok_or(Error::Closed)?),
            _ => Err(Error::InvalidArgument(format!(
                "{} nodes found, select one with --node ({})",
                nodes.len(),
                nodes
                    .iter()
                    .map(|n| format!("{} @ {}", n.id, n.ip))
                    .collect::<Vec<_>>()
                    .join(", ")
            ))),
        };
    };
    let sel = selector(s)?;
    let deadline = Instant::now() + wait_dur(g);
    let mut next_poll = Instant::now() + Duration::from_millis(500);
    loop {
        if let Some(n) = ctrl.nodes().into_iter().find(|n| matches(&sel, n)) {
            return Ok(n);
        }
        if Instant::now() >= deadline {
            return Err(Error::UnknownNode(s.clone()));
        }
        tokio::time::sleep(Duration::from_millis(50)).await;
        if Instant::now() >= next_poll {
            ctrl.poll_now().await?;
            next_poll = Instant::now() + Duration::from_millis(500);
        }
    }
}

fn targets(ctrl: &Controller) -> String {
    ctrl.broadcast_targets()
        .iter()
        .map(|t| t.to_string())
        .collect::<Vec<_>>()
        .join(", ")
}

/// Port-Address given by `--port` / `--port-number`, checked against the node.
pub fn explicit_port(g: &Global, node: &NodeState) -> Result<Option<PortAddress>> {
    if let Some(p) = &g.port {
        let pa = parse_pa(p)?;
        if node.port(pa).is_none() {
            return Err(Error::UnknownPort {
                node: node.id.to_string(),
                port_address: pa.to_string(),
            });
        }
        return Ok(Some(pa));
    }
    if let Some(n) = g.port_number {
        return node
            .ports
            .iter()
            .find(|p| p.port_number == n)
            .map(|p| Some(p.port_address))
            .ok_or_else(|| Error::InvalidArgument(format!("node {} has no port {n}", node.id)));
    }
    Ok(None)
}

/// Parse a Port-Address (`N:S:U` or number).
pub fn parse_pa(s: &str) -> Result<PortAddress> {
    s.parse::<PortAddress>()
        .map_err(|_| Error::InvalidArgument(format!("bad Port-Address {s:?} (N:S:U or 0..32767)")))
}

/// Parse a UID.
pub fn parse_uid(s: &str) -> Result<Uid> {
    s.parse::<Uid>()
        .map_err(|_| Error::InvalidArgument(format!("bad UID {s:?} (MMMM:DDDDDDDD)")))
}

/// RDM-capable output ports of a node.
pub fn rdm_ports(node: &NodeState) -> Vec<PortAddress> {
    let mut v: Vec<PortAddress> = node
        .ports
        .iter()
        .filter(|p| p.direction == PortDirection::Output && p.rdm_enabled)
        .map(|p| p.port_address)
        .collect();
    v.dedup();
    v
}

/// Port to reach `uid`: `--port`, any port for the node's own UID, the port whose TOD lists the UID (TODs are
/// requested if needed), or the only RDM port.
pub async fn port_for_uid(
    ctrl: &Controller,
    g: &Global,
    node: &NodeState,
    uid: Uid,
) -> Result<PortAddress> {
    if let Some(pa) = explicit_port(g, node)? {
        return Ok(pa);
    }
    let rdm = rdm_ports(node);
    if Some(uid) == node.uid {
        return rdm
            .first()
            .or(node.ports.first().map(|p| &p.port_address))
            .copied()
            .ok_or_else(|| Error::InvalidArgument("node has no ports".into()));
    }
    let find = || {
        rdm.iter()
            .copied()
            .find(|pa| ctrl.tod(&node.id, *pa).is_some_and(|t| t.contains(&uid)))
    };
    if let Some(pa) = find() {
        return Ok(pa);
    }
    for pa in &rdm {
        ctrl.tod_request(&node.id, *pa).await?;
    }
    let deadline = Instant::now() + Duration::from_millis(g.timeout_ms.unwrap_or(1500));
    while Instant::now() < deadline {
        tokio::time::sleep(Duration::from_millis(50)).await;
        if let Some(pa) = find() {
            return Ok(pa);
        }
    }
    match rdm.as_slice() {
        [only] => Ok(*only),
        _ => Err(Error::InvalidArgument(format!(
            "{uid} is in no TOD of node {}; select the port with --port or --port-number",
            node.id
        ))),
    }
}
