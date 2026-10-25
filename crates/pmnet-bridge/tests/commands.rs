//! Command round trips through the server to the mock backend, and error replies.

mod common;

use common::{config, mock_with_node, start, Client, NODE, TUBE};
use pmnet_bridge::backend::mock::{sample_tube, MockCall, MockRdm};
use pmnet_bridge::backend::{RdmPayload, RdmRequest};
use pmnet_bridge::protocol::{PortRef, RdmValue, TodAction};
use serde_json::{json, Value};
use std::time::{Duration, Instant};

/// RDM addressing of the tube on port 1.
fn tube(pid: u16) -> Value {
    json!({"nodeId": NODE, "port": {"bind": 1, "index": 0}, "uid": "7ff0:74b6510c", "pid": pid})
}

/// Last recorded RDM request.
fn last_rdm(calls: &[MockCall]) -> RdmRequest {
    calls
        .iter()
        .rev()
        .find_map(|c| match c {
            MockCall::RdmGet(r) | MockCall::RdmSet(r) => Some(r.clone()),
            _ => None,
        })
        .expect("an RDM call")
}

#[tokio::test]
async fn rdm_get_set_round_trip() {
    let mock = mock_with_node();
    mock.set_rdm(
        TUBE,
        0x00F0,
        MockRdm::Ack {
            data: vec![0x00, 0x1F],
            value: Some(RdmValue::Integer(31)),
        },
    );
    mock.set_rdm(TUBE, 0x1000, MockRdm::Nack(0x0000));
    mock.set_rdm(TUBE, 0x0082, MockRdm::Timeout);
    let server = start(&mock, config()).await;
    let mut c = Client::handshaken(server.local_addr()).await;

    let r = c.ok("rdmGet", tube(0x00F0)).await;
    assert_eq!(r, json!({"data": "001f", "value": 31}));
    let req = last_rdm(&mock.calls());
    assert_eq!(req.address.uid, TUBE);
    assert_eq!(
        (req.address.port, req.address.sub_device, req.pid),
        (PortRef::new(1, 0), 0, 0x00F0)
    );
    assert_eq!(req.payload, RdmPayload::Empty);

    // GET with parameter data (hex, either case) and with a decoded argument value.
    let mut args = tube(0x00E1);
    args["data"] = json!("0A");
    c.err("rdmGet", args).await;
    assert_eq!(
        last_rdm(&mock.calls()).payload,
        RdmPayload::Data(vec![0x0A])
    );
    let mut args = tube(0x00E1);
    args["value"] = json!(2);
    c.err("rdmGet", args).await;
    assert_eq!(
        last_rdm(&mock.calls()).payload,
        RdmPayload::Value(RdmValue::Integer(2))
    );

    // SET with a value; the device then reports it (last writer wins in the mock).
    let mut args = tube(0x00F0);
    args["value"] = json!(100);
    args["subDevice"] = json!(0);
    assert_eq!(c.ok("rdmSet", args).await, json!({"data": ""}));
    assert_eq!(
        c.ok("rdmGet", tube(0x00F0)).await,
        json!({"data": "", "value": 100})
    );

    // NACK carries reason and name; timeouts map to `timeout`.
    let e = c.err("rdmGet", tube(0x1000)).await;
    assert_eq!(e["code"], "nack");
    assert_eq!(
        e["details"],
        json!({"nackReason": 0, "nackName": "UNKNOWN_PID"})
    );
    assert_eq!(c.err("rdmGet", tube(0x0082)).await["code"], "timeout");

    // Broadcasts return right away.
    let mut args = tube(0x1000);
    args["uid"] = json!("FFFF:FFFFFFFF");
    args["value"] = json!(true);
    assert_eq!(
        c.ok("rdmSet", args).await,
        json!({"data": "", "broadcast": true})
    );
    server.shutdown().await;
}

