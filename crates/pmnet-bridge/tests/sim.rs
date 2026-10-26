//! End to end against the real backend: `pixelmapper-net`'s simulated node (Art-Net over UDP on localhost, with
//! simulated tubes and a third-party fixture), the bridge with `PixelmapperNetBackend`, and a WebSocket client.
//! Every test uses its own ephemeral ports, so they run in parallel.

mod common;

use common::{config, eventually, Client};
use pixelmapper_net::flash::tube::TubeFlashOptions;
use pixelmapper_net::image::tube::build_test_image;
use pixelmapper_net::image::TubeImage;
use pixelmapper_net::pids;
use pixelmapper_net::rdm::Uid;
use pixelmapper_net::sim::tube::TubeTiming;
use pixelmapper_net::sim::{SimConfig, SimFixture, SimNode, SimTube, TubeMode};
use pmnet_bridge::backend::pixelmapper_net::{
    contract_timing, BackendConfig, PixelmapperNetBackend,
};
use pmnet_bridge::backend::NetBackend;
use pmnet_bridge::protocol::frame::encode_frame_set;
use pmnet_bridge::server::{serve, ServerHandle};
use serde_json::{json, Value};
use std::net::SocketAddr;
use std::sync::Arc;
use std::time::Duration;

const TUBE1: Uid = Uid::new(pids::ESTA_MANUFACTURER, 0x0000_1001);
const TUBE2: Uid = Uid::new(pids::ESTA_MANUFACTURER, 0x0000_1002);
const FIXTURE: Uid = Uid::new(0x7FF1, 0x0000_2001);

/// `{bind, index: 0}`.
fn port(bind: u8) -> Value {
    json!({"bind": bind, "index": 0})
}

/// A tube image with a recognisable app.
fn tube_image(version: u32, model: u16) -> TubeImage {
    let app: Vec<u8> = (0..1500u32).map(|i| (i * 13 + version) as u8).collect();
    build_test_image(&app, pids::ESTA_MANUFACTURER, model, version)
}

/// A tube running app 0.1.0 that restarts quickly.
fn tube(uid: Uid) -> SimTube {
    SimTube::new(uid, Some(tube_image(0x0100, pids::MODEL_TUBE))).with_timing(TubeTiming {
        app_start_delay: Duration::from_millis(50),
        ..TubeTiming::default()
    })
}

/// A free UDP port on localhost for the simulator (bound and released again).
fn free_udp_addr() -> SocketAddr {
    let s = std::net::UdpSocket::bind("127.0.0.1:0").expect("bind");
    s.local_addr().expect("addr")
}

/// The backend talking to a simulator at `sim`, with short test timings.
fn backend_config(sim: SocketAddr) -> BackendConfig {
    let mut cfg = BackendConfig::local(vec![sim]);
    cfg.controller.poll_interval = Duration::from_millis(300);
    cfg.controller.rdm.timeout = Duration::from_millis(150);
    cfg.controller = contract_timing(cfg.controller);
    cfg.tube_flash = TubeFlashOptions {
        enter_wait: Duration::from_millis(100),
        boot_info_retry_wait: Duration::from_millis(50),
        finish_wait: Duration::from_millis(300),
        ..TubeFlashOptions::default()
    };
    cfg.tod_settle = Duration::from_millis(300);
    cfg.tod_timeout = Duration::from_secs(1);
    cfg
}

/// A simulated node with two tubes on port 1 and a fixture on port 2.
async fn start_sim(addr: SocketAddr) -> SimNode {
    let sim = SimNode::start(SimConfig {
        bind: addr,
        ..SimConfig::default()
    })
    .await
    .expect("simulator starts");
    sim.add_responder(1, Box::new(tube(TUBE1)));
    sim.add_responder(1, Box::new(tube(TUBE2)));
    sim.add_responder(2, Box::new(SimFixture::default_par(FIXTURE)));
    sim
}

/// Bridge server on the real backend.
async fn start_bridge(sim: SocketAddr) -> (PixelmapperNetBackend, ServerHandle) {
    let backend = PixelmapperNetBackend::start(backend_config(sim)).await;
    let server = serve(config(), Arc::new(backend.clone()))
        .await
        .expect("server starts");
    (backend, server)
}

