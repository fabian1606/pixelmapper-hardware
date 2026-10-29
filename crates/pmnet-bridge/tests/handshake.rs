//! Connection level: `Origin` allow-list, `hello` in both directions, close codes, shutdown.

#![cfg(feature = "server")]

mod common;

use common::{config, mock_with_node, start, Client, NODE};
use pmnet_bridge::backend::mock::MockBackend;
use pmnet_bridge::server::origin::OriginPolicy;
use pmnet_bridge::server::ServerConfig;
use serde_json::json;
use std::time::Duration;
use tokio_tungstenite::tungstenite;

#[tokio::test]
async fn hello_event_then_client_hello_then_snapshot() {
    let mock = mock_with_node();
    let server = start(&mock, config()).await;
    let mut c = Client::connect(server.local_addr()).await;

    // The bridge's hello comes first and carries versions, interfaces and UDP state.
    assert_eq!(c.hello["type"], "hello");
    assert_eq!(c.hello["bridge"], env!("CARGO_PKG_VERSION"));
    assert_eq!(c.hello["protocol"], "1.0.0");
    assert_eq!(c.hello["interfaces"][0]["name"], "mock0");
    assert_eq!(c.hello["interfaces"][0]["ip"], "2.0.0.1");
    assert_eq!(
        c.hello["udp"],
        json!({"bound": true, "port": 6454, "error": null})
    );

    // Commands before the client hello are refused.
    let e = c.err("nodes", json!({})).await;
    assert_eq!(e["code"], "helloRequired");

    let r = c
        .request(
            "hello",
            json!({"client": "pixelmapper", "clientVersion": "1.2.3", "protocol": "1.4.0"}),
        )
        .await;
    assert_eq!(r, json!({"type": "result", "id": 2, "result": {}}));

    let snap = c.ok("nodes", json!({})).await;
    assert_eq!(snap["nodes"][0]["nodeId"], NODE);
    assert_eq!(snap["nodes"][0]["ports"].as_array().unwrap().len(), 4);
    assert_eq!(
        snap["nodes"][0]["ports"][3]["port"],
        json!({"bind": 4, "index": 0})
    );
    assert_eq!(snap["tods"][0]["uids"], json!(["7FF0:74B6510C"]));

    // A second hello is harmless.
    c.ok("hello", json!({"protocol": "1.0.0"})).await;
    server.shutdown().await;
}

#[tokio::test]
async fn incompatible_protocol_is_refused_with_4001() {
    let mock = MockBackend::new();
    let server = start(&mock, config()).await;
    let mut c = Client::connect(server.local_addr()).await;
    let e = c
        .err(
            "hello",
            json!({"client": "x", "clientVersion": "0", "protocol": "2.0.0"}),
        )
        .await;
    assert_eq!(e["code"], "incompatibleProtocol");
    assert_eq!(e["details"]["protocol"], "1.0.0");
    assert_eq!(c.closed().await, Some(4001));

    let mut c = Client::connect(server.local_addr()).await;
    let e = c.err("hello", json!({"protocol": "one"})).await;
    assert_eq!(e["code"], "incompatibleProtocol");
    assert_eq!(c.closed().await, Some(4001));

    // A hello without protocol is a bad request, the connection stays.
    let mut c = Client::connect(server.local_addr()).await;
    let e = c.err("hello", json!({"client": "x"})).await;
    assert_eq!(
        (e["code"].as_str(), e["details"]["field"].as_str()),
        (Some("badRequest"), Some("protocol"))
    );
    c.ok("hello", json!({"protocol": "1.0.0"})).await;
    server.shutdown().await;
}

#[tokio::test]
async fn missing_client_hello_closes_with_4002() {
    let mock = MockBackend::new();
    let server = start(
        &mock,
        ServerConfig {
            hello_timeout: Duration::from_millis(200),
            ..config()
        },
    )
    .await;
    let mut c = Client::connect(server.local_addr()).await;
    assert_eq!(c.closed().await, Some(4002));

    // A handshaken connection is not affected by the deadline.
    let mut c = Client::handshaken(server.local_addr()).await;
    tokio::time::sleep(Duration::from_millis(400)).await;
    c.ok("nodes", json!({})).await;
    server.shutdown().await;
}

#[tokio::test]
async fn origin_allow_list() {
    let mock = MockBackend::new();
    let origins = OriginPolicy::with_defaults(&["https://app.example.org"]).unwrap();
    let server = start(
        &mock,
        ServerConfig {
            origins,
            ..config()
        },
    )
    .await;
    let addr = server.local_addr();

    for allowed in [
        "http://localhost:3000",
        "http://127.0.0.1:5173",
        "tauri://localhost",
        "https://app.example.org",
    ] {
        let c = Client::try_connect(addr, Some(allowed)).await;
        assert!(c.is_ok(), "{allowed} must be allowed");
    }
    for denied in [
        "https://evil.example",
        "http://localhost.evil.example:3000",
        "null",
        "https://app.example.org:8443",
    ] {
        match Client::try_connect(addr, Some(denied)).await {
            Err(tungstenite::Error::Http(resp)) => assert_eq!(resp.status(), 403, "{denied}"),
            Err(e) => panic!("{denied}: unexpected error {e}"),
            Ok(_) => panic!("{denied} must be rejected"),
        }
    }
    // No Origin (CLI tools) is allowed.
    Client::handshaken(addr).await;
    server.shutdown().await;
}

#[tokio::test]
async fn shutdown_closes_connections_with_1001() {
    let mock = MockBackend::new();
    let server = start(&mock, config()).await;
    let mut a = Client::handshaken(server.local_addr()).await;
    let mut b = Client::connect(server.local_addr()).await;
    let addr = server.local_addr();
    server.shutdown().await;
    assert_eq!(a.closed().await, Some(1001));
    assert_eq!(b.closed().await, Some(1001));
    assert!(
        Client::try_connect(addr, None).await.is_err(),
        "listener is gone"
    );
}

#[tokio::test]
async fn listens_on_loopback_only() {
    let mock = MockBackend::new();
    let server = start(
        &mock,
        ServerConfig {
            bind_ipv6: true,
            ..config()
        },
    )
    .await;
    for addr in server.local_addrs() {
        assert!(addr.ip().is_loopback(), "{addr} is not loopback");
    }
    server.shutdown().await;
}
