//! End-to-end tests of the `pmnet` binary against a simulated node.

use pixelmapper_net::image::tube::build_test_image;
use pixelmapper_net::pids;
use pixelmapper_net::rdm::Uid;
use pixelmapper_net::sim::tube::TubeTiming;
use pixelmapper_net::sim::{SimConfig, SimNode, SimTube};
use serde_json::Value;
use std::io::{BufRead, BufReader};
use std::path::PathBuf;
use std::process::{Command, Stdio};
use std::time::Duration;

const BIN: &str = env!("CARGO_BIN_EXE_pmnet");

fn tube_uid(i: u32) -> Uid {
    Uid::new(pids::ESTA_MANUFACTURER, 0x0000_0A01 + i)
}

fn app() -> Vec<u8> {
    (0..900u32).map(|i| (i * 13 + 1) as u8).collect()
}

/// A sim node with two tubes on port 1 (app 1.0.0, fast app start).
async fn sim() -> SimNode {
    let sim = SimNode::start(SimConfig::default()).await.unwrap();
    let img = build_test_image(
        &app(),
        pids::ESTA_MANUFACTURER,
        pids::MODEL_TUBE,
        0x0001_0000,
    );
    for i in 0..2 {
        let timing = TubeTiming {
            app_start_delay: Duration::from_millis(50),
            ..TubeTiming::default()
        };
        let t = SimTube::new(tube_uid(i), Some(img.clone())).with_timing(timing);
        sim.add_responder(1, Box::new(t));
    }
    sim
}

struct Run {
    code: i32,
    lines: Vec<Value>,
    stderr: String,
}

/// Run `pmnet` against `sim` with JSON output.
async fn pmnet(sim: &SimNode, args: &[&str]) -> Run {
    let addr = sim.local_addr().to_string();
    let mut all: Vec<String> = [
        "--bind",
        "127.0.0.1:0",
        "--no-reuse-port",
        "--broadcast",
        &addr,
        "--json",
        "--wait",
        "800",
    ]
    .iter()
    .map(|s| s.to_string())
    .collect();
    all.extend(args.iter().map(|s| s.to_string()));
    run_args(all).await
}

async fn run_args(args: Vec<String>) -> Run {
    let out = tokio::task::spawn_blocking(move || Command::new(BIN).args(&args).output())
        .await
        .unwrap()
        .unwrap();
    let stdout = String::from_utf8_lossy(&out.stdout).to_string();
    let lines = stdout
        .lines()
        .filter(|l| !l.trim().is_empty())
        .map(|l| serde_json::from_str(l).unwrap_or_else(|e| panic!("not JSON: {l:?}: {e}")))
        .collect();
    Run {
        code: out.status.code().unwrap_or(-1),
        lines,
        stderr: String::from_utf8_lossy(&out.stderr).to_string(),
    }
}

fn ok(r: &Run) {
    assert_eq!(r.code, 0, "stderr: {}", r.stderr);
}

fn of_type<'a>(r: &'a Run, t: &str) -> Vec<&'a Value> {
    r.lines.iter().filter(|l| l["type"] == t).collect()
}

fn temp_dir(name: &str) -> PathBuf {
    let d = std::env::temp_dir().join(format!("pmnet-cli-{}-{name}", std::process::id()));
    std::fs::create_dir_all(&d).unwrap();
    d
}

#[tokio::test(flavor = "multi_thread")]
async fn poll_lists_node() {
    let sim = sim().await;
    let r = pmnet(&sim, &["poll"]).await;
    ok(&r);
    let nodes = of_type(&r, "node");
    assert_eq!(nodes.len(), 1);
    assert_eq!(nodes[0]["node"]["uid"], sim.uid().to_string());
    assert_eq!(nodes[0]["node"]["ports"].as_array().unwrap().len(), 4);
    sim.shutdown();
}