/// Waits until `nodes` lists `node`.
async fn wait_for_node(c: &mut Client, node: &str) -> Value {
    for _ in 0..100 {
        let r = c.ok("nodes", json!({})).await;
        if let Some(n) = r["nodes"]
            .as_array()
            .and_then(|a| a.iter().find(|n| n["nodeId"] == node))
        {
            return n.clone();
        }
        tokio::time::sleep(Duration::from_millis(50)).await;
    }
    panic!("node {node} not discovered");
}

/// Waits for the next `tod` event of `bind`.
async fn tod_event(c: &mut Client, bind: u8) -> Value {
    within(&format!("a tod of bind {bind}"), async {
        loop {
            let t = c.event("tod").await;
            if t["port"]["bind"] == bind {
                return t;
            }
        }
    })
    .await
}

/// Runs `fut`; fails the test naming `what` after 10 s (other events may keep the client busy meanwhile).
async fn within<T>(what: &str, fut: impl std::future::Future<Output = T>) -> T {
    tokio::time::timeout(Duration::from_secs(10), fut)
        .await
        .unwrap_or_else(|_| panic!("timed out waiting for {what}"))
}

/// RDM addressing arguments.
fn rdm(node: &str, bind: u8, uid: Uid, pid: u16) -> Value {
    json!({"nodeId": node, "port": port(bind), "uid": uid.to_string(), "pid": pid})
}