#[tokio::test]
async fn rdm_describe_tod_and_node_commands() {
    let mock = mock_with_node();
    mock.set_description(sample_tube(TUBE, 1));
    let server = start(&mock, config()).await;
    let mut c = Client::handshaken(server.local_addr()).await;
    let port = json!({"bind": 1, "index": 0});

    let d = c
        .ok(
            "rdmDescribe",
            json!({"nodeId": NODE, "port": port, "uid": "7FF0:74B6510C", "refresh": true}),
        )
        .await;
    assert_eq!(d["uid"], "7FF0:74B6510C");
    assert_eq!(d["deviceInfo"]["deviceModelId"], 1);
    assert_eq!(d["personalities"][1]["footprint"], 3);
    assert!(mock
        .calls()
        .iter()
        .any(|c| matches!(c, MockCall::RdmDescribe(_, true))));

    // todRequest answers {} and the TOD arrives as an event.
    assert_eq!(
        c.ok("todRequest", json!({"nodeId": NODE, "port": port}))
            .await,
        json!({})
    );
    let tod = c.event("tod").await;
    assert_eq!(tod["uids"], json!(["7FF0:74B6510C"]));
    c.ok(
        "todControl",
        json!({"nodeId": NODE, "port": port, "action": "incOff"}),
    )
    .await;
    let action = mock.calls().iter().find_map(|c| match c {
        MockCall::TodControl(_, _, a) => Some(*a),
        _ => None,
    });
    assert_eq!(action, Some(TodAction::IncOff));
    let e = c
        .err(
            "todControl",
            json!({"nodeId": NODE, "port": port, "action": "explode"}),
        )
        .await;
    assert_eq!(e["details"]["field"], "action");

    // artAddress: result is the node state; the change is also announced as an event.
    let node = c
        .ok(
            "artAddress",
            json!({"nodeId": NODE, "shortName": "Stage L", "ports": [{"port": {"bind": 2, "index": 0}, "outputPortAddress": 0x123, "merge": "ltp"}]}),
        )
        .await;
    assert_eq!(node["node"]["shortName"], "Stage L");
    assert_eq!(node["node"]["ports"][1]["outputPortAddress"], 0x123);
    assert_eq!(node["node"]["ports"][1]["output"]["merge"], "ltp");
    assert_eq!(c.event("node").await["node"]["shortName"], "Stage L");
    let e = c.err("artAddress", json!({"nodeId": NODE, "ports": [{"port": {"bind": 1, "index": 0}, "outputPortAddress": 40000}]})).await;
    assert_eq!(e["code"], "invalidPortAddress");
    let e = c
        .err(
            "artAddress",
            json!({"nodeId": NODE, "ports": [{"port": {"bind": 7, "index": 0}}]}),
        )
        .await;
    assert_eq!(e["code"], "unknownPort");

    // artInput and ipProg.
    let node = c
        .ok(
            "artInput",
            json!({"nodeId": NODE, "bind": 3, "disabled": [true]}),
        )
        .await;
    assert_eq!(node["node"]["ports"][2]["input"]["disabled"], true);
    let e = c
        .err(
            "artInput",
            json!({"nodeId": NODE, "bind": 3, "disabled": [true, false]}),
        )
        .await;
    assert_eq!(e["details"]["field"], "disabled");
    let ip = c
        .ok(
            "ipProg",
            json!({"nodeId": NODE, "ip": "2.0.0.77", "mask": "255.0.0.0"}),
        )
        .await;
    assert_eq!(
        ip,
        json!({"ip": "2.0.0.77", "mask": "255.0.0.0", "gateway": "0.0.0.0", "dhcp": false})
    );
    assert_eq!(
        c.event("node").await["node"]["ip"],
        "2.27.44.61",
        "artInput's event comes first"
    );
    assert_eq!(c.event("node").await["node"]["ip"], "2.0.0.77");
    let e = c
        .err("ipProg", json!({"nodeId": NODE, "ip": "2.0.0.300"}))
        .await;
    assert_eq!(e["details"]["field"], "ip");

    // poll.
    c.ok("poll", json!({})).await;
    assert!(mock.calls().contains(&MockCall::Poll));
    server.shutdown().await;
}