#[tokio::test(flavor = "multi_thread")]
async fn tod_shows_tubes() {
    let sim = sim().await;
    let r = pmnet(&sim, &["tod"]).await;
    ok(&r);
    let tods = of_type(&r, "tod");
    let p1 = tods.iter().find(|t| t["portNumber"] == 1).unwrap();
    let uids: Vec<&str> = p1["uids"]
        .as_array()
        .unwrap()
        .iter()
        .map(|u| u.as_str().unwrap())
        .collect();
    assert_eq!(uids, vec!["7FF0:00000A01", "7FF0:00000A02"]);
    sim.shutdown();
}

#[tokio::test(flavor = "multi_thread")]
async fn rdm_get_device_info_and_describe() {
    let sim = sim().await;
    let uid = tube_uid(0).to_string();
    let r = pmnet(&sim, &["rdm", "get", &uid, "DEVICE_INFO"]).await;
    ok(&r);
    let l = &of_type(&r, "rdm")[0];
    assert_eq!(l["pid"], 0x60);
    assert_eq!(l["decoded"]["deviceModelId"], pids::MODEL_TUBE);
    assert_eq!(l["decoded"]["softwareVersionId"], 0x0001_0000);

    let r = pmnet(&sim, &["rdm", "describe", &uid]).await;
    ok(&r);
    let d = &of_type(&r, "device")[0]["device"];
    assert_eq!(d["uid"], uid);
    assert_eq!(d["deviceInfo"]["deviceModelId"], pids::MODEL_TUBE);
    assert!(!d["supportedParameters"].as_array().unwrap().is_empty());

    // unknown PID name is a usage-level error with exit code 1 and a code on stderr
    let r = pmnet(&sim, &["rdm", "get", &uid, "NO_SUCH_PID"]).await;
    assert_eq!(r.code, 1);
    assert!(r.stderr.contains("invalid_argument"), "{}", r.stderr);
    sim.shutdown();
}

#[tokio::test(flavor = "multi_thread")]
async fn node_config_name_and_set() {
    let sim = sim().await;
    let r = pmnet(&sim, &["node", "config", "name", "--short", "Bridge X"]).await;
    ok(&r);
    assert_eq!(sim.snapshot().short_name, "Bridge X");
    assert_eq!(of_type(&r, "node")[0]["node"]["shortName"], "Bridge X");

    let r = pmnet(&sim, &["node", "config", "set", "PANEL_LOCK", "on"]).await;
    ok(&r);
    assert!(sim.snapshot().panel_locked);

    let r = pmnet(&sim, &["node", "config", "led", "locate"]).await;
    ok(&r);
    assert_eq!(
        sim.snapshot().indicators,
        pixelmapper_net::sim::SimIndicators::Locate
    );
    sim.shutdown();
}

#[tokio::test(flavor = "multi_thread")]
async fn dmx_reaches_sim() {
    let sim = sim().await;
    let r = pmnet(&sim, &["dmx", "0:0:1", "1=255", "3-4=7", "--sync"]).await;
    ok(&r);
    assert_eq!(of_type(&r, "dmx")[0]["nodes"], 1);
    let f = sim.last_dmx(1).expect("frame on port 1");
    assert_eq!(&f.data[..4], &[255, 0, 7, 7]);
    sim.shutdown();
}