#[tokio::test]
async fn discovery_rdm_configuration_dmx_and_tods() {
    let sim_addr = free_udp_addr();
    let (backend, server) = start_bridge(sim_addr).await;
    let mut c = Client::handshaken(server.local_addr()).await;
    assert_eq!(c.hello["udp"]["bound"], true);
    assert_eq!(
        c.hello["udp"]["port"],
        backend.local_addr().expect("bound").port()
    );

    // The node appears after the handshake: `node` events (one more per merged BindIndex page), then the TODs
    // the bridge requested.
    let sim = start_sim(sim_addr).await;
    let node_id = sim.uid().to_string();
    let node = within("the node with 4 ports", async {
        loop {
            let n = c.event("node").await["node"].clone();
            assert_eq!(n["nodeId"], node_id);
            if n["ports"].as_array().is_some_and(|p| p.len() == 4) {
                return n;
            }
        }
    })
    .await;
    assert_eq!(node["uid"], node_id);
    assert_eq!(node["ip"], "127.0.0.1");
    assert_eq!(node["shortName"], "PM Node");
    assert_eq!(node["estaMan"], pids::ESTA_MANUFACTURER);
    assert_eq!(node["mac"], "02:00:00:12:34:56");
    let ports = node["ports"].as_array().expect("ports");
    assert_eq!(ports.len(), 4);
    for (i, p) in ports.iter().enumerate() {
        assert_eq!(p["port"], port(i as u8 + 1));
        assert_eq!(p["direction"], "output");
        assert_eq!(p["outputPortAddress"], i + 1);
        assert_eq!(p["output"]["rdm"], true);
    }
    let tod = tod_event(&mut c, 1).await;
    assert_eq!(tod["nodeId"], node_id);
    assert_eq!(tod["portAddress"], 1);
    assert_eq!(tod["available"], true);
    assert_eq!(tod["uids"], json!([TUBE1.to_string(), TUBE2.to_string()]));
    let tod2 = tod_event(&mut c, 2).await;
    assert_eq!(tod2["uids"], json!([FIXTURE.to_string()]));
    let nodes = c.ok("nodes", json!({})).await;
    assert_eq!(nodes["nodes"][0]["nodeId"], node_id);
    assert!(nodes["tods"].as_array().expect("tods").len() >= 2);

    // RDM GET with decoded value.
    let r = c.ok("rdmGet", rdm(&node_id, 1, TUBE1, 0x0060)).await;
    assert_eq!(r["value"]["deviceModelId"], pids::MODEL_TUBE);
    assert_eq!(r["value"]["softwareVersionId"], 0x0100);
    assert_eq!(r["data"].as_str().expect("hex").len(), 38);

    // RDM SET by value, read back from the device (and from the simulator).
    let mut set = rdm(&node_id, 1, TUBE1, 0x00F0);
    set["value"] = json!(42);
    let r = c.ok("rdmSet", set).await;
    assert_eq!(r["data"], "");
    let r = c.ok("rdmGet", rdm(&node_id, 1, TUBE1, 0x00F0)).await;
    assert_eq!(
        (r["value"].clone(), r["data"].clone()),
        (json!(42), json!("002a"))
    );
    assert_eq!(
        sim.with_responder(TUBE1, |t: &mut SimTube| t.start_address()),
        Some(42)
    );
    // Out of range for the contract's value table: rejected before sending.
    let mut bad = rdm(&node_id, 1, TUBE1, 0x00F0);
    bad["value"] = json!(600);
    let e = c.err("rdmSet", bad).await;
    assert_eq!(
        (e["code"].clone(), e["details"]["field"].clone()),
        (json!("badRequest"), json!("value"))
    );

    // rdmDescribe of a tube.
    let mut describe = rdm(&node_id, 1, TUBE1, 0);
    describe.as_object_mut().expect("object").remove("pid");
    let d = c.ok("rdmDescribe", describe).await;
    assert_eq!(d["uid"], TUBE1.to_string());
    assert_eq!(d["deviceInfo"]["dmxStartAddress"], 42);
    assert!(d["softwareVersionLabel"].is_string());
    assert!(d["manufacturerLabel"].is_string());
    let count = d["deviceInfo"]["personalityCount"].as_u64().expect("count");
    assert_eq!(
        d["personalities"].as_array().expect("list").len() as u64,
        count
    );
    assert!(d["personalities"][0]["description"].is_string());
    assert!(!d["slots"].as_array().expect("slots").is_empty());
    assert!(d["supportedParameters"]
        .as_array()
        .expect("pids")
        .contains(&json!(pids::PID_ENTER_BOOTLOADER)));
    assert_eq!(
        d["parameterDescriptions"][0]["pid"],
        pids::PID_ENTER_BOOTLOADER
    );
    assert_eq!(d["errors"], json!([]));

    // The node's own responder: manufacturer PIDs decode via pids.toml (bool field) and after rdmDescribe.
    let node_uid = sim.uid();
    let r = c
        .ok(
            "rdmGet",
            rdm(&node_id, 1, node_uid, pids::PID_PORT1_ENABLED),
        )
        .await;
    assert_eq!(r["value"], true);
    let mut describe = rdm(&node_id, 1, node_uid, 0);
    describe.as_object_mut().expect("object").remove("pid");
    let d = c.ok("rdmDescribe", describe).await;
    assert_eq!(d["deviceInfo"]["deviceModelId"], pids::MODEL_NODE);
    let descs = d["parameterDescriptions"].as_array().expect("descriptions");
    assert!(descs
        .iter()
        .any(|p| p["pid"] == pids::PID_PORT1_DMX_BREAK && p["dataTypeName"] == "uint16"));
    let mut set = rdm(&node_id, 1, node_uid, pids::PID_PORT1_DMX_BREAK);
    set["value"] = json!(200);
    c.ok("rdmSet", set).await;
    let r = c
        .ok(
            "rdmGet",
            rdm(&node_id, 1, node_uid, pids::PID_PORT1_DMX_BREAK),
        )
        .await;
    assert_eq!(r["value"], 200);

    // Errors from the network: a NACK with its reason, no answer from an unknown UID.
    let e = c.err("rdmGet", rdm(&node_id, 1, TUBE1, 0x0901)).await;
    assert_eq!(e["code"], "nack");
    assert_eq!(e["details"]["nackReason"], 0);
    assert_eq!(e["details"]["nackName"], "UNKNOWN_PID");
    let ghost = Uid::new(pids::ESTA_MANUFACTURER, 0xDEAD_BEEF);
    let e = c.err("rdmGet", rdm(&node_id, 1, ghost, 0x0060)).await;
    assert_eq!(e["code"], "timeout");
    // A broadcast SET returns right after sending.
    let mut b = rdm(&node_id, 1, Uid::BROADCAST, 0x1000);
    b["value"] = json!(false);
    let r = c.ok("rdmSet", b).await;
    assert_eq!(
        (r["broadcast"].clone(), r["data"].clone()),
        (json!(true), json!(""))
    );

    // artAddress: the `node` event with the node's answer arrives before the result.
    let id = c
        .send_command(
            "artAddress",
            json!({"nodeId": node_id, "shortName": "Bridge Test"}),
        )
        .await;
    let r = within("the artAddress result", async {
        let mut renamed = false;
        loop {
            let m = c.next_json().await;
            if m["type"] == "node" && m["node"]["shortName"] == "Bridge Test" {
                renamed = true;
            }
            if m["id"] == id {
                assert!(renamed, "result before the node event: {m}");
                return m;
            }
        }
    })
    .await;
    assert_eq!(r["type"], "result", "{r}");
    assert_eq!(r["result"]["node"]["shortName"], "Bridge Test");
    assert_eq!(sim.snapshot().short_name, "Bridge Test");

    // Per-port settings: the failsafe mode of port 1 (BindIndex 1), RDM off on port 4, whose TOD then is a TodNak.
    let r = c
        .ok(
            "artAddress",
            json!({"nodeId": node_id, "ports": [
                {"port": port(1), "failsafe": "zero"},
                {"port": port(4), "rdm": false}
            ]}),
        )
        .await;
    assert_eq!(r["node"]["ports"][3]["output"]["rdm"], false);
    let snap = sim.snapshot();
    assert_eq!((snap.ports[0].failsafe, snap.ports[1].failsafe), (1, 0));
    assert!(!snap.ports[3].rdm_enabled);
    let nodes = c.ok("nodes", json!({})).await;
    let ports = &nodes["nodes"][0]["ports"];
    assert_eq!(ports[0]["failsafe"], "zero");
    assert_eq!(
        ports[1]["failsafe"],
        Value::Null,
        "pixelmapper-net keeps Status3 of the lowest BindIndex only"
    );
    c.ok("todRequest", json!({"nodeId": node_id, "port": port(4)}))
        .await;
    let tod = tod_event(&mut c, 4).await;
    assert_eq!(
        (tod["available"].clone(), tod["uids"].clone()),
        (json!(false), json!([]))
    );

    // ipProg with only nodeId reads (ArtIpProg Command 0) and changes nothing.
    let before = sim.snapshot();
    let r = c.ok("ipProg", json!({"nodeId": node_id})).await;
    assert_eq!(r["ip"], before.ip.to_string());
    assert_eq!(r["mask"], before.netmask.to_string());
    assert_eq!(r["gateway"], before.gateway.to_string());
    assert_eq!(r["dhcp"], before.dhcp);
    let after = sim.snapshot();
    assert_eq!((after.ip, after.dhcp), (before.ip, before.dhcp));
    let e = c
        .err("ipProg", json!({"nodeId": node_id, "dhcp": false}))
        .await;
    assert_eq!(e["details"]["field"], "dhcp");

    // DMX: route → ArtDmx at the node (Physical = connection number), ArtSync accepted.
    c.ok(
        "dmxRoutes",
        json!({"routes": [{"route": 1, "nodeId": node_id, "portAddress": 1}]}),
    )
    .await;
    c.send_binary(encode_frame_set(&[(1, &[10, 20, 30])], true))
        .await;
    let frame = eventually(|| sim.last_dmx(1)).await;
    assert_eq!(&frame.data[..3], &[10, 20, 30]);
    assert_eq!(frame.physical, 0);
    eventually(|| (sim.sync_count() > 0).then_some(())).await;

    // todRequest: the (unchanged) TOD arrives as `tod`.
    c.ok("todRequest", json!({"nodeId": node_id, "port": port(1)}))
        .await;
    let tod = tod_event(&mut c, 1).await;
    assert_eq!(tod["uids"].as_array().expect("uids").len(), 2);
    // todControl flush: the TOD after the discovery arrives as `tod`, also when it did not change.
    c.ok(
        "todControl",
        json!({"nodeId": node_id, "port": port(1), "action": "flush"}),
    )
    .await;
    let tod = tod_event(&mut c, 1).await;
    assert_eq!(tod["uids"].as_array().expect("uids").len(), 2);
    // A device leaving: unsolicited ArtTodData → `tod`.
    sim.remove_responder(1, TUBE2);
    let tod = tod_event(&mut c, 1).await;
    assert_eq!(tod["uids"], json!([TUBE1.to_string()]));

    // The node goes silent: `nodeGone` after three poll cycles.
    sim.shutdown();
    let gone = c.event("nodeGone").await;
    assert_eq!(gone["nodeId"], node_id);
    assert!(backend.snapshot().nodes.is_empty());

    server.shutdown().await;
    backend.shutdown();
}

