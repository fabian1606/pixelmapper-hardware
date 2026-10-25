//! `poll` and `tod`.

use crate::net;
use crate::out::{to_value, Out};
use crate::Global;
use pixelmapper_net::artnet::PortAddress;
use pixelmapper_net::state::{MergeMode, OutputStyle, PortDirection};
use pixelmapper_net::{Event, NodeState, Result};
use serde_json::json;
use std::time::{Duration, Instant};
use tokio::sync::broadcast::error::RecvError;

/// Human text of a node with its ports.
pub fn node_text(n: &NodeState) -> String {
    let uid = n.uid.map(|u| u.to_string()).unwrap_or_else(|| "-".into());
    let mut s = format!(
        "{} \"{}\" ({})\n  ip {}  udp {}  uid {}  fw {}.{}  mac {}  oem 0x{:04X}",
        n.id,
        n.short_name,
        n.long_name,
        n.ip,
        n.address,
        uid,
        n.firmware_version >> 8,
        n.firmware_version & 0xFF,
        pixelmapper_net::state::format_mac(&n.mac),
        n.oem
    );
    s.push_str(&format!(
        "\n  indicators {:?}  failsafe {:?}  dhcp {}",
        n.status1.indicators, n.status3.failsafe, n.status2.dhcp_active
    ));
    for p in &n.ports {
        let dir = match p.direction {
            PortDirection::Output => "out",
            PortDirection::Input => "in ",
            PortDirection::None => "-  ",
        };
        let mut flags = vec![
            if p.rdm_enabled { "rdm" } else { "no-rdm" },
            match p.merge_mode {
                MergeMode::Htp => "htp",
                MergeMode::Ltp => "ltp",
            },
            match p.output_style {
                OutputStyle::Delta => "delta",
                OutputStyle::Continuous => "const",
            },
        ];
        if p.merging {
            flags.push("merging");
        }
        if p.data_active {
            flags.push("data");
        }
        if p.input_disabled {
            flags.push("input-disabled");
        }
        s.push_str(&format!(
            "\n  port {} (bind {})  {dir}  {:<9} ({:>5})  {}",
            p.port_number,
            p.bind_index,
            p.port_address.to_string(),
            p.port_address.raw(),
            flags.join(" ")
        ));
    }
    s
}

/// `poll [--watch]`.
pub async fn poll(g: &Global, o: &Out, watch: bool) -> Result<()> {
    let ctrl = net::controller(g).await?;
    let mut events = ctrl.subscribe();
    let nodes = net::discover_all(&ctrl, g).await?;
    if nodes.is_empty() {
        o.note("no nodes found");
    }
    for n in &nodes {
        o.emit(json!({"type": "node", "node": to_value(n)}), || {
            node_text(n)
        });
    }
    if !watch {
        ctrl.shutdown();
        return Ok(());
    }
    // drop the discovery events already printed as nodes
    while events.try_recv().is_ok() {}
    loop {
        match events.recv().await {
            Ok(ev) => o.emit(to_value(&ev), || event_text(&ev)),
            Err(RecvError::Lagged(n)) => o.note(&format!("({n} events lost)")),
            Err(RecvError::Closed) => return Ok(()),
        }
    }
}

fn event_text(ev: &Event) -> String {
    match ev {
        Event::NodeUpdated { node } => format!("updated: {}", node_text(node)),
        Event::NodeGone { id } => format!("gone: {id}"),
        Event::TodChanged {
            node,
            port_address,
            uids,
            added,
            removed,
            nak,
        } => format!(
            "tod {node} {port_address}: {} device(s){}{}{}",
            uids.len(),
            list(" +", added),
            list(" -", removed),
            if *nak { " (NAK)" } else { "" }
        ),
        Event::QueuedMessage {
            uid, message: m, ..
        } => format!(
            "queued message from {uid}: {} cc 0x{:02X} data {}",
            m.pid_name
                .map(str::to_string)
                .unwrap_or_else(|| format!("0x{:04X}", m.pid)),
            m.command_class,
            pixelmapper_net::to_hex(&m.data)
        ),
        Event::Diag { address, text, .. } => format!("diag {address}: {text}"),
        Event::FlashProgress { progress: p } => format!(
            "flash job {}: {:?} {}/{} {}",
            p.job_id, p.phase, p.done, p.total, p.message
        ),
    }
}

fn list<T: std::fmt::Display>(prefix: &str, v: &[T]) -> String {
    v.iter().map(|x| format!("{prefix}{x}")).collect()
}

/// `tod [--flush]`.
pub async fn tod(g: &Global, o: &Out, flush: bool) -> Result<()> {
    let ctrl = net::controller(g).await?;
    let node = net::select_node(&ctrl, g).await?;
    let ports: Vec<PortAddress> = match net::explicit_port(g, &node)? {
        Some(pa) => vec![pa],
        None => net::rdm_ports(&node),
    };
    let timeout = Duration::from_millis(g.timeout_ms.unwrap_or(2000));
    if flush {
        let mut events = ctrl.subscribe();
        for pa in &ports {
            ctrl.tod_flush(&node.id, *pa).await?;
        }
        // the node answers at once and again after the discovery; collect until every port reported a change
        let mut seen = std::collections::BTreeSet::new();
        let deadline = Instant::now() + timeout;
        while seen.len() < ports.len() {
            let left = deadline.saturating_duration_since(Instant::now());
            match tokio::time::timeout(left, events.recv()).await {
                Ok(Ok(Event::TodChanged {
                    node: id,
                    port_address,
                    nak: false,
                    ..
                })) if id == node.id => {
                    seen.insert(port_address);
                }
                Ok(Ok(_)) | Ok(Err(RecvError::Lagged(_))) => {}
                Ok(Err(RecvError::Closed)) | Err(_) => break,
            }
        }
    }
    for pa in &ports {
        ctrl.tod_request(&node.id, *pa).await?;
    }
    let deadline = Instant::now() + timeout;
    while Instant::now() < deadline && ports.iter().any(|pa| ctrl.tod(&node.id, *pa).is_none()) {
        tokio::time::sleep(Duration::from_millis(30)).await;
    }
    for pa in &ports {
        let port_number = node.port(*pa).map(|p| p.port_number);
        let uids = ctrl.tod(&node.id, *pa);
        o.emit(
            json!({
                "type": "tod",
                "node": node.id,
                "portAddress": pa,
                "portNumber": port_number,
                "uids": uids,
            }),
            || match &uids {
                Some(u) => {
                    let mut s = format!(
                        "port {} {pa}: {} device(s)",
                        port_number.unwrap_or(0),
                        u.len()
                    );
                    for x in u {
                        s.push_str(&format!("\n  {x}"));
                    }
                    s
                }
                None => format!("port {} {pa}: no TOD received", port_number.unwrap_or(0)),
            },
        );
    }
    ctrl.shutdown();
    Ok(())
}