#[tokio::test(flavor = "multi_thread")]
async fn flash_tube_updates_sim_tube() {
    let sim = sim().await;
    let dir = temp_dir("flash");
    let hex = dir.join("tube_fw-1.1.0.hex");
    let img = build_test_image(
        &app(),
        pids::ESTA_MANUFACTURER,
        pids::MODEL_TUBE,
        0x0001_0100,
    );
    std::fs::write(&hex, img.to_hex()).unwrap();
    let hex_s = hex.to_string_lossy().to_string();

    let r = run_args(vec![
        "flash".into(),
        "info".into(),
        hex_s.clone(),
        "--json".into(),
    ])
    .await;
    ok(&r);
    assert_eq!(of_type(&r, "imageInfo")[0]["info"]["versionLabel"], "1.1.0");

    let uid = tube_uid(1);
    let r = pmnet(
        &sim,
        &[
            "flash",
            "tube",
            &uid.to_string(),
            &hex_s,
            "--finish-wait-ms",
            "300",
        ],
    )
    .await;
    ok(&r);
    assert_eq!(of_type(&r, "imageInfo").len(), 1);
    let progress = of_type(&r, "progress");
    assert!(progress.iter().any(|p| p["progress"]["phase"] == "writing"));
    assert_eq!(progress.last().unwrap()["progress"]["phase"], "done");
    assert_eq!(
        of_type(&r, "flashReport")[0]["report"]["newVersion"],
        0x0001_0100
    );
    let v = sim.with_responder::<SimTube, _>(uid, |t| t.app_version());
    assert_eq!(v, Some(Some(0x0001_0100)));

    // an older version is refused without --allow-downgrade
    let old = dir.join("tube_fw-1.0.0.hex");
    let img = build_test_image(
        &app(),
        pids::ESTA_MANUFACTURER,
        pids::MODEL_TUBE,
        0x0001_0000,
    );
    std::fs::write(&old, img.to_hex()).unwrap();
    let old_s = old.to_string_lossy().to_string();
    let r = pmnet(&sim, &["flash", "tube", &uid.to_string(), &old_s]).await;
    assert_eq!(r.code, 1);
    assert!(r.stderr.contains("\"code\":\"flash\""), "{}", r.stderr);
    assert_eq!(
        sim.with_responder::<SimTube, _>(uid, |t| t.app_version()),
        Some(Some(0x0001_0100))
    );
    let _ = std::fs::remove_dir_all(&dir);
    sim.shutdown();
}

#[tokio::test(flavor = "multi_thread")]
async fn usage_error_exits_2() {
    let r = run_args(vec!["no-such-command".into()]).await;
    assert_eq!(r.code, 2);
}

#[test]
fn sim_subcommand_prints_address() {
    let mut child = Command::new(BIN)
        .args(["--json", "sim", "--listen", "127.0.0.1:0", "--tubes", "2"])
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .unwrap();
    let mut line = String::new();
    BufReader::new(child.stdout.take().unwrap())
        .read_line(&mut line)
        .unwrap();
    let _ = child.kill();
    let _ = child.wait();
    let v: Value = serde_json::from_str(&line).unwrap();
    assert_eq!(v["type"], "sim");
    let addr: std::net::SocketAddr = v["address"].as_str().unwrap().parse().unwrap();
    assert_ne!(addr.port(), 0);
    assert_eq!(v["tubes"].as_array().unwrap().len(), 2);
}

#[tokio::test(flavor = "multi_thread")]
async fn node_image_alf_and_flash() {
    use pixelmapper_net::image::node::build_test_bin;
    let sim = sim().await;
    let dir = temp_dir("node");
    let bin = dir.join("pixelmapper_node.bin");
    std::fs::write(&bin, build_test_bin(40_000, 0x0000_0200, 0x1, "0.2.0")).unwrap();
    let alf = dir.join("node.alf");
    let (bin_s, alf_s) = (
        bin.to_string_lossy().to_string(),
        alf.to_string_lossy().to_string(),
    );
    let r = run_args(vec![
        "--json".into(),
        "flash".into(),
        "make-alf".into(),
        bin_s.clone(),
        alf_s.clone(),
    ])
    .await;
    ok(&r);
    let r = run_args(vec!["--json".into(), "flash".into(), "info".into(), alf_s]).await;
    ok(&r);
    assert_eq!(of_type(&r, "imageInfo")[0]["kind"], "node");

    let r = pmnet(&sim, &["flash", "node", &bin_s]).await;
    ok(&r);
    assert_eq!(sim.snapshot().fw_version, 0x0000_0200);
    let _ = std::fs::remove_dir_all(&dir);
    sim.shutdown();
}