/// Collects `flashProgress` events of `job` until it ends; returns them.
async fn flash_events(c: &mut Client, job: &str) -> Vec<Value> {
    within(&format!("the end of job {job}"), async {
        let mut out = Vec::new();
        loop {
            let ev = c.event("flashProgress").await;
            if ev["job"]["jobId"] != job {
                continue;
            }
            let phase = ev["job"]["phase"].as_str().expect("phase").to_owned();
            out.push(ev["job"].clone());
            if ["done", "failed", "cancelled"].contains(&phase.as_str()) {
                return out;
            }
        }
    })
    .await
}

#[tokio::test]
async fn tube_flash_jobs() {
    let sim_addr = free_udp_addr();
    let sim = start_sim(sim_addr).await;
    let node_id = sim.uid().to_string();
    let (backend, server) = start_bridge(sim_addr).await;
    let mut c = Client::handshaken(server.local_addr()).await;
    wait_for_node(&mut c, &node_id).await;

    let flash = |hex: String, allow: bool| {
        json!({"nodeId": node_id, "port": port(1), "uid": TUBE1.to_string(), "hex": hex,
               "allowDowngrade": allow})
    };
    let new = tube_image(0x0200, pids::MODEL_TUBE);

    // imageInfo and the checks that answer before a job starts.
    let info = c.ok("imageInfo", json!({"hex": new.to_hex()})).await;
    assert_eq!(info["version"], "0.2.0");
    assert_eq!(info["versionId"], 0x0200);
    assert_eq!(info["modelId"], pids::MODEL_TUBE);
    let e = c.err("flash", flash("not intel hex".into(), false)).await;
    assert_eq!(e["code"], "invalidImage");
    let wrong = tube_image(0x0300, 0x77);
    let e = c.err("flash", flash(wrong.to_hex(), false)).await;
    assert_eq!(e["code"], "deviceMismatch");
    let old = tube_image(0x0050, pids::MODEL_TUBE);
    let e = c.err("flash", flash(old.to_hex(), false)).await;
    assert_eq!(e["code"], "downgrade");
    assert_eq!(e["details"]["current"], 0x0100);
    assert_eq!(e["details"]["image"], 0x0050);

    // A real update with progress events up to `done`.
    let r = c.ok("flash", flash(new.to_hex(), false)).await;
    let job_id = r["job"]["jobId"].as_str().expect("jobId").to_owned();
    assert_eq!(r["job"]["phase"], "entering");
    assert_eq!(r["job"]["image"]["versionId"], 0x0200);
    let total = r["job"]["pagesTotal"].as_u64().expect("pages");
    assert!(total > 0);
    let events = flash_events(&mut c, &job_id).await;
    let phases: Vec<&str> = events
        .iter()
        .map(|j| j["phase"].as_str().unwrap_or(""))
        .collect();
    for p in ["writing", "verifying", "finishing", "confirming", "done"] {
        assert!(phases.contains(&p), "phase {p} missing in {phases:?}");
    }
    let done = events.last().expect("done");
    assert_eq!(done["result"]["softwareVersionId"], 0x0200);
    assert_eq!(done["inBootloader"], false);
    assert_eq!(done["error"], Value::Null);
    // Progress within a phase is throttled; the page count is complete at the latest with the next phase.
    assert!(events.iter().any(|j| j["phase"] == "writing"));
    assert!(events
        .iter()
        .any(|j| j["phase"] == "verifying" && j["pagesDone"] == j["pagesTotal"]));
    assert_eq!(
        sim.with_responder(TUBE1, |t: &mut SimTube| t.app_version()),
        Some(Some(0x0200))
    );

    // One job per port: a second flash while one runs is `busy`; cancelling ends the job as `cancelled`.
    let newer = tube_image(0x0300, pids::MODEL_TUBE);
    let r = c.ok("flash", flash(newer.to_hex(), false)).await;
    let second = r["job"]["jobId"].as_str().expect("jobId").to_owned();
    let e = c.err("flash", flash(newer.to_hex(), false)).await;
    assert_eq!(e["code"], "busy");
    c.ok("flashCancel", json!({"jobId": second})).await;
    let events = flash_events(&mut c, &second).await;
    assert_eq!(events.last().expect("end")["phase"], "cancelled");
    let e = c.err("flashCancel", json!({"jobId": "f999"})).await;
    assert_eq!(e["code"], "unknownJob");
    let jobs = c.ok("flashJobs", json!({})).await;
    let ids: Vec<&str> = jobs["jobs"]
        .as_array()
        .expect("jobs")
        .iter()
        .filter_map(|j| j["jobId"].as_str())
        .collect();
    assert_eq!(ids, [job_id.as_str(), second.as_str()]);
    // A cancelled update leaves the tube in its bootloader (or it had not left the app yet).
    let mode = sim.with_responder(TUBE1, |t: &mut SimTube| t.mode());
    assert!(matches!(mode, Some(TubeMode::Boot | TubeMode::App)));

    server.shutdown().await;
    backend.shutdown();
    sim.shutdown();
}