#[tokio::test]
async fn error_replies() {
    let mock = mock_with_node();
    let server = start(&mock, config()).await;
    let mut c = Client::handshaken(server.local_addr()).await;

    // Not attributable: id null.
    for bad in [
        "{",
        "[1,2]",
        r#"{"type":"nodes"}"#,
        r#"{"type":"nodes","id":-4}"#,
        r#"{"type":"nodes","id":"7"}"#,
    ] {
        c.send_text(bad).await;
        let e = c.next_json().await;
        assert_eq!(
            (e["type"].as_str(), e["id"].clone(), e["code"].as_str()),
            (Some("error"), Value::Null, Some("badRequest")),
            "{bad}"
        );
    }
    // Attributable but invalid.
    c.send_text(r#"{"id":9}"#).await;
    let e = c.reply(9).await;
    assert_eq!(
        (e["code"].as_str(), e["details"]["field"].as_str()),
        (Some("badRequest"), Some("type"))
    );
    assert_eq!(
        c.err("selfDestruct", json!({})).await["code"],
        "unknownCommand"
    );

    let port = json!({"bind": 1, "index": 0});
    let cases = [
        (
            "rdmGet",
            json!({"nodeId": NODE, "port": port, "uid": "7FF0-74B6510C", "pid": 96}),
            "badRequest",
            "uid",
        ),
        (
            "rdmGet",
            json!({"nodeId": NODE, "port": port, "uid": "7FF0:74B6510C", "pid": 70000}),
            "badRequest",
            "pid",
        ),
        (
            "rdmGet",
            json!({"nodeId": NODE, "port": port, "uid": "7FF0:74B6510C", "pid": 96, "data": "abc"}),
            "badRequest",
            "data",
        ),
        (
            "rdmGet",
            json!({"nodeId": NODE, "port": port, "uid": "7FF0:74B6510C", "pid": 96, "data": "00", "value": 1}),
            "badRequest",
            "value",
        ),
        (
            "rdmGet",
            json!({"nodeId": NODE, "port": port, "uid": "7FF0:74B6510C", "pid": 96, "subDevice": 600}),
            "badRequest",
            "subDevice",
        ),
        (
            "rdmSet",
            json!({"nodeId": NODE, "port": port, "uid": "7FF0:74B6510C", "pid": 240}),
            "badRequest",
            "data",
        ),
        (
            "rdmGet",
            json!({"nodeId": NODE, "port": {"bind": 0, "index": 0}, "uid": "7FF0:74B6510C", "pid": 96}),
            "badRequest",
            "port",
        ),
        (
            "rdmGet",
            json!({"nodeId": NODE, "port": {"bind": 1}, "uid": "7FF0:74B6510C", "pid": 96}),
            "badRequest",
            "port",
        ),
        (
            "rdmDescribe",
            json!({"nodeId": NODE, "port": port, "uid": "FFFF:FFFFFFFF"}),
            "badRequest",
            "uid",
        ),
        ("todRequest", json!({"port": port}), "badRequest", "nodeId"),
    ];
    for (kind, args, code, field) in cases {
        let e = c.err(kind, args.clone()).await;
        assert_eq!(e["code"], code, "{kind} {args}");
        assert_eq!(e["details"]["field"], field, "{kind} {args}: {e}");
    }
    let e = c
        .err(
            "rdmGet",
            json!({"nodeId": "7FF0:00000000", "port": port, "uid": "7FF0:74B6510C", "pid": 96}),
        )
        .await;
    assert_eq!(e["code"], "unknownNode");
    let e = c
        .err(
            "todRequest",
            json!({"nodeId": NODE, "port": {"bind": 9, "index": 0}}),
        )
        .await;
    assert_eq!(e["code"], "unknownPort");

    // Unknown argument fields are ignored; the connection survived all of the above.
    c.ok("nodes", json!({"future": {"option": true}})).await;
    server.shutdown().await;
}

#[tokio::test]
async fn slow_commands_do_not_block_the_connection() {
    let mock = mock_with_node();
    mock.set_rdm(
        TUBE,
        0x0060,
        MockRdm::Ack {
            data: vec![1],
            value: None,
        },
    );
    mock.set_rdm_delay(Duration::from_millis(400));
    let server = start(&mock, config()).await;
    let mut c = Client::handshaken(server.local_addr()).await;

    let started = Instant::now();
    let slow = c.send_command("rdmGet", tube(0x0060)).await;
    let fast = c.send_command("nodes", json!({})).await;
    // The `nodes` reply arrives while the RDM request is still waiting for the "network".
    let first = c.next_json().await;
    assert_eq!(first["id"], fast);
    assert!(started.elapsed() < Duration::from_millis(300));
    assert_eq!(c.reply(slow).await["result"]["data"], "01");
    server.shutdown().await;
}

#[tokio::test]
async fn flash_jobs_and_progress() {
    let mock = mock_with_node();
    mock.set_description(sample_tube(TUBE, 1));
    mock.set_flash_step(Duration::from_millis(5));
    let server = start(&mock, config()).await;
    let mut a = Client::handshaken(server.local_addr()).await;
    let mut b = Client::handshaken(server.local_addr()).await;
    let hex = ":10000000FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF00\n:00000001FF\n";

    let info = a.ok("imageInfo", json!({"hex": hex})).await;
    assert_eq!(info["version"], "1.2.0");
    assert_eq!(
        a.err("imageInfo", json!({"hex": "garbage"})).await["code"],
        "invalidImage"
    );

    let port = json!({"bind": 1, "index": 0});
    let job = a.ok("flash", json!({"nodeId": NODE, "port": port, "uid": "7FF0:74B6510C", "hex": hex, "allowDowngrade": false})).await;
    let job_id = job["job"]["jobId"].as_str().unwrap().to_owned();
    assert_eq!(job["job"]["phase"], "entering");

    // Progress goes to every connection, through to `done`.
    for client in [&mut a, &mut b] {
        loop {
            let ev = client.event("flashProgress").await;
            assert_eq!(ev["job"]["jobId"], job_id.as_str());
            if ev["job"]["phase"] == "done" {
                assert_eq!(ev["job"]["result"]["softwareVersionLabel"], "1.2.0");
                break;
            }
        }
    }
    let jobs = a.ok("flashJobs", json!({})).await;
    assert_eq!(jobs["jobs"][0]["phase"], "done");
    assert_eq!(
        a.err("flashCancel", json!({"jobId": "nope"})).await["code"],
        "unknownJob"
    );

    // One job per port: a second one is `busy` while the first runs; cancel ends it.
    mock.set_flash_step(Duration::from_secs(10));
    let args = json!({"nodeId": NODE, "port": port, "uid": "7FF0:74B6510C", "hex": hex, "allowDowngrade": false});
    let job = a.ok("flash", args.clone()).await["job"]["jobId"]
        .as_str()
        .unwrap()
        .to_owned();
    assert_eq!(a.err("flash", args).await["code"], "busy");
    a.ok("flashCancel", json!({"jobId": job})).await;
    loop {
        let ev = a.event("flashProgress").await;
        if ev["job"]["phase"] == "cancelled" {
            break;
        }
    }
    server.shutdown().await;
}
