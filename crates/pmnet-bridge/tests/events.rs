//! Event fan-out, ordering against results, poll-target reference counting and the slow-client policy.

mod common;

use common::{config, eventually, mock_with_node, node, start, Client, Incoming, NODE};
use pmnet_bridge::backend::mock::MockBackend;
use pmnet_bridge::protocol::{Event, LogEntry, LogLevel, NodeId};
use pmnet_bridge::server::ServerConfig;
use serde_json::json;
use std::net::Ipv4Addr;
use std::time::Duration;

#[tokio::test]
async fn events_reach_every_handshaken_connection_only() {
    let mock = MockBackend::new();
    let server = start(&mock, config()).await;
    let mut a = Client::handshaken(server.local_addr()).await;
    let mut b = Client::handshaken(server.local_addr()).await;
    let mut pending = Client::connect(server.local_addr()).await;

    mock.upsert_node(node());
    for c in [&mut a, &mut b] {
        let ev = c.event("node").await;
        assert_eq!(ev["node"]["nodeId"], NODE);
        assert_eq!(ev["node"]["oem"], 255);
    }
    // Not handshaken: nothing but the hello result afterwards.
    assert!(pending.silent_for(Duration::from_millis(150)).await);
    let r = pending.request("hello", json!({"protocol": "1.0.0"})).await;
    assert_eq!(r["type"], "result");
    assert!(pending.silent_for(Duration::from_millis(100)).await);

    // A node event that only changes lastSeen is not forwarded; the next real event is.
    let mut seen_again = node();
    seen_again.last_seen += 5_000;
    mock.upsert_node(seen_again.clone());
    mock.emit(Event::Log(LogEntry::new(LogLevel::Info, "marker")));
    assert_eq!(a.next_json().await["type"], "log");
    seen_again.short_name = "Renamed".into();
    mock.upsert_node(seen_again);
    assert_eq!(a.next_json().await["node"]["shortName"], "Renamed");
    assert_eq!(pending.event("node").await["node"]["shortName"], "Renamed");
    server.shutdown().await;
}

#[tokio::test]
async fn snapshot_is_ordered_with_events() {
    let mock = mock_with_node();
    let server = start(&mock, config()).await;
    let mut c = Client::handshaken(server.local_addr()).await;
    assert_eq!(
        c.ok("nodes", json!({})).await["nodes"]
            .as_array()
            .unwrap()
            .len(),
        1
    );

    mock.remove_node(&NodeId::from(NODE));
    let id = c.send_command("nodes", json!({})).await;
    // nodeGone arrives before the snapshot, and the snapshot no longer contains the node or its TOD.
    let gone = c.next_json().await;
    assert_eq!(
        (gone["type"].as_str(), gone["nodeId"].as_str()),
        (Some("nodeGone"), Some(NODE))
    );
    let snap = c.reply(id).await;
    assert_eq!(snap["result"], json!({"nodes": [], "tods": []}));
    assert_eq!(
        c.err(
            "todRequest",
            json!({"nodeId": NODE, "port": {"bind": 1, "index": 0}})
        )
        .await["code"],
        "unknownNode"
    );
    server.shutdown().await;
}

#[tokio::test]
async fn poll_targets_are_reference_counted_per_connection() {
    let mock = MockBackend::new();
    let server = start(&mock, config()).await;
    let ip = Ipv4Addr::new(10, 0, 0, 7);
    let mut a = Client::handshaken(server.local_addr()).await;
    let mut b = Client::handshaken(server.local_addr()).await;

    a.ok("addPollTarget", json!({"ip": "10.0.0.7"})).await;
    a.ok("addPollTarget", json!({"ip": "10.0.0.7"})).await;
    b.ok("addPollTarget", json!({"ip": "10.0.0.7"})).await;
    b.ok("addPollTarget", json!({"ip": "10.0.0.8"})).await;
    assert_eq!(mock.poll_targets(), vec![ip, Ipv4Addr::new(10, 0, 0, 8)]);

    a.ok("removePollTarget", json!({"ip": "10.0.0.7"})).await;
    assert_eq!(
        mock.poll_targets(),
        vec![ip, Ipv4Addr::new(10, 0, 0, 8)],
        "b still uses it"
    );
    assert_eq!(
        a.err("addPollTarget", json!({"ip": "10.0.0"})).await["details"]["field"],
        "ip"
    );

    // Closing b drops its targets.
    drop(b);
    eventually(|| mock.poll_targets().is_empty().then_some(())).await;
    server.shutdown().await;
}

#[tokio::test]
async fn slow_client_is_closed_without_blocking_others() {
    let mock = MockBackend::new();
    let server = start(
        &mock,
        ServerConfig {
            outbound_capacity: 64,
            ..config()
        },
    )
    .await;
    let mut fast = Client::handshaken(server.local_addr()).await;
    let mut slow = Client::handshaken(server.local_addr()).await;

    // ~24 MB of events: far more than the loopback socket buffers plus the slow client's queue.
    let text = "x".repeat(4096);
    let total = 6_000;
    for batch in 0..total / 50 {
        for i in 0..50 {
            mock.emit(Event::Log(LogEntry::new(
                LogLevel::Debug,
                format!("{} {text}", batch * 50 + i),
            )));
        }
        // The fast client keeps up (it reads everything of this batch before the next one is emitted).
        for i in 0..50 {
            let ev = fast.next_json().await;
            assert!(ev["text"]
                .as_str()
                .unwrap()
                .starts_with(&format!("{} ", batch * 50 + i)));
        }
    }

    // The slow client gets what fit into the buffers, then the connection ends (4003, or dropped).
    let mut received = 0;
    loop {
        match slow.read().await {
            Incoming::Json(_) => received += 1,
            Incoming::Close(code) => {
                assert_eq!(code, Some(4003));
                break;
            }
            Incoming::Ended => break,
        }
    }
    assert!(received < total, "the slow client was not cut off");
    // The fast client is still fine.
    fast.ok("nodes", json!({})).await;
    server.shutdown().await;
}
