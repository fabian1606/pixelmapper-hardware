//! Binary DMX path: route table, frame sets, ArtSync flag, per-connection Physical, rate limit, refresh.

mod common;

use common::{config, eventually, mock_with_node, node, start, Client, NODE};
use pmnet_bridge::backend::mock::MockBackend;
use pmnet_bridge::backend::DmxTarget;
use pmnet_bridge::protocol::frame::encode_frame_set;
use pmnet_bridge::protocol::NodeId;
use serde_json::json;
use std::net::Ipv4Addr;
use std::time::{Duration, Instant};

#[tokio::test]
async fn frame_sets_follow_the_route_table() {
    let mock = mock_with_node();
    let server = start(&mock, config()).await;
    let mut a = Client::handshaken(server.local_addr()).await;

    // Binary before the handshake is ignored.
    let mut early = Client::connect(server.local_addr()).await;
    early
        .send_binary(encode_frame_set(&[(1, &[1, 2])], true))
        .await;

    let routes = json!({"routes": [
        {"route": 1, "nodeId": NODE, "portAddress": 1},
        {"route": 2, "ip": "2.0.0.50", "portAddress": 0x0102},
        {"route": 3, "nodeId": "7FF0:00000099", "portAddress": 3}
    ]});
    a.ok("dmxRoutes", routes).await;
    a.send_binary(encode_frame_set(
        &[(1, &[10, 11, 12]), (2, &[20; 512]), (3, &[30]), (9, &[90])],
        true,
    ))
    .await;

    let sent = eventually(|| mock.dmx_sent().into_iter().next()).await;
    assert!(sent.sync);
    assert_eq!(sent.physical, 0, "first connection");
    // Route 3's node is unknown (dropped by the backend), route 9 is not in the table.
    assert_eq!(sent.frames.len(), 2);
    assert_eq!(sent.frames[0].target, DmxTarget::Node(NodeId::from(NODE)));
    assert_eq!(
        (sent.frames[0].port_address, &sent.frames[0].data[..]),
        (1, &[10u8, 11, 12][..])
    );
    assert_eq!(
        sent.frames[1].target,
        DmxTarget::Ip(Ipv4Addr::new(2, 0, 0, 50))
    );
    assert_eq!(
        (sent.frames[1].port_address, sent.frames[1].data.len()),
        (0x0102, 512)
    );
    assert_eq!(
        mock.dmx_sent().len(),
        1,
        "the early connection's frame was ignored"
    );

    // Without the sync flag (may be held back by the 44 Hz limit for a moment).
    a.send_binary(encode_frame_set(&[(2, &[1, 2, 3, 4])], false))
        .await;
    eventually(|| (mock.dmx_sent().len() >= 2).then_some(())).await;
    // A further connection has its own Physical and its own (empty) table.
    let mut b = Client::handshaken(server.local_addr()).await;
    b.send_binary(encode_frame_set(&[(1, &[1])], true)).await;
    b.ok(
        "dmxRoutes",
        json!({"routes": [{"route": 1, "ip": "2.0.0.51", "portAddress": 7}]}),
    )
    .await;
    b.send_binary(encode_frame_set(&[(1, &[5, 6])], true)).await;
    let sets = eventually(|| {
        let s = mock.dmx_sent();
        (s.len() >= 3).then_some(s)
    })
    .await;
    assert!(!sets[1].sync);
    assert_eq!(sets[1].physical, 0);
    assert_eq!(
        sets[2].physical, 2,
        "third connection (the early one was #1)"
    );
    assert_eq!(
        sets[2].frames[0].target,
        DmxTarget::Ip(Ipv4Addr::new(2, 0, 0, 51))
    );
    server.shutdown().await;
}

#[tokio::test]
async fn route_validation_and_malformed_binary() {
    let mock = mock_with_node();
    let server = start(&mock, config()).await;
    let mut c = Client::handshaken(server.local_addr()).await;

    let e = c
        .err(
            "dmxRoutes",
            json!({"routes": [{"route": 1, "ip": "2.255.255.255", "portAddress": 1}]}),
        )
        .await;
    assert_eq!(
        (e["code"].as_str(), e["details"]["field"].as_str()),
        (Some("badRequest"), Some("routes[0].ip"))
    );
    let e = c
        .err(
            "dmxRoutes",
            json!({"routes": [{"route": 1, "nodeId": NODE, "ip": "2.0.0.9", "portAddress": 1}]}),
        )
        .await;
    assert_eq!(e["details"]["field"], "routes[0]");
    let e = c
        .err(
            "dmxRoutes",
            json!({"routes": [{"route": 0, "nodeId": NODE, "portAddress": 1}]}),
        )
        .await;
    assert_eq!(e["details"]["field"], "routes[0].route");

    c.send_binary(b"XX\x01\x00\x00\x00".to_vec()).await;
    let log = c.event("log").await;
    assert_eq!(log["level"], "warn");
    assert!(log["text"].as_str().unwrap().contains("magic"), "{log}");
    // Warnings are rate-limited; the connection stays usable.
    c.send_binary(b"PM\x01\x00\x01\x00\x01\x00\x09\x00".to_vec())
        .await;
    c.ok("nodes", json!({})).await;
    assert!(mock.dmx_sent().is_empty());
    server.shutdown().await;
}

#[tokio::test]
async fn rate_limit_coalesces_and_idle_routes_are_refreshed() {
    let mock = MockBackend::new();
    let mut slow_node = node();
    slow_node.refresh_rate = 10; // 100 ms between ArtDmx
    mock.upsert_node(slow_node);
    let server = start(&mock, config()).await; // refresh after 300 ms
    let mut c = Client::handshaken(server.local_addr()).await;
    c.ok(
        "dmxRoutes",
        json!({"routes": [{"route": 1, "nodeId": NODE, "portAddress": 1}]}),
    )
    .await;

    let start_at = Instant::now();
    c.send_binary(encode_frame_set(&[(1, &[1])], true)).await;
    c.send_binary(encode_frame_set(&[(1, &[2])], true)).await;
    c.send_binary(encode_frame_set(&[(1, &[3])], true)).await;

    // First set immediately; the two faster ones coalesce into one ArtDmx with the latest data.
    let sets = eventually(|| {
        let s = mock.dmx_sent();
        (s.len() >= 2).then_some(s)
    })
    .await;
    let coalesced_after = start_at.elapsed();
    assert_eq!(sets[0].frames[0].data[0], 1);
    assert_eq!(sets[1].frames[0].data[0], 3);
    assert!(
        coalesced_after >= Duration::from_millis(90),
        "rate limit not applied: {coalesced_after:?}"
    );

    // Idle: the last data is re-sent (with ArtSync) after the refresh period, and again after the next one.
    let sets = eventually(|| {
        let s = mock.dmx_sent();
        (s.len() >= 4).then_some(s)
    })
    .await;
    assert!(sets[2..4]
        .iter()
        .all(|s| s.sync && s.frames[0].data[0] == 3));
    assert!(start_at.elapsed() >= Duration::from_millis(600));

    // Closing the connection stops the refresh.
    drop(c);
    tokio::time::sleep(Duration::from_millis(100)).await;
    let n = mock.dmx_sent().len();
    tokio::time::sleep(Duration::from_millis(700)).await;
    assert_eq!(mock.dmx_sent().len(), n);
    server.shutdown().await;
}
