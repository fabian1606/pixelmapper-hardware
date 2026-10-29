//! The transport-independent session API (`pmnet_bridge::bridge`) on the mock backend, without any WebSocket:
//! handshake, command round trip, events, DMX, the close paths and shutdown. Runs without the `server` feature.

use pmnet_bridge::backend::mock::{sample_node, MockBackend};
use pmnet_bridge::backend::DmxTarget;
use pmnet_bridge::bridge::{
    Bridge, BridgeConfig, ClientInfo, ClientSession, Outgoing, OutgoingRx, SessionClosed,
    CLOSE_GOING_AWAY, CLOSE_INCOMPATIBLE, CLOSE_NO_HELLO, CLOSE_TOO_SLOW,
};
use pmnet_bridge::protocol::frame::encode_frame_set;
use pmnet_bridge::protocol::{Event, LogEntry, LogLevel, NodeId};
use serde_json::{json, Value};
use std::net::Ipv4Addr;
use std::sync::Arc;
use std::time::Duration;

/// How long a test waits for any expected message.
const WAIT: Duration = Duration::from_secs(3);
/// Node id of the sample node.
const NODE: &str = "7FF0:8A1B2C3D";

// ── Helpers ───────────────────────────────────────────────────────────────────────────────────────────────────────

/// Test configuration: short DMX refresh.
fn config() -> BridgeConfig {
    BridgeConfig {
        dmx_refresh: Duration::from_millis(300),
        ..BridgeConfig::default()
    }
}

/// Mock with the sample node and its bridge.
fn bridge(config: BridgeConfig) -> (MockBackend, Bridge<MockBackend>) {
    let mock = MockBackend::new();
    mock.upsert_node(sample_node(NODE, Ipv4Addr::new(2, 27, 44, 61), 4));
    let bridge = Bridge::new(Arc::new(mock.clone()), config);
    (mock, bridge)
}

/// The next outgoing message within `WAIT`.
async fn next(rx: &mut OutgoingRx) -> Option<Outgoing> {
    tokio::time::timeout(WAIT, rx.recv())
        .await
        .expect("message in time")
}

/// The next outgoing message as JSON.
async fn next_json(rx: &mut OutgoingRx) -> Value {
    match next(rx).await {
        Some(Outgoing::Text(text)) => serde_json::from_str(&text).expect("JSON"),
        other => panic!("expected text, got {other:?}"),
    }
}

/// Sends a command and returns its reply (skipping events).
async fn request(
    session: &ClientSession,
    rx: &mut OutgoingRx,
    id: u32,
    kind: &str,
    args: Value,
) -> Value {
    let mut msg = args;
    msg["type"] = json!(kind);
    msg["id"] = json!(id);
    session
        .handle_text(&msg.to_string())
        .await
        .expect("session open");
    loop {
        let v = next_json(rx).await;
        if v["id"] == json!(id) && (v["type"] == "result" || v["type"] == "error") {
            return v;
        }
    }
}

/// Connects and completes the handshake.
async fn handshaken(bridge: &Bridge<MockBackend>) -> (ClientSession, OutgoingRx) {
    let (session, mut rx) = bridge.connect(ClientInfo::new("test"));
    assert_eq!(next_json(&mut rx).await["type"], "hello");
    let r = request(
        &session,
        &mut rx,
        1,
        "hello",
        json!({"client": "test", "clientVersion": "0", "protocol": "1.0.0"}),
    )
    .await;
    assert_eq!(r["type"], "result", "hello failed: {r}");
    (session, rx)
}

/// Polls `check` until it returns `Some` or `WAIT` elapses.
async fn eventually<T>(mut check: impl FnMut() -> Option<T>) -> T {
    let deadline = tokio::time::Instant::now() + WAIT;
    loop {
        if let Some(v) = check() {
            return v;
        }
        assert!(
            tokio::time::Instant::now() < deadline,
            "condition not reached"
        );
        tokio::time::sleep(Duration::from_millis(10)).await;
    }
}

// ── Tests ─────────────────────────────────────────────────────────────────────────────────────────────────────────

#[tokio::test]
async fn handshake_and_command_round_trip() {
    let (_mock, bridge) = bridge(config());
    let (session, mut rx) = bridge.connect(ClientInfo::new("test"));
    let hello = next_json(&mut rx).await;
    assert_eq!(hello["type"], "hello");
    assert_eq!(hello["protocol"], "1.0.0");

    // Commands before the handshake are refused.
    let r = request(&session, &mut rx, 7, "nodes", json!({})).await;
    assert_eq!(r["code"], "helloRequired");

    let r = request(&session, &mut rx, 8, "hello", json!({"protocol": "1.0.0"})).await;
    assert_eq!(r["type"], "result");
    let r = request(&session, &mut rx, 9, "nodes", json!({})).await;
    assert_eq!(r["result"]["nodes"][0]["nodeId"], NODE);
    let r = request(&session, &mut rx, 10, "bogus", json!({})).await;
    assert_eq!(r["code"], "unknownCommand");
    assert!(session.id() < 2);
}