/// The `node` events arriving within `d` (other messages are skipped).
async fn node_events_within(c: &mut Client, d: Duration) -> Vec<Value> {
    let deadline = tokio::time::Instant::now() + d;
    let mut out = Vec::new();
    while let Ok(m) = tokio::time::timeout_at(deadline, c.next_json()).await {
        if m["type"] == "node" {
            out.push(m);
        }
    }
    out
}

#[tokio::test]
async fn steady_node_sends_no_repeated_node_events() {
    let sim_addr = free_udp_addr();
    let (backend, server) = start_bridge(sim_addr).await;
    let mut c = Client::handshaken(server.local_addr()).await;
    let sim = start_sim(sim_addr).await;
    within("the node with 4 ports", async {
        loop {
            let n = c.event("node").await;
            if n["node"]["ports"].as_array().is_some_and(|p| p.len() == 4) {
                return;
            }
        }
    })
    .await;
    let before = c.ok("nodes", json!({})).await["nodes"][0].clone();

    // ~8 poll cycles: every ArtPollReply advances the NodeReport counter (and lastSeen) – no `node` event.
    let events = node_events_within(&mut c, Duration::from_millis(2500)).await;
    assert!(events.is_empty(), "repeated node events: {events:?}");
    // The `nodes` snapshot still carries the current counter and lastSeen.
    let after = c.ok("nodes", json!({})).await["nodes"][0].clone();
    assert_ne!(after["nodeReport"], before["nodeReport"]);
    assert!(after["lastSeen"].as_u64() > before["lastSeen"].as_u64());

    // A real change is still an event.
    sim.set_short_name_local("Panel Name");
    let events = node_events_within(&mut c, Duration::from_millis(1500)).await;
    assert_eq!(events.len(), 1, "{events:?}");
    assert_eq!(events[0]["node"]["shortName"], "Panel Name");

    server.shutdown().await;
    backend.shutdown();
    sim.shutdown();
}

#[tokio::test]
async fn unbound_socket_is_reported_not_fatal() {
    // Bind to an address this host does not have: the backend starts unbound and says why.
    let mut cfg = BackendConfig::local(vec![free_udp_addr()]);
    cfg.controller.bind = "192.0.2.1:0".parse().expect("addr");
    cfg.bind_retry = Duration::from_secs(60);
    let backend = PixelmapperNetBackend::start(cfg).await;
    let status = backend.status();
    assert!(!status.udp.bound);
    assert!(status.udp.error.is_some());
    assert!(backend.local_addr().is_none());
    let server = serve(config(), Arc::new(backend.clone()))
        .await
        .expect("server starts");
    let mut c = Client::handshaken(server.local_addr()).await;
    assert_eq!(c.hello["udp"]["bound"], false);
    let e = c.err("poll", json!({})).await;
    assert_eq!(e["code"], "udpUnavailable");
    server.shutdown().await;
    backend.shutdown();
}