#[tokio::test]
async fn events_reach_handshaken_sessions() {
    let (mock, bridge) = bridge(config());
    let (_a, mut a) = handshaken(&bridge).await;
    let (_b, mut b) = handshaken(&bridge).await;
    mock.emit(Event::Log(LogEntry::new(LogLevel::Info, "marker")));
    for rx in [&mut a, &mut b] {
        let ev = next_json(rx).await;
        assert_eq!(
            (ev["type"].as_str(), ev["text"].as_str()),
            (Some("log"), Some("marker"))
        );
    }
}

#[tokio::test]
async fn dmx_frame_sets_use_the_route_table_and_physical() {
    let (mock, bridge) = bridge(config());
    let (_first, _rx1) = handshaken(&bridge).await;
    let (session, mut rx) = handshaken(&bridge).await;
    let r = request(
        &session,
        &mut rx,
        2,
        "dmxRoutes",
        json!({"routes": [{"route": 1, "nodeId": NODE, "portAddress": 1}]}),
    )
    .await;
    assert_eq!(r["type"], "result", "{r}");
    session
        .handle_binary(encode_frame_set(&[(1, &[10, 11, 12])], true))
        .await
        .expect("open");
    let sent = eventually(|| mock.dmx_sent().into_iter().next()).await;
    assert!(sent.sync);
    assert_eq!(sent.physical, 1, "second session");
    assert_eq!(sent.frames[0].target, DmxTarget::Node(NodeId::from(NODE)));
    assert_eq!(&sent.frames[0].data[..], &[10, 11, 12]);
    // The idle route is refreshed by the session task.
    eventually(|| (mock.dmx_sent().len() >= 2).then_some(())).await;
}

#[tokio::test]
async fn client_close_ends_the_session_and_releases_poll_targets() {
    let (mock, bridge) = bridge(config());
    let (session, mut rx) = handshaken(&bridge).await;
    let r = request(
        &session,
        &mut rx,
        2,
        "addPollTarget",
        json!({"ip": "10.0.0.9"}),
    )
    .await;
    assert_eq!(r["type"], "result");
    assert_eq!(mock.poll_targets(), vec![Ipv4Addr::new(10, 0, 0, 9)]);
    session.close();
    assert!(session.is_closed());
    assert_eq!(
        next(&mut rx).await,
        None,
        "no close message for a client close"
    );
    assert_eq!(session.handle_text("{}").await, Err(SessionClosed));
    eventually(|| mock.poll_targets().is_empty().then_some(())).await;

    // Dropping the ClientSession has the same effect.
    let (session, mut rx) = handshaken(&bridge).await;
    drop(session);
    assert_eq!(next(&mut rx).await, None);
}

#[tokio::test]
async fn bridge_closes_with_codes() {
    // Incompatible protocol: error, then 4001.
    let (_mock, bridge) = bridge(BridgeConfig {
        hello_timeout: Duration::from_millis(200),
        ..config()
    });
    let (session, mut rx) = bridge.connect(ClientInfo::new("test"));
    next_json(&mut rx).await;
    let r = request(&session, &mut rx, 1, "hello", json!({"protocol": "2.0.0"})).await;
    assert_eq!(r["code"], "incompatibleProtocol");
    assert!(
        matches!(next(&mut rx).await, Some(Outgoing::Close { code, .. }) if code == CLOSE_INCOMPATIBLE)
    );
    assert_eq!(next(&mut rx).await, None);
    session.closed().await;

    // No hello: 4002.
    let (_session, mut rx) = bridge.connect(ClientInfo::new("test"));
    next_json(&mut rx).await;
    assert!(
        matches!(next(&mut rx).await, Some(Outgoing::Close { code, .. }) if code == CLOSE_NO_HELLO)
    );

    // Shutdown: 1001 for open sessions.
    let (_session, mut rx) = handshaken(&bridge).await;
    bridge.shutdown();
    assert!(
        matches!(next(&mut rx).await, Some(Outgoing::Close { code, .. }) if code == CLOSE_GOING_AWAY)
    );
}

#[tokio::test]
async fn unread_outgoing_queue_closes_with_4003() {
    let (mock, bridge) = bridge(BridgeConfig {
        outbound_capacity: 4,
        ..config()
    });
    let (_session, mut rx) = handshaken(&bridge).await;
    for i in 0..10 {
        mock.emit(Event::Log(LogEntry::new(LogLevel::Info, format!("{i}"))));
    }
    let close = loop {
        match next(&mut rx).await {
            Some(Outgoing::Text(_)) => continue,
            other => break other,
        }
    };
    assert!(matches!(close, Some(Outgoing::Close { code, .. }) if code == CLOSE_TOO_SLOW));
}
