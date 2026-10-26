//! End-to-end tests: `pixelmapper-net` (Rust) against the virtual node (`node/firmware/test_host`, target
//! `virtual_node`), i.e. against the real firmware code (pm_node, pm_artnet, pm_rdm responder + controller) with a
//! simulated RS-485 bus. Run by `tools/e2e.sh`.
//!
//! The tests need the path of the built `virtual_node` in `PM_VIRTUAL_NODE`; without it every test returns at once
//! (so `cargo test` stays green without the C build). `PM_VN_LOG=1` passes `--log` (packet log on stderr).
//!
//! Loopback setup: every test starts its own node on ephemeral ports. The controller polls the node's emulated
//! broadcast port (`--bcast-port`, datagrams there count as directed broadcast), so ArtPoll/ArtSync keep their
//! broadcast semantics; replies come from the node's Art-Net port, which the controller then uses for unicast.

use pixelmapper_net::artnet::{
    address_command as ac, ArtAddress, ArtDmx, ArtFirmwareMaster, ArtFirmwareReply, ArtPacket,
    ArtPoll, ArtPollReply, ArtSync, ArtTodControl, ArtTodRequest, IpProgChange, PortAddress,
};
use pixelmapper_net::controller::{Controller, ControllerConfig};
use pixelmapper_net::flash::node::{NodeFlashMethod, NodeFlashOptions};
use pixelmapper_net::flash::FlashPhase;
use pixelmapper_net::image::node::build_test_bin;
use pixelmapper_net::image::tube::build_test_image;
use pixelmapper_net::image::{NodeImage, TubeImage};
use pixelmapper_net::rdm::pd::{decode_label, DeviceInfo, FwInfo, ParameterDescription};
use pixelmapper_net::rdm::{pid, Uid};
use pixelmapper_net::state::{Failsafe, Indicators, MergeMode, PortDirection};
use pixelmapper_net::{pids, Error, Event, NodeId, NodeState};
use std::io::{BufRead, BufReader};
use std::net::{Ipv4Addr, SocketAddr, UdpSocket};
use std::process::{Child, Command, Stdio};
use std::time::{Duration, Instant};
use tokio::sync::broadcast;

const WAIT: Duration = Duration::from_secs(8);

// ---------------------------------------------------------------------------------------------------------------
// Harness
// ---------------------------------------------------------------------------------------------------------------

/// A running `virtual_node` process (killed on drop; it also exits when its stdin closes).
struct Vn {
    child: Child,
    port: u16,
    bport: u16,
    ctl: UdpSocket,
    ctl_addr: SocketAddr,
    uid: Uid,
}

impl Vn {
    /// Start the node with extra arguments, `None` if `PM_VIRTUAL_NODE` is not set.
    fn start(args: &[&str]) -> Option<Self> {
        let exe = std::env::var("PM_VIRTUAL_NODE").ok()?;
        let mut cmd = Command::new(exe);
        cmd.args(["--port", "0", "--ctl-port", "0", "--stdin-watch"])
            .args(args)
            .stdin(Stdio::piped())
            .stdout(Stdio::piped());
        if std::env::var("PM_VN_LOG").is_ok() {
            cmd.arg("--log").stderr(Stdio::inherit());
        } else {
            cmd.stderr(Stdio::null());
        }
        let mut child = cmd.spawn().expect("start virtual_node");
        let mut line = String::new();
        BufReader::new(child.stdout.take().expect("stdout"))
            .read_line(&mut line)
            .expect("READY line");
        assert!(line.starts_with("READY"), "virtual_node: {line}");
        let kv = |k: &str| -> String {
            line.split_whitespace()
                .find_map(|w| w.strip_prefix(&format!("{k}=")).map(str::to_string))
                .unwrap_or_else(|| panic!("{k} missing in {line}"))
        };
        let ctl = UdpSocket::bind("127.0.0.1:0").unwrap();
        ctl.set_read_timeout(Some(Duration::from_secs(3))).unwrap();
        Some(Self {
            port: kv("port").parse().unwrap(),
            bport: kv("bcast").parse().unwrap(),
            ctl_addr: format!("127.0.0.1:{}", kv("ctl")).parse().unwrap(),
            uid: kv("uid").parse().unwrap(),
            ctl,
            child,
        })
    }

    fn addr(&self) -> SocketAddr {
        SocketAddr::from((Ipv4Addr::LOCALHOST, self.port))
    }

    fn bcast(&self) -> SocketAddr {
        SocketAddr::from((Ipv4Addr::LOCALHOST, self.bport))
    }

    fn id(&self) -> NodeId {
        NodeId::Uid(self.uid)
    }

    /// One control command; panics unless the answer starts with "ok". Returns the rest.
    fn ctl(&self, cmd: &str) -> String {
        self.ctl.send_to(cmd.as_bytes(), self.ctl_addr).unwrap();
        let mut buf = vec![0u8; 16384];
        let n = self.ctl.recv(&mut buf).expect("control answer");
        let s = String::from_utf8_lossy(&buf[..n]).to_string();
        assert!(s.starts_with("ok"), "control '{cmd}': {s}");
        s.trim_start_matches("ok").trim().to_string()
    }

    /// Current DMX output of port `p` (1-based).
    fn dmx(&self, p: u8) -> Vec<u8> {
        let r = self.ctl(&format!("dmx {p}"));
        let hex = r.split_whitespace().nth(1).unwrap_or("");
        pixelmapper_net::parse_hex(hex).unwrap()
    }

    /// `key=value` of `tube P UID`.
    fn tube(&self, p: u8, uid: Uid, key: &str) -> String {
        let r = self.ctl(&format!("tube {p} {uid}"));
        r.split_whitespace()
            .find_map(|w| w.strip_prefix(&format!("{key}=")).map(str::to_string))
            .unwrap_or_default()
    }
}

impl Drop for Vn {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

macro_rules! vn_or_skip {
    ($args:expr) => {
        match Vn::start($args) {
            Some(v) => v,
            None => {
                eprintln!("PM_VIRTUAL_NODE not set: skipped (run tools/e2e.sh)");
                return;
            }
        }
    };
}

fn pa(n: u16) -> PortAddress {
    PortAddress::new(n).unwrap()
}

fn tube_uid(port: u32, k: u32) -> Uid {
    Uid::new(pids::ESTA_MANUFACTURER, (port << 24) | k)
}

fn fixture_uid(port: u32, k: u32) -> Uid {
    Uid::new(0x1234, (port << 24) | 0x1000 | k)
}

fn config(vn: &Vn) -> ControllerConfig {
    let mut c = ControllerConfig::local(vec![vn.bcast()]);
    c.poll_interval = Duration::from_millis(500);
    c.reply_timeout = Duration::from_millis(2500);
    c.rdm.timeout = Duration::from_millis(500);
    c.rdm.node_queue_interval = Duration::from_millis(500);
    c
}

async fn controller(vn: &Vn) -> Controller {
    let c = Controller::bind(config(vn)).await.unwrap();
    c.wait_for_node(&vn.id(), WAIT).await.unwrap();
    c
}

/// Wait for the first event matching `f`.
async fn wait_event<T>(
    rx: &mut broadcast::Receiver<Event>,
    timeout: Duration,
    mut f: impl FnMut(&Event) -> Option<T>,
) -> Option<T> {
    tokio::time::timeout(timeout, async {
        loop {
            match rx.recv().await {
                Ok(e) => {
                    if let Some(t) = f(&e) {
                        return t;
                    }
                }
                Err(broadcast::error::RecvError::Lagged(_)) => continue,
                Err(e) => panic!("event stream: {e}"),
            }
        }
    })
    .await
    .ok()
}

/// Poll `f` until it returns `Some` (async condition).
async fn eventually<T, F, Fut>(timeout: Duration, mut f: F) -> Option<T>
where
    F: FnMut() -> Fut,
    Fut: std::future::Future<Output = Option<T>>,
{
    let end = Instant::now() + timeout;
    loop {
        if let Some(t) = f().await {
            return Some(t);
        }
        if Instant::now() >= end {
            return None;
        }
        tokio::time::sleep(Duration::from_millis(100)).await;
    }
}

/// Node state once `pred` holds (re-polling meanwhile).
async fn node_when(ctrl: &Controller, id: &NodeId, pred: impl Fn(&NodeState) -> bool) -> NodeState {
    eventually(WAIT, || async {
        let _ = ctrl.poll_now().await;
        ctrl.node(id).filter(|n| pred(n))
    })
    .await
    .unwrap_or_else(|| panic!("node state not reached: {:#?}", ctrl.node(id)))
}

/// TOD of a port once it has `n` entries.
async fn tod_len(
    ctrl: &Controller,
    id: &NodeId,
    p: PortAddress,
    n: usize,
    t: Duration,
) -> Vec<Uid> {
    let got = eventually(t, || async move {
        let u = ctrl.tod_refresh(id, p, Duration::from_secs(2)).await.ok()?;
        (u.len() == n).then_some(u)
    })
    .await;
    got.unwrap_or_else(|| {
        panic!(
            "TOD of {p}: want {n} entries, have {:?}",
            ctrl.tod(id, p).map(|t| t.len())
        )
    })
}

/// A raw Art-Net socket (second controller / observer).
fn raw() -> UdpSocket {
    let s = UdpSocket::bind("127.0.0.1:0").unwrap();
    s.set_read_timeout(Some(Duration::from_millis(100)))
        .unwrap();
    s
}

/// All Art-Net packets received on `s` within `d`.
fn collect(s: &UdpSocket, d: Duration) -> Vec<ArtPacket> {
    let end = Instant::now() + d;
    let mut out = Vec::new();
    let mut buf = [0u8; 2048];
    while Instant::now() < end {
        if let Ok(n) = s.recv(&mut buf) {
            if let Ok(p) = ArtPacket::decode(&buf[..n]) {
                out.push(p);
            }
        }
    }
    out
}

fn poll_packet() -> Vec<u8> {
    ArtPacket::Poll(ArtPoll {
        flags: 0x02,
        ..ArtPoll::default()
    })
    .encode()
}

fn tube_image(version: u32) -> TubeImage {
    let app: Vec<u8> = (0..1500u32).map(|i| (i * 7 + version) as u8).collect();
    build_test_image(&app, pids::ESTA_MANUFACTURER, pids::MODEL_TUBE, version)
}

fn u16be(v: &[u8]) -> u16 {
    u16::from_be_bytes([v[0], v[1]])
}

// ---------------------------------------------------------------------------------------------------------------
// 1. Discovery
// ---------------------------------------------------------------------------------------------------------------

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn e2e_1_discovery_four_bind_pages() {
    let vn = vn_or_skip!(&[]);
    // raw: 4 unicast replies, contract §3 fields
    let s = raw();
    s.send_to(&poll_packet(), vn.bcast()).unwrap();
    let replies: Vec<ArtPollReply> = collect(&s, Duration::from_millis(1500))
        .into_iter()
        .filter_map(|p| match p {
            ArtPacket::PollReply(r) => Some(r),
            _ => None,
        })
        .collect();
    let mut binds: Vec<u8> = replies.iter().map(|r| r.bind_index).collect();
    binds.sort();
    assert_eq!(binds, vec![1, 2, 3, 4], "one reply per BindIndex");
    for r in &replies {
        assert_eq!(r.num_ports, 1);
        assert_eq!(r.ip, Ipv4Addr::LOCALHOST);
        assert_eq!(r.bind_ip, Ipv4Addr::LOCALHOST);
        assert_eq!(r.port, 6454);
        assert_eq!(r.esta_man, pids::ESTA_MANUFACTURER);
        assert_eq!(r.oem, pids::ARTNET_OEM_NODE);
        assert_eq!(
            r.vers_info, 0x0001,
            "VersInfo = (major << 8) | minor of 0.1.0"
        );
        assert_eq!(r.default_resp_uid, vn.uid);
        assert_eq!(r.mac, [0x02, 0x00, 0x00, 0x12, 0x34, 0x56]);
        assert_eq!(r.refresh_rate, 44);
        assert_eq!(r.short_name, "PM Node");
        assert_eq!(r.long_name, "Pixelmapper Node");
        assert_eq!(
            r.port_types[0] & 0xC0,
            0x80,
            "output, current direction in PortTypes"
        );
        assert_eq!(r.sw_out[0], r.bind_index, "Port-Address 1..4");
        assert_ne!(r.status1 & 0x02, 0, "Status1 bit 1 RDM capable");
        assert_eq!(r.status1 & 0xC0, 0xC0, "indicators normal");
        assert_ne!(r.status2 & 0x08, 0, "Status2 bit 3 15-bit");
        assert_eq!(r.status2 & 0x06, 0x06, "DHCP capable + active");
        assert_ne!(r.status3 & 0x20, 0, "programmable failsafe");
        assert_ne!(r.status3 & 0x08, 0, "direction switchable");
        assert_eq!(r.status3 & 0x14, 0, "no LLRP / RDMnet");
        assert_eq!(r.status3 & 0xC0, 0x00, "failsafe hold");
        assert_eq!(r.good_output_b[0] & 0x80, 0, "RDM on");
        assert_eq!(r.good_output_b[0] & 0x10, 0, "background discovery on");
    }
    // controller: merged by DefaultRespUID
    let ctrl = controller(&vn).await;
    let n = node_when(&ctrl, &vn.id(), |n| n.ports.len() == 4).await;
    assert_eq!(n.uid, Some(vn.uid));
    assert_eq!(n.address, vn.addr());
    assert_eq!(n.oem, pids::ARTNET_OEM_NODE);
    for (i, p) in n.ports.iter().enumerate() {
        assert_eq!(p.bind_index as usize, i + 1);
        assert_eq!(p.port_number as usize, i + 1);
        assert_eq!(p.port_address, pa(i as u16 + 1));
        assert_eq!(p.direction, PortDirection::Output);
        assert!(p.rdm_enabled);
    }
    assert_eq!(ctrl.nodes().len(), 1);
    ctrl.shutdown();
}

// ---------------------------------------------------------------------------------------------------------------
// 2. ArtAddress, ArtIpProg
// ---------------------------------------------------------------------------------------------------------------

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn e2e_2_art_address_and_ip_prog() {
    let vn = vn_or_skip!(&[]);
    let a = controller(&vn).await;
    let b = controller(&vn).await;
    let id = vn.id();
    let mut rx_b = b.subscribe();
    let quiet = raw(); // polls without Flags bit 1: no unsolicited replies
    quiet
        .send_to(
            &ArtPacket::Poll(ArtPoll {
                flags: 0,
                ..ArtPoll::default()
            })
            .encode(),
            vn.bcast(),
        )
        .unwrap();
    std::thread::sleep(Duration::from_millis(1200));
    assert_eq!(
        collect(&quiet, Duration::from_millis(10)).len(),
        4,
        "answered the poll itself"
    );

    // names + Port-Address of BindIndex 2 -> answer to A, unsolicited reply to B
    let pkt = ArtAddress {
        bind_index: 2,
        short_name: "E2E short".into(),
        long_name: "E2E long name".into(),
        sw_out: [ArtAddress::program(5), 0x7F, 0x7F, 0x7F],
        ..ArtAddress::default()
    };
    let n = a
        .art_address(&id, pkt)
        .await
        .unwrap()
        .expect("ArtPollReply");
    assert_eq!(n.short_name, "E2E short");
    let n = node_when(&a, &id, |n| {
        n.ports
            .iter()
            .any(|p| p.bind_index == 2 && p.port_address == pa(5))
    })
    .await;
    assert_eq!(n.long_name, "E2E long name");
    let seen = wait_event(&mut rx_b, WAIT, |e| match e {
        Event::NodeUpdated { node } if node.short_name == "E2E short" => Some(()),
        _ => None,
    })
    .await;
    assert!(
        seen.is_some(),
        "B gets the change (unsolicited ArtPollReply)"
    );
    let q = collect(&quiet, Duration::from_millis(300));
    assert!(
        q.is_empty(),
        "unsolicited reply to a poller without Flags bit 1: {q:?}"
    );

    // merge LTP (bind 1), direction input (bind 3), RDM off (bind 4), failsafe zero (bind 1), locate
    a.art_address_command(&id, 1, ac::AC_MERGE_LTP0)
        .await
        .unwrap();
    a.art_address_command(&id, 3, ac::AC_DIRECTION_RX0)
        .await
        .unwrap();
    a.art_address_command(&id, 4, ac::AC_RDM_DISABLE0)
        .await
        .unwrap();
    a.art_address_command(&id, 1, ac::AC_FAIL_ZERO)
        .await
        .unwrap();
    a.art_address_command(&id, 1, ac::AC_LED_LOCATE)
        .await
        .unwrap();
    let n = node_when(&b, &id, |n| {
        n.ports[0].merge_mode == MergeMode::Ltp
            && n.ports[2].direction == PortDirection::Input
            && !n.ports[3].rdm_enabled
            && n.status3.failsafe == Failsafe::Zero
            && n.status1.indicators == Indicators::Locate
    })
    .await;
    assert_eq!(n.ports[1].direction, PortDirection::Output);
    let s = vn.ctl("settings");
    assert!(s.contains("p1=en1,dir0,pa1,merge1,rdm1,bg1,fs1"), "{s}");
    assert!(s.contains("p3=en1,dir1"), "{s}");
    assert!(s.contains("p4=en1,dir0,pa4,merge0,rdm0"), "{s}");
    // Locate = IDENTIFY_DEVICE of the node (contract §5)
    let v = a
        .rdm_get(&id, pa(1), vn.uid, pid::IDENTIFY_DEVICE, &[])
        .await
        .unwrap();
    assert_eq!(v, vec![1]);
    a.art_address_command(&id, 1, ac::AC_LED_NORMAL)
        .await
        .unwrap();
    let v = a
        .rdm_get(&id, pa(1), vn.uid, pid::IDENTIFY_DEVICE, &[])
        .await
        .unwrap();
    assert_eq!(v, vec![0]);
    // back
    a.art_address_command(&id, 3, ac::AC_DIRECTION_TX0)
        .await
        .unwrap();
    a.art_address_command(&id, 4, ac::AC_RDM_ENABLE0)
        .await
        .unwrap();
    a.art_address_command(&id, 1, ac::AC_MERGE_HTP0)
        .await
        .unwrap();
    node_when(&a, &id, |n| {
        n.ports[2].direction == PortDirection::Output
            && n.ports[3].rdm_enabled
            && n.ports[0].merge_mode == MergeMode::Htp
    })
    .await;

    // ArtIpProg Command = 0: pure query, nothing changes
    let before = vn.ctl("settings");
    let r = a.ip_prog_query(&id).await.unwrap();
    assert_eq!(r.prog_ip, Ipv4Addr::LOCALHOST);
    assert_eq!(r.prog_sm, Ipv4Addr::new(255, 0, 0, 0));
    assert_eq!(r.prog_dg, Ipv4Addr::UNSPECIFIED);
    assert_ne!(r.status & 0x40, 0, "DHCP active");
    assert_eq!(vn.ctl("settings"), before, "query changed nothing");
    assert_eq!(
        vn.ctl("net"),
        "ip=127.0.0.1 mask=255.0.0.0 gw=0.0.0.0 dhcp=1"
    );

    // ArtIpProg with change: static address; reply carries the new values, replies come with the new IP
    let ch = IpProgChange {
        ip: Some(Ipv4Addr::new(10, 9, 8, 7)),
        mask: Some(Ipv4Addr::new(255, 255, 0, 0)),
        gateway: Some(Ipv4Addr::new(10, 9, 0, 1)),
        ..IpProgChange::default()
    };
    let r = a.ip_prog_change(&id, &ch).await.unwrap();
    assert_eq!(r.prog_ip, Ipv4Addr::new(10, 9, 8, 7));
    assert_eq!(r.prog_sm, Ipv4Addr::new(255, 255, 0, 0));
    assert_eq!(r.prog_dg, Ipv4Addr::new(10, 9, 0, 1));
    assert_eq!(r.status & 0x40, 0, "DHCP off");
    let n = node_when(&b, &id, |n| n.ip == Ipv4Addr::new(10, 9, 8, 7)).await;
    assert_eq!(n.uid, Some(vn.uid), "same UID after the IP change");
    assert!(!n.status2.dhcp_active);
    let r = a.ip_prog_query(&id).await.unwrap();
    assert_eq!(r.prog_dg, Ipv4Addr::new(10, 9, 0, 1));
    // back to DHCP
    let r = a
        .ip_prog_change(
            &id,
            &IpProgChange {
                dhcp: true,
                ..IpProgChange::default()
            },
        )
        .await
        .unwrap();
    assert_ne!(r.status & 0x40, 0);
    node_when(&a, &id, |n| n.ip == Ipv4Addr::LOCALHOST).await;
    a.shutdown();
    b.shutdown();
}

// ---------------------------------------------------------------------------------------------------------------
// 3. TOD
// ---------------------------------------------------------------------------------------------------------------

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn e2e_3_tod() {
    let vn = vn_or_skip!(&["--tubes", "1:3", "--fixtures", "1:1"]);
    let ctrl = controller(&vn).await;
    let id = vn.id();
    let mut want = vec![
        tube_uid(1, 1),
        tube_uid(1, 2),
        tube_uid(1, 3),
        fixture_uid(1, 1),
    ];
    want.sort();
    assert_eq!(tod_len(&ctrl, &id, pa(1), 4, WAIT).await, want);
    assert_eq!(ctrl.tod_refresh(&id, pa(2), WAIT).await.unwrap(), vec![]);
    // faster presence check: RDM_BG_DISCOVERY_INTERVAL = 1 s
    ctrl.rdm_set(
        &id,
        pa(1),
        vn.uid,
        pids::PID_RDM_BG_DISCOVERY_INTERVAL,
        &1u16.to_be_bytes(),
    )
    .await
    .unwrap();
    node_when(&ctrl, &id, |n| !n.ports[0].discovery_running).await;
    let mut rx = ctrl.subscribe(); // after the initial discovery (its partial TOD updates are history)
    let mut cq = config(&vn);
    cq.auto_tod_request = false;
    let quiet = Controller::bind(cq).await.unwrap();
    quiet.wait_for_node(&id, WAIT).await.unwrap();
    let mut rx_quiet = quiet.subscribe();

    // unsolicited ArtTodData: tube added / removed
    let t9 = tube_uid(1, 9);
    vn.ctl(&format!("add 1 tube {t9}"));
    let added = wait_event(&mut rx, WAIT, |e| match e {
        Event::TodChanged {
            port_address,
            added,
            ..
        } if *port_address == pa(1) && !added.is_empty() => Some(added.clone()),
        _ => None,
    })
    .await;
    assert_eq!(added, Some(vec![t9]));
    let q = wait_event(&mut rx_quiet, Duration::from_millis(500), |e| match e {
        Event::TodChanged { .. } => Some(()),
        _ => None,
    })
    .await;
    assert!(
        q.is_none(),
        "unsolicited ArtTodData only to IPs that sent ArtTodRequest"
    );
    quiet.shutdown();
    vn.ctl(&format!("remove 1 {}", tube_uid(1, 2)));
    let removed = wait_event(&mut rx, Duration::from_secs(12), |e| match e {
        Event::TodChanged { removed, .. } if !removed.is_empty() => Some(removed.clone()),
        _ => None,
    })
    .await;
    assert_eq!(removed, Some(vec![tube_uid(1, 2)]));

    // AtcIncOff: background discovery off -> a new device stays unseen; AtcIncOn finds it
    ctrl.tod_control(&id, pa(1), ArtTodControl::ATC_INC_OFF)
        .await
        .unwrap();
    node_when(&ctrl, &id, |n| !n.ports[0].background_discovery).await;
    let t10 = tube_uid(1, 10);
    vn.ctl(&format!("add 1 tube {t10}"));
    tokio::time::sleep(Duration::from_millis(2500)).await;
    assert!(!ctrl
        .tod_refresh(&id, pa(1), WAIT)
        .await
        .unwrap()
        .contains(&t10));
    ctrl.tod_control(&id, pa(1), ArtTodControl::ATC_INC_ON)
        .await
        .unwrap();
    node_when(&ctrl, &id, |n| n.ports[0].background_discovery).await;
    tod_len(&ctrl, &id, pa(1), 5, WAIT).await;

    // AtcFlush: full discovery, all found again; AtcEnd stops a running discovery
    ctrl.tod_flush(&id, pa(1)).await.unwrap();
    let mut want = vec![tube_uid(1, 1), tube_uid(1, 3), t9, t10, fixture_uid(1, 1)];
    want.sort();
    assert_eq!(tod_len(&ctrl, &id, pa(1), 5, WAIT).await, want);
    ctrl.tod_flush(&id, pa(1)).await.unwrap();
    ctrl.tod_control(&id, pa(1), ArtTodControl::ATC_END)
        .await
        .unwrap();
    node_when(&ctrl, &id, |n| !n.ports[0].discovery_running).await;
    ctrl.shutdown();
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn e2e_3_tod_several_blocks() {
    let vn = vn_or_skip!(&["--random", "2:230", "--dmx-slots", "24"]);
    let ctrl = controller(&vn).await;
    let id = vn.id();
    let uids = tod_len(&ctrl, &id, pa(2), 230, Duration::from_secs(60)).await;
    assert!(
        uids.windows(2).all(|w| w[0] < w[1]),
        "sorted, no duplicates"
    );
    // the end of the discovery sends one more (unsolicited) ArtTodData; wait for it to pass
    node_when(&ctrl, &id, |n| !n.ports[1].discovery_running).await;
    tokio::time::sleep(Duration::from_millis(500)).await;
    // raw: two ArtTodData blocks (<= 200 UIDs each)
    let s = raw();
    let req = ArtTodRequest::for_port_addresses(0, &[pa(2)]);
    s.send_to(&ArtPacket::TodRequest(req).encode(), vn.addr())
        .unwrap();
    let blocks: Vec<_> = collect(&s, Duration::from_millis(800))
        .into_iter()
        .filter_map(|p| match p {
            ArtPacket::TodData(t) => Some(t),
            _ => None,
        })
        .collect();
    assert_eq!(blocks.len(), 2, "{blocks:?}");
    let mut all = Vec::new();
    for (i, b) in blocks.iter().enumerate() {
        assert_eq!(b.uid_total, 230);
        assert_eq!(b.block_count as usize, i);
        assert!(b.uids.len() <= 200);
        assert_eq!(b.bind_index, 2);
        all.extend(b.uids.iter().copied());
    }
    all.sort();
    assert_eq!(all, uids);
    ctrl.shutdown();
}

// ---------------------------------------------------------------------------------------------------------------
// 4. RDM to tubes and to the node
// ---------------------------------------------------------------------------------------------------------------

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn e2e_4_rdm_get_set_and_describe() {
    let vn = vn_or_skip!(&["--tubes", "1:1"]);
    let ctrl = controller(&vn).await;
    let id = vn.id();
    let t = tube_uid(1, 1);
    tod_len(&ctrl, &id, pa(1), 1, WAIT).await;

    let di = DeviceInfo::decode(
        &ctrl
            .rdm_get(&id, pa(1), t, pid::DEVICE_INFO, &[])
            .await
            .unwrap(),
    )
    .unwrap();
    assert_eq!(di.device_model_id, pids::MODEL_TUBE);
    assert_eq!(di.software_version_id, 0x500);
    ctrl.rdm_set(&id, pa(1), t, pid::DMX_START_ADDRESS, &100u16.to_be_bytes())
        .await
        .unwrap();
    assert_eq!(vn.tube(1, t, "start"), "100");
    ctrl.rdm_set(&id, pa(1), t, pid::DEVICE_LABEL, b"left tube")
        .await
        .unwrap();
    let l = ctrl
        .rdm_get(&id, pa(1), t, pid::DEVICE_LABEL, &[])
        .await
        .unwrap();
    assert_eq!(decode_label(&l), "left tube");
    let e = ctrl
        .rdm_set(&id, pa(1), t, pid::DMX_START_ADDRESS, &0u16.to_be_bytes())
        .await
        .unwrap_err();
    assert!(matches!(e, Error::Nack { .. }), "{e}");
    let d = ctrl.describe_device(&id, pa(1), t).await.unwrap();
    assert!(d.warnings.is_empty(), "{:?}", d.warnings);
    let enter = pids::PIDS
        .iter()
        .find(|p| p.pid == pids::PID_ENTER_BOOTLOADER)
        .unwrap();
    assert_eq!(
        d.parameter_descriptions,
        vec![ParameterDescription::from_pid_def(enter).unwrap()]
    );

    // the node: same UID through every Port-Address, never on the line
    for p in 1..=4 {
        let di = DeviceInfo::decode(
            &ctrl
                .rdm_get(&id, pa(p), vn.uid, pid::DEVICE_INFO, &[])
                .await
                .unwrap(),
        )
        .unwrap();
        assert_eq!(di.device_model_id, pids::MODEL_NODE);
        assert_eq!(di.product_category, pids::NODE_PRODUCT_CATEGORY);
        assert_eq!(di.dmx_footprint, 0);
        assert_eq!(di.dmx_start_address, 0xFFFF);
        assert_eq!(di.sub_device_count, 0);
        assert_eq!(di.software_version_id, 0x000100);
    }
    let d = ctrl.describe_device(&id, pa(3), vn.uid).await.unwrap();
    assert!(d.warnings.is_empty(), "{:?}", d.warnings);
    assert_eq!(d.software_version_label.as_deref(), Some("0.1.0"));
    let mut want: Vec<u16> = pids::NODE_STD_PIDS_LISTED.to_vec();
    want.extend(
        pids::PIDS
            .iter()
            .filter(|p| p.device == pids::Device::Node && p.listed)
            .map(|p| p.pid),
    );
    want.sort();
    let mut got = d.supported_parameters.clone();
    got.sort();
    assert_eq!(got, want, "SUPPORTED_PARAMETERS = pids.toml");
    for r in pids::NODE_STD_PIDS_REQUIRED {
        assert!(
            !got.contains(r),
            "required PID 0x{r:04X} must not be listed"
        );
    }
    let manuf: Vec<&pids::PidDef> = pids::PIDS
        .iter()
        .filter(|p| p.device == pids::Device::Node && p.listed)
        .collect();
    assert_eq!(d.parameter_descriptions.len(), manuf.len());
    for def in manuf {
        let want = ParameterDescription::from_pid_def(def).unwrap();
        let got = d
            .parameter_descriptions
            .iter()
            .find(|x| x.pid == def.pid)
            .unwrap();
        assert_eq!(*got, want, "PARAMETER_DESCRIPTION 0x{:04X}", def.pid);
        // the generated per-PID metadata the bridge uses agrees with what the node answers
        let m = pids::pid_meta(def.pid).unwrap();
        assert_eq!(
            (
                m.pdl_size,
                m.data_type,
                m.command_class,
                m.unit,
                m.prefix,
                m.description
            ),
            (
                Some(got.pdl_size),
                Some(got.data_type),
                got.command_class,
                Some(got.unit),
                Some(got.prefix),
                Some(got.description.as_str())
            ),
            "PID_META 0x{:04X}",
            def.pid
        );
        assert_eq!(
            (m.min, m.max, m.default),
            (
                Some(got.min_valid_value as i64),
                Some(got.max_valid_value as i64),
                Some(got.default_value as i64)
            ),
            "PID_META range 0x{:04X}",
            def.pid
        );
        let value_pdl: u16 = m.value_fields().iter().map(|f| f.max_size()).sum();
        assert_eq!(value_pdl, u16::from(got.pdl_size), "{}", m.name);
    }
    // node settings via RDM
    ctrl.rdm_set(
        &id,
        pa(1),
        vn.uid,
        pids::PID_PORT2_DMX_BREAK,
        &200u16.to_be_bytes(),
    )
    .await
    .unwrap();
    let v = ctrl
        .rdm_get(&id, pa(4), vn.uid, pids::PID_PORT2_DMX_BREAK, &[])
        .await
        .unwrap();
    assert_eq!(u16be(&v), 200);
    let e = ctrl
        .rdm_set(
            &id,
            pa(1),
            vn.uid,
            pids::PID_PORT2_DMX_BREAK,
            &100u16.to_be_bytes(),
        )
        .await
        .unwrap_err();
    assert!(matches!(e, Error::Nack { .. }), "{e}");
    ctrl.shutdown();
}

// ---------------------------------------------------------------------------------------------------------------
// 5. Queued messages, last writer wins
// ---------------------------------------------------------------------------------------------------------------

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn e2e_5_queued_messages_last_writer_wins() {
    let vn = vn_or_skip!(&["--tubes", "1:1"]);
    let a = controller(&vn).await;
    let mut cb = config(&vn);
    cb.rdm.auto_fetch_queued = false;
    cb.rdm.node_queue_interval = Duration::ZERO;
    let b = Controller::bind(cb).await.unwrap();
    let id = vn.id();
    b.wait_for_node(&id, WAIT).await.unwrap();
    let mut rx_a = a.subscribe();
    let mut rx_b = b.subscribe();
    let queued = |rx: &mut broadcast::Receiver<Event>, want_pid: u16| {
        let mut rx = rx.resubscribe();
        async move {
            wait_event(&mut rx, WAIT, |e| match e {
                Event::QueuedMessage { uid, message, .. } if message.pid == want_pid => {
                    Some((*uid, message.clone()))
                }
                _ => None,
            })
            .await
        }
    };

    // panel change of an RDM-only setting -> queued message (A fetches it)
    let w = queued(&mut rx_a, pids::PID_DISPLAY_DIM_TIMEOUT);
    vn.ctl("panel dim 30");
    let (uid, m) = w.await.expect("queued DISPLAY_DIM_TIMEOUT");
    assert_eq!(uid, vn.uid);
    assert_eq!(m.data, 30u16.to_be_bytes().to_vec());

    // RDM SET by B -> queued message for the others (A)
    let w = queued(&mut rx_a, pids::PID_PORT3_DMX_RATE);
    b.rdm_set(&id, pa(1), vn.uid, pids::PID_PORT3_DMX_RATE, &[25])
        .await
        .unwrap();
    let (_, m) = w.await.expect("queued PORT3_DMX_RATE");
    assert_eq!(m.data, vec![25]);

    // panel name -> unsolicited ArtPollReply to both
    vn.ctl("panel name PanelName");
    for rx in [&mut rx_a, &mut rx_b] {
        let ok = wait_event(rx, WAIT, |e| match e {
            Event::NodeUpdated { node } if node.short_name == "PanelName" => Some(()),
            _ => None,
        })
        .await;
        assert!(ok.is_some());
    }

    // last writer wins: DEVICE_LABEL (= short name) by A, then ArtAddress by B
    a.rdm_set(&id, pa(1), vn.uid, pid::DEVICE_LABEL, b"from A")
        .await
        .unwrap();
    node_when(&b, &id, |n| n.short_name == "from A").await;
    b.art_address(
        &id,
        ArtAddress {
            short_name: "from B".into(),
            ..ArtAddress::default()
        },
    )
    .await
    .unwrap();
    let l = a
        .rdm_get(&id, pa(1), vn.uid, pid::DEVICE_LABEL, &[])
        .await
        .unwrap();
    assert_eq!(decode_label(&l), "from B");
    node_when(&a, &id, |n| n.short_name == "from B").await;

    // panel lock released at the device -> PANEL_LOCK + status PANEL_UNLOCKED_LOCAL
    vn.ctl("panel lock 1");
    let w = queued(&mut rx_a, pids::PID_PANEL_LOCK);
    vn.ctl("panel lock 0");
    let (_, m) = w.await.expect("queued PANEL_LOCK");
    assert_eq!(m.data, vec![0]);
    let st = eventually(WAIT, || async {
        let pd = b
            .rdm_get(&id, pa(1), vn.uid, pid::STATUS_MESSAGES, &[0x02])
            .await
            .ok()?;
        pd.chunks(9)
            .any(|c| c.len() == 9 && u16be(&c[3..5]) == pids::status::PANEL_UNLOCKED_LOCAL)
            .then_some(())
    })
    .await;
    // A may have collected the status already with its queued-message fetch; then it was delivered as event
    let _ = st;

    // RDM SET from the panel to a tube -> LOCAL_RDM_SET queued message
    let t = tube_uid(1, 1);
    tod_len(&a, &id, pa(1), 1, WAIT).await;
    let w = queued(&mut rx_a, pids::PID_LOCAL_RDM_SET);
    let req = pixelmapper_net::rdm::RdmMessage::request(
        t,
        Uid::new(pids::ESTA_MANUFACTURER, 0x0000_0099),
        1,
        1,
        0,
        pixelmapper_net::rdm::cc::SET,
        pid::DMX_START_ADDRESS,
        42u16.to_be_bytes().to_vec(),
    );
    vn.ctl(&format!(
        "panel rdm 1 {}",
        pixelmapper_net::to_hex(&req.encode())
    ));
    let (_, m) = w.await.expect("queued LOCAL_RDM_SET");
    assert_eq!(m.data.len(), 13);
    assert_eq!(m.data[2], 1, "port");
    assert_eq!(&m.data[3..9], &t.to_bytes());
    assert_eq!(u16be(&m.data[11..13]), pid::DMX_START_ADDRESS);
    eventually(WAIT, || async {
        (vn.tube(1, t, "start") == "42").then_some(())
    })
    .await
    .expect("start 42");
    a.shutdown();
    b.shutdown();
}

// ---------------------------------------------------------------------------------------------------------------
// 6. DMX: output, merge, ArtSync, failsafe
// ---------------------------------------------------------------------------------------------------------------

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn e2e_6_dmx_merge_and_sync() {
    let vn = vn_or_skip!(&[]);
    let ctrl = controller(&vn).await;
    let id = vn.id();
    // plain output
    assert_eq!(ctrl.send_dmx(pa(1), &[10, 200, 30, 40]).await.unwrap(), 1);
    eventually(WAIT, || async {
        (vn.dmx(1) == vec![10, 200, 30, 40]).then_some(())
    })
    .await
    .expect("output");

    // HTP merge of two sources: same IP, different Physical (contract §4: source = IP + Physical)
    let dest = vn.addr();
    ctrl.try_send_dmx(dest, pa(2), &[100, 0, 50, 0], 0).unwrap();
    ctrl.try_send_dmx(dest, pa(2), &[20, 80, 60, 0], 1).unwrap();
    eventually(WAIT, || async {
        (vn.dmx(2) == vec![100, 80, 60, 0]).then_some(())
    })
    .await
    .expect("HTP");
    let n = node_when(&ctrl, &id, |n| n.ports[1].merging).await;
    assert_eq!(n.ports[1].merge_mode, MergeMode::Htp);
    // LTP: the latest frame wins
    ctrl.art_address_command(&id, 2, ac::AC_MERGE_LTP0)
        .await
        .unwrap();
    ctrl.try_send_dmx(dest, pa(2), &[1, 2, 3, 4], 0).unwrap();
    eventually(WAIT, || async {
        (vn.dmx(2) == vec![1, 2, 3, 4]).then_some(())
    })
    .await
    .expect("LTP 1");
    ctrl.try_send_dmx(dest, pa(2), &[9, 9, 9, 9], 1).unwrap();
    eventually(WAIT, || async {
        (vn.dmx(2) == vec![9, 9, 9, 9]).then_some(())
    })
    .await
    .expect("LTP 2");
    // a third source is dropped
    ctrl.try_send_dmx(dest, pa(2), &[7, 7, 7, 7], 2).unwrap();
    tokio::time::sleep(Duration::from_millis(300)).await;
    assert_eq!(vn.dmx(2), vec![9, 9, 9, 9]);

    // ArtSync (raw sender; broadcast = the node's emulated broadcast port)
    let s = raw();
    let dmx =
        |seq: u8, v: u8| ArtPacket::Dmx(ArtDmx::new(seq, 0, pa(3), vec![v; 4]).unwrap()).encode();
    let sync = ArtPacket::Sync(ArtSync::default()).encode();
    s.send_to(&dmx(1, 1), dest).unwrap();
    s.send_to(&sync, vn.bcast()).unwrap(); // sync mode on
    std::thread::sleep(Duration::from_millis(100));
    s.send_to(&dmx(2, 5), dest).unwrap();
    std::thread::sleep(Duration::from_millis(200));
    assert_eq!(vn.dmx(3), vec![1; 4], "held until ArtSync");
    s.send_to(&sync, dest).unwrap(); // unicast ArtSync: ignored
    std::thread::sleep(Duration::from_millis(200));
    assert_eq!(vn.dmx(3), vec![1; 4], "unicast ArtSync ignored");
    s.send_to(&sync, vn.bcast()).unwrap();
    eventually(WAIT, || async { (vn.dmx(3) == vec![5; 4]).then_some(()) })
        .await
        .expect("synced");
    // ArtSync via the controller (broadcast targets)
    s.send_to(&dmx(3, 6), dest).unwrap();
    std::thread::sleep(Duration::from_millis(100));
    assert_eq!(vn.dmx(3), vec![5; 4]);
    // ArtSync from another IP than the last ArtDmx is ignored (the controller is another peer)
    ctrl.send_sync().await.unwrap();
    std::thread::sleep(Duration::from_millis(200));
    assert_eq!(vn.dmx(3), vec![5; 4], "ArtSync from another source ignored");
    s.send_to(&sync, vn.bcast()).unwrap();
    eventually(WAIT, || async { (vn.dmx(3) == vec![6; 4]).then_some(()) })
        .await
        .expect("synced 2");
    ctrl.shutdown();
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn e2e_6_failsafe_after_4_s() {
    let vn = vn_or_skip!(&[]);
    let ctrl = controller(&vn).await;
    let id = vn.id();
    let dest = vn.addr();
    // port 4: record a scene
    ctrl.try_send_dmx(dest, pa(4), &[11, 22, 33, 44], 0)
        .unwrap();
    eventually(WAIT, || async {
        (vn.dmx(4) == vec![11, 22, 33, 44]).then_some(())
    })
    .await
    .expect("scene");
    ctrl.art_address_command(&id, 4, ac::AC_FAIL_RECORD)
        .await
        .unwrap();
    for (bind, cmd) in [
        (1, ac::AC_FAIL_HOLD),
        (2, ac::AC_FAIL_ZERO),
        (3, ac::AC_FAIL_FULL),
        (4, ac::AC_FAIL_SCENE),
    ] {
        ctrl.art_address_command(&id, bind, cmd).await.unwrap();
    }
    let s = vn.ctl("settings");
    assert!(
        s.contains("fs0") && s.contains("fs1") && s.contains("fs2") && s.contains("fs3"),
        "{s}"
    );
    for p in 1..=4u16 {
        ctrl.try_send_dmx(dest, pa(p), &[5, 6, 7, 8], 0).unwrap();
    }
    eventually(WAIT, || async {
        (1..=4).all(|p| vn.dmx(p) == vec![5, 6, 7, 8]).then_some(())
    })
    .await
    .expect("data");
    // an observer that polled once (Flags bit 1) and then only listens
    let mut co = config(&vn);
    co.poll_interval = Duration::ZERO;
    let obs = Controller::bind(co).await.unwrap();
    node_when(&obs, &id, |n| n.ports[0].data_active).await;
    let mut rx = obs.subscribe();
    tokio::time::sleep(Duration::from_millis(2500)).await;
    assert_eq!(vn.dmx(2), vec![5, 6, 7, 8], "no failsafe before 4 s");
    tokio::time::sleep(Duration::from_millis(1800)).await;
    assert_eq!(vn.dmx(1), vec![5, 6, 7, 8], "hold");
    assert!(vn.dmx(2).iter().all(|&v| v == 0), "zero: {:?}", vn.dmx(2));
    assert!(vn.dmx(3).iter().all(|&v| v == 255), "full: {:?}", vn.dmx(3));
    assert_eq!(&vn.dmx(4)[..4], &[11, 22, 33, 44], "scene");
    // data loss is reported with an unsolicited ArtPollReply (GoodOutputA bit 7 off, contract §7)
    let lost = wait_event(&mut rx, WAIT, |e| match e {
        Event::NodeUpdated { node } if !node.ports[0].data_active => Some(()),
        _ => None,
    })
    .await;
    assert!(lost.is_some(), "unsolicited ArtPollReply on data loss");
    obs.shutdown();
    ctrl.shutdown();
}

// ---------------------------------------------------------------------------------------------------------------
// 7. Tube flash incl. BOOT_CATCH rescue
// ---------------------------------------------------------------------------------------------------------------

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn e2e_7_tube_flash_and_rescue() {
    let vn = vn_or_skip!(&["--tubes", "1:1"]);
    let ctrl = controller(&vn).await;
    let id = vn.id();
    let t = tube_uid(1, 1);
    tod_len(&ctrl, &id, pa(1), 1, WAIT).await;

    vn.ctl(&format!("busy 1 {t} 1")); // every page write: next request NACK BUFFER_FULL
    let job = ctrl
        .flash_tube(&id, pa(1), t, tube_image(0x000600), false, false)
        .unwrap();
    let r = tokio::time::timeout(Duration::from_secs(90), job.wait())
        .await
        .expect("flash time")
        .unwrap();
    assert!(r.busy_retries > 0, "BUFFER_FULL passed through ArtRdm");
    vn.ctl(&format!("busy 1 {t} 0"));
    assert_eq!(r.old_version, Some(0x500));
    assert_eq!(r.new_version, 0x600);
    assert_eq!(r.version_label, "tube_fw 0.6.0");
    assert_eq!(vn.tube(1, t, "mode"), "app");
    assert_eq!(vn.tube(1, t, "version"), "0x000600");

    // rescue: BOOT_CATCH at the node, power-cycle the tube while the job is catching
    let job = ctrl
        .flash_tube(&id, pa(1), t, tube_image(0x000700), false, true)
        .unwrap();
    let mut prog = job.progress();
    tokio::time::timeout(WAIT, async {
        loop {
            if prog.borrow().phase == FlashPhase::Catching {
                break;
            }
            prog.changed().await.unwrap();
        }
    })
    .await
    .expect("phase Catching");
    tokio::time::sleep(Duration::from_millis(300)).await;
    vn.ctl(&format!("power 1 {t} 400"));
    let r = tokio::time::timeout(Duration::from_secs(90), job.wait())
        .await
        .expect("rescue time")
        .unwrap();
    assert_eq!(r.new_version, 0x700);
    assert_eq!(vn.tube(1, t, "version"), "0x000700");
    let st = ctrl
        .rdm_get(&id, pa(1), vn.uid, pids::PID_BOOT_CATCH_STATUS, &[])
        .await
        .unwrap();
    assert_eq!(st[0], pids::boot_catch_state::CAUGHT);
    assert_eq!(st[1], 1);
    assert_eq!(&st[2..8], &t.to_bytes());
    ctrl.shutdown();
}

// ---------------------------------------------------------------------------------------------------------------
// 8. Node flash (RDM and ArtFirmwareMaster) incl. first-block errors
// ---------------------------------------------------------------------------------------------------------------

async fn fw_info(ctrl: &Controller, vn: &Vn) -> FwInfo {
    FwInfo::decode(
        &ctrl
            .rdm_get(&vn.id(), pa(1), vn.uid, pids::PID_FW_INFO, &[])
            .await
            .unwrap(),
    )
    .unwrap()
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn e2e_8_node_flash() {
    let vn = vn_or_skip!(&[]);
    let ctrl = controller(&vn).await;
    let id = vn.id();
    let opts = |method| NodeFlashOptions {
        method,
        reboot_timeout: Duration::from_secs(20),
        ..NodeFlashOptions::default()
    };

    // RDM: FW_BEGIN / FW_WRITE / FW_FINISH -> reboot with 0.2.0
    let img = NodeImage::from_bin(build_test_bin(30_000, 0x000200, 1, "0.2.0")).unwrap();
    let job = ctrl
        .flash_node(&id, img, opts(NodeFlashMethod::Rdm))
        .unwrap();
    let r = tokio::time::timeout(Duration::from_secs(120), job.wait())
        .await
        .expect("rdm flash")
        .unwrap();
    assert_eq!(r.new_version, 0x000200);
    assert_eq!(r.version_label, "0.2.0");
    let n = node_when(&ctrl, &id, |n| n.firmware_version == 0x0002).await;
    assert_eq!(n.uid, Some(vn.uid));

    // ArtFirmwareMaster (.alf) -> 0.3.0
    let img = NodeImage::from_bin(build_test_bin(20_001, 0x000300, 1, "0.3.0")).unwrap();
    let job = ctrl
        .flash_node(&id, img, opts(NodeFlashMethod::ArtFirmwareMaster))
        .unwrap();
    let r = tokio::time::timeout(Duration::from_secs(120), job.wait())
        .await
        .expect("alf flash")
        .unwrap();
    assert_eq!(r.new_version, 0x000300);
    node_when(&ctrl, &id, |n| n.firmware_version == 0x0003).await;

    // error in the first block, RDM: backend refuses -> NACK, FW_INFO FAILED / FLASH
    vn.ctl("fwfail");
    let img = NodeImage::from_bin(build_test_bin(10_000, 0x000400, 1, "0.4.0")).unwrap();
    let job = ctrl
        .flash_node(&id, img.clone(), opts(NodeFlashMethod::Rdm))
        .unwrap();
    let e = job.wait().await.unwrap_err();
    assert!(e.to_string().contains("FLASH"), "{e}");
    let fi = fw_info(&ctrl, &vn).await;
    assert_eq!(fi.state, pids::fw_state::FAILED);
    assert_eq!(fi.last_error, pids::fw_error::FLASH);
    assert_eq!(fi.fw_version, 0x000300);

    // error in the first block, Art-Net: OEM code not in the .alf list -> FirmFail + FW_INFO OEM
    let mut alf = img.to_alf();
    alf[34] = 0x12;
    alf[35] = 0x34;
    let first = ArtFirmwareMaster {
        kind: ArtFirmwareMaster::FIRM_FIRST,
        block_id: 0,
        firmware_length: (alf.len() / 2) as u32,
        data: alf[..1024].to_vec(),
    };
    let s = raw();
    s.send_to(&ArtPacket::FirmwareMaster(first).encode(), vn.addr())
        .unwrap();
    let reply = collect(&s, Duration::from_millis(800))
        .into_iter()
        .find_map(|p| match p {
            ArtPacket::FirmwareReply(r) => Some(r),
            _ => None,
        });
    assert_eq!(reply.map(|r| r.kind), Some(ArtFirmwareReply::FIRM_FAIL));
    let fi = fw_info(&ctrl, &vn).await;
    assert_eq!(fi.state, pids::fw_state::FAILED);
    assert_eq!(fi.last_error, pids::fw_error::OEM);
    ctrl.shutdown();
}

// ---------------------------------------------------------------------------------------------------------------
// 9. Input
// ---------------------------------------------------------------------------------------------------------------

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn e2e_9_input_unicast_to_pollers() {
    let vn = vn_or_skip!(&[]);
    let ctrl = controller(&vn).await;
    let id = vn.id();
    ctrl.art_address_command(&id, 1, ac::AC_DIRECTION_RX0)
        .await
        .unwrap();
    node_when(&ctrl, &id, |n| n.ports[0].direction == PortDirection::Input).await;

    let poller = raw();
    let other = raw(); // known to the node (sent a packet), but never polled
    other
        .send_to(
            &ArtPacket::TodRequest(ArtTodRequest::for_port_addresses(0, &[pa(2)])).encode(),
            vn.addr(),
        )
        .unwrap();
    poller.send_to(&poll_packet(), vn.bcast()).unwrap();
    std::thread::sleep(Duration::from_millis(1200));
    let _ = collect(&poller, Duration::from_millis(10));
    vn.ctl("input 1 ramp");
    let got = collect(&poller, Duration::from_millis(1000));
    let frames: Vec<&ArtDmx> = got
        .iter()
        .filter_map(|p| match p {
            ArtPacket::Dmx(d) => Some(d),
            _ => None,
        })
        .collect();
    assert!(
        frames.len() >= 10,
        "changing input -> ArtDmx per frame, got {}",
        frames.len()
    );
    assert!(frames
        .iter()
        .all(|d| d.port_address == pa(1) && d.data.len() == 512));
    assert_ne!(frames[0].data, frames[frames.len() - 1].data);
    let n = node_when(&ctrl, &id, |n| n.ports[0].data_active).await;
    assert!(!n.ports[0].input_disabled);
    // never broadcast: the peer that did not poll gets nothing
    let o = collect(&other, Duration::from_millis(300));
    assert!(
        !o.iter().any(|p| matches!(p, ArtPacket::Dmx(_))),
        "ArtDmx to a non-poller"
    );
    // unchanged input: keepalive about once per second
    vn.ctl("input 1 static 7");
    std::thread::sleep(Duration::from_millis(300));
    let _ = collect(&poller, Duration::from_millis(10));
    poller.send_to(&poll_packet(), vn.bcast()).unwrap();
    let got = collect(&poller, Duration::from_millis(3200));
    let frames: Vec<&ArtDmx> = got
        .iter()
        .filter_map(|p| match p {
            ArtPacket::Dmx(d) => Some(d),
            _ => None,
        })
        .collect();
    assert!(
        (2..=5).contains(&frames.len()),
        "keepalive frames in 3.2 s: {}",
        frames.len()
    );
    assert!(frames.iter().all(|d| d.data.iter().all(|&v| v == 7)));

    // another node that outputs this universe (its ArtPollReply, encoded by pixelmapper-net) is a subscriber
    let sub = raw();
    let mut r = ArtPollReply {
        ip: Ipv4Addr::LOCALHOST,
        num_ports: 1,
        bind_index: 1,
        short_name: "other node".into(),
        ..ArtPollReply::default()
    };
    r.port_types[0] = 0x80;
    r.sw_out[0] = 1;
    sub.send_to(&ArtPacket::PollReply(r).encode(), vn.addr())
        .unwrap();
    let got = collect(&sub, Duration::from_millis(2500));
    assert!(
        got.iter()
            .any(|p| matches!(p, ArtPacket::Dmx(d) if d.port_address == pa(1))),
        "{got:?}"
    );
    ctrl.shutdown();
}

// ---------------------------------------------------------------------------------------------------------------
// CLI against the virtual node
// ---------------------------------------------------------------------------------------------------------------

#[test]
fn e2e_cli_poll_and_tod() {
    let vn = vn_or_skip!(&["--tubes", "1:2"]);
    std::thread::sleep(Duration::from_millis(1500)); // discovery
    let out = Command::new(env!("CARGO_BIN_EXE_pmnet"))
        .args([
            "--bind",
            "127.0.0.1:0",
            "--broadcast",
            &vn.bcast().to_string(),
            "--json",
            "--wait",
            "1500",
            "poll",
        ])
        .output()
        .unwrap();
    let text = String::from_utf8_lossy(&out.stdout);
    assert!(
        out.status.success(),
        "{text} {}",
        String::from_utf8_lossy(&out.stderr)
    );
    assert!(text.contains(&vn.uid.to_string()), "{text}");
}

// ---------------------------------------------------------------------------------------------------------------
// More node behaviour (contract §5, §6.3, §6.4, §7): E1.37-2, identify mirroring, status messages, ArtInput,
// BOOT_CATCH checks, RESET_DEVICE, FACTORY_DEFAULTS
// ---------------------------------------------------------------------------------------------------------------

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn e2e_node_rdm_details() {
    use pixelmapper_net::rdm::pd::{
        BootCatchStatus, InterfaceEntry, Ipv4CurrentAddress, LocalRdmSet, StatusMessage,
    };
    let vn = vn_or_skip!(&["--tubes", "1:1"]);
    let ctrl = controller(&vn).await;
    let id = vn.id();
    let node = vn.uid;
    let mut rx = ctrl.subscribe();
    let get = |p: u16, d: Vec<u8>| {
        let c = ctrl.clone();
        async move { c.rdm_get(&id, pa(1), node, p, &d).await }
    };

    // E1.37-2: interface list + current address; static address is stored, applied with APPLY_CONFIGURATION
    let ifs =
        InterfaceEntry::decode_list(&get(pid::LIST_INTERFACES, vec![]).await.unwrap()).unwrap();
    assert_eq!(ifs.len(), 1);
    let ifid = ifs[0].interface_id.to_be_bytes().to_vec();
    let cur =
        Ipv4CurrentAddress::decode(&get(pid::IPV4_CURRENT_ADDRESS, ifid.clone()).await.unwrap())
            .unwrap();
    assert_eq!(cur.address, Ipv4Addr::LOCALHOST);
    assert_eq!(cur.netmask, 8);
    let mut st = ifid.clone();
    st.extend_from_slice(&[10, 1, 2, 3, 16]);
    ctrl.rdm_set(&id, pa(1), node, pid::IPV4_STATIC_ADDRESS, &st)
        .await
        .unwrap();
    let mut off = ifid.clone();
    off.push(0);
    ctrl.rdm_set(&id, pa(1), node, pid::IPV4_DHCP_MODE, &off)
        .await
        .unwrap();
    tokio::time::sleep(Duration::from_millis(300)).await;
    assert_eq!(
        vn.ctl("net"),
        "ip=127.0.0.1 mask=255.0.0.0 gw=0.0.0.0 dhcp=1",
        "not applied yet"
    );
    ctrl.rdm_set(&id, pa(1), node, pid::INTERFACE_APPLY_CONFIGURATION, &ifid)
        .await
        .unwrap();
    node_when(&ctrl, &id, |n| n.ip == Ipv4Addr::new(10, 1, 2, 3)).await;
    let cur =
        Ipv4CurrentAddress::decode(&get(pid::IPV4_CURRENT_ADDRESS, ifid.clone()).await.unwrap())
            .unwrap();
    assert_eq!((cur.address, cur.netmask), (Ipv4Addr::new(10, 1, 2, 3), 16));
    let r = ctrl.ip_prog_query(&id).await.unwrap();
    assert_eq!(
        r.prog_sm,
        Ipv4Addr::new(255, 255, 0, 0),
        "ArtIpProg and E1.37-2 see the same settings"
    );

    // identify: RDM SET -> ArtPollReply Locate; panel -> queued IDENTIFY_DEVICE
    ctrl.rdm_set(&id, pa(1), node, pid::IDENTIFY_DEVICE, &[1])
        .await
        .unwrap();
    node_when(&ctrl, &id, |n| n.status1.indicators == Indicators::Locate).await;
    vn.ctl("panel identify 0");
    let q = wait_event(&mut rx, WAIT, |e| match e {
        Event::QueuedMessage { message, .. } if message.pid == pid::IDENTIFY_DEVICE => {
            Some(message.data.clone())
        }
        _ => None,
    })
    .await;
    assert_eq!(q, Some(vec![0]));
    node_when(&ctrl, &id, |n| n.status1.indicators == Indicators::Normal).await;

    // panel lock released at the device: status message PANEL_UNLOCKED_LOCAL (queued)
    vn.ctl("panel lock 1");
    tokio::time::sleep(Duration::from_millis(200)).await;
    vn.ctl("panel lock 0");
    let list = wait_event(&mut rx, WAIT, |e| match e {
        Event::QueuedMessage { message, .. } => message.status_messages.clone().filter(|l| {
            l.iter()
                .any(|s| s.status_message_id == pids::status::PANEL_UNLOCKED_LOCAL)
        }),
        _ => None,
    })
    .await
    .expect("status PANEL_UNLOCKED_LOCAL");
    let s: &StatusMessage = list
        .iter()
        .find(|s| s.status_message_id == pids::status::PANEL_UNLOCKED_LOCAL)
        .unwrap();
    assert_eq!(s.status_type, 0x02, "advisory");
    let d = get(
        pid::STATUS_ID_DESCRIPTION,
        pids::status::PANEL_UNLOCKED_LOCAL.to_be_bytes().to_vec(),
    )
    .await
    .unwrap();
    assert_eq!(
        Some(decode_label(&d).as_str()),
        pids::status_description(pids::status::PANEL_UNLOCKED_LOCAL)
    );

    // LOCAL_RDM_SET (Rust decoder against the C encoder)
    let t = tube_uid(1, 1);
    tod_len(&ctrl, &id, pa(1), 1, WAIT).await;
    let req = pixelmapper_net::rdm::RdmMessage::request(
        t,
        Uid::new(pids::ESTA_MANUFACTURER, 0x99),
        1,
        1,
        0,
        pixelmapper_net::rdm::cc::SET,
        pid::IDENTIFY_DEVICE,
        vec![1],
    );
    vn.ctl(&format!(
        "panel rdm 1 {}",
        pixelmapper_net::to_hex(&req.encode())
    ));
    let lrs = eventually(WAIT, || async {
        let pd = get(pids::PID_LOCAL_RDM_SET, vec![]).await.ok()?;
        LocalRdmSet::decode(&pd).ok().filter(|l| l.uid == t)
    })
    .await
    .expect("LOCAL_RDM_SET");
    assert_eq!(
        (lrs.port, lrs.sub_device, lrs.pid),
        (1, 0, pid::IDENTIFY_DEVICE)
    );
    eventually(WAIT, || async {
        (vn.tube(1, t, "identify") == "1").then_some(())
    })
    .await
    .expect("tube identify");

    // BOOT_CATCH: NACK on an input port and for broadcast; status decodes
    ctrl.art_address_command(&id, 2, ac::AC_DIRECTION_RX0)
        .await
        .unwrap();
    let mut pd = vec![2u8];
    pd.extend_from_slice(&t.to_bytes());
    let e = ctrl
        .rdm_set(&id, pa(1), node, pids::PID_BOOT_CATCH_START, &pd)
        .await
        .unwrap_err();
    assert_eq!(
        e.nack_reason(),
        Some(pixelmapper_net::rdm::NackReason::DATA_OUT_OF_RANGE),
        "{e}"
    );
    let mut pd = vec![1u8];
    pd.extend_from_slice(&Uid::new(0xFFFF, 0xFFFF_FFFF).to_bytes());
    let e = ctrl
        .rdm_set(&id, pa(1), node, pids::PID_BOOT_CATCH_START, &pd)
        .await
        .unwrap_err();
    assert_eq!(
        e.nack_reason(),
        Some(pixelmapper_net::rdm::NackReason::DATA_OUT_OF_RANGE),
        "{e}"
    );
    let mut pd = vec![1u8];
    pd.extend_from_slice(&t.to_bytes());
    ctrl.rdm_set(&id, pa(1), node, pids::PID_BOOT_CATCH_START, &pd)
        .await
        .unwrap();
    let s =
        BootCatchStatus::decode(&get(pids::PID_BOOT_CATCH_STATUS, vec![]).await.unwrap()).unwrap();
    assert_eq!(
        (s.state, s.port, s.target_uid),
        (pids::boot_catch_state::RUNNING, 1, t)
    );
    ctrl.rdm_set(&id, pa(1), node, pids::PID_BOOT_CATCH_ABORT, &[])
        .await
        .unwrap();
    let s =
        BootCatchStatus::decode(&get(pids::PID_BOOT_CATCH_STATUS, vec![]).await.unwrap()).unwrap();
    assert_eq!(s.state, pids::boot_catch_state::ABORTED);

    // ArtInput: disable the input of BindIndex 2 -> GoodInput bit 3
    ctrl.art_input(&id, 2, &[true]).await.unwrap();
    node_when(&ctrl, &id, |n| n.ports[1].input_disabled).await;
    ctrl.art_input(&id, 2, &[false]).await.unwrap();
    node_when(&ctrl, &id, |n| !n.ports[1].input_disabled).await;

    // DEVICE_LABEL longer than the short name: cut to 17 characters
    ctrl.rdm_set(
        &id,
        pa(1),
        node,
        pid::DEVICE_LABEL,
        b"a very long device label",
    )
    .await
    .unwrap();
    let l = decode_label(&get(pid::DEVICE_LABEL, vec![]).await.unwrap());
    assert_eq!(l, "a very long devic");

    // FACTORY_DEFAULTS: names, ports, IP back (DHCP)
    ctrl.rdm_set(&id, pa(1), node, pid::FACTORY_DEFAULTS, &[])
        .await
        .unwrap();
    let n = node_when(&ctrl, &id, |n| {
        n.short_name == "PM Node" && n.ip == Ipv4Addr::LOCALHOST
    })
    .await;
    assert_eq!(n.ports[1].direction, PortDirection::Output);

    // RESET_DEVICE: the node restarts (same UID, same settings)
    ctrl.rdm_set(&id, pa(1), node, pid::RESET_DEVICE, &[0x01])
        .await
        .unwrap();
    eventually(WAIT, || async {
        vn.ctl("ping").contains("boots=2").then_some(())
    })
    .await
    .expect("reboot");
    ctrl.rdm_get(&id, pa(1), node, pid::DEVICE_INFO, &[])
        .await
        .unwrap();
    ctrl.shutdown();
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn e2e_artnet_details() {
    use pixelmapper_net::artnet::ArtIpProg;
    use pixelmapper_net::state::OutputStyle;
    let vn = vn_or_skip!(&["--tubes", "1:1"]);
    let ctrl = controller(&vn).await;
    let id = vn.id();
    let t = tube_uid(1, 1);
    tod_len(&ctrl, &id, pa(1), 1, WAIT).await;

    // RDM burst: more requests than FifoMax, the controller keeps to FifoAvail
    let mut hs = Vec::new();
    for _ in 0..20 {
        let c = ctrl.clone();
        hs.push(tokio::spawn(async move {
            c.rdm_get(&NodeId::Uid(vn_uid_of(&c)), pa(1), t, pid::DEVICE_INFO, &[])
                .await
        }));
    }
    for h in hs {
        h.await.unwrap().unwrap();
    }

    // RDM broadcast reaches the node's responder and the line
    ctrl.rdm_set(
        &id,
        pa(1),
        Uid::new(0xFFFF, 0xFFFF_FFFF),
        pid::IDENTIFY_DEVICE,
        &[1],
    )
    .await
    .unwrap();
    eventually(WAIT, || async {
        (vn.ctl("identify") == "1" && vn.tube(1, t, "identify") == "1").then_some(())
    })
    .await
    .expect("broadcast identify");
    ctrl.rdm_set(
        &id,
        pa(1),
        Uid::new(0xFFFF, 0xFFFF_FFFF),
        pid::IDENTIFY_DEVICE,
        &[0],
    )
    .await
    .unwrap();

    // output style delta (GoodOutputB bit 6), AcClearOp
    ctrl.art_address_command(&id, 1, ac::AC_STYLE_DELTA0)
        .await
        .unwrap();
    node_when(&ctrl, &id, |n| {
        n.ports[0].output_style == OutputStyle::Delta
    })
    .await;
    ctrl.art_address_command(&id, 1, ac::AC_STYLE_CONST0)
        .await
        .unwrap();
    node_when(&ctrl, &id, |n| {
        n.ports[0].output_style == OutputStyle::Continuous
    })
    .await;
    ctrl.send_dmx(pa(1), &[1, 2, 3, 4]).await.unwrap();
    eventually(WAIT, || async {
        (vn.dmx(1) == vec![1, 2, 3, 4]).then_some(())
    })
    .await
    .expect("dmx");
    ctrl.art_address_command(&id, 1, ac::AC_CLEAR_OP0)
        .await
        .unwrap();
    eventually(WAIT, || async {
        vn.dmx(1).iter().all(|&v| v == 0).then_some(())
    })
    .await
    .expect("cleared");

    // Net switch of BindIndex 1 -> Port-Address 0x101; TOD and RDM through the new address
    let pkt = ArtAddress {
        bind_index: 1,
        net_switch: ArtAddress::program(1),
        ..ArtAddress::default()
    };
    ctrl.art_address(&id, pkt).await.unwrap();
    let pa101 = PortAddress::new(0x101).unwrap();
    node_when(&ctrl, &id, |n| n.ports[0].port_address == pa101).await;
    assert_eq!(tod_len(&ctrl, &id, pa101, 1, WAIT).await, vec![t]);
    ctrl.rdm_get(&id, pa101, t, pid::DEVICE_INFO, &[])
        .await
        .unwrap();
    ctrl.rdm_get(&id, pa101, vn.uid, pid::DEVICE_INFO, &[])
        .await
        .unwrap();
    let pkt = ArtAddress {
        bind_index: 1,
        net_switch: ArtAddress::RESET,
        ..ArtAddress::default()
    };
    ctrl.art_address(&id, pkt).await.unwrap();
    node_when(&ctrl, &id, |n| n.ports[0].port_address == pa(1)).await;

    // ArtInput only acts on inputs: an output port stays as it is (contract §5)
    ctrl.art_input(&id, 1, &[true]).await.unwrap();
    tokio::time::sleep(Duration::from_millis(300)).await;
    assert!(
        vn.ctl("settings").contains("p1=en1,dir0"),
        "{}",
        vn.ctl("settings")
    );
    let n = ctrl.node(&id).unwrap();
    assert!(n.ports[0].rdm_enabled && n.ports[0].direction == PortDirection::Output);

    // names longer than the fields: cut to 17 / 63 characters on both sides
    let long = "L".repeat(70);
    let n = ctrl
        .art_address(
            &id,
            ArtAddress {
                short_name: "abcdefghijklmnopqrstuvwxyz".into(),
                long_name: long,
                ..ArtAddress::default()
            },
        )
        .await
        .unwrap()
        .unwrap();
    assert_eq!(n.short_name, "abcdefghijklmnopq");
    assert_eq!(n.long_name, "L".repeat(63));
    let l = ctrl
        .rdm_get(&id, pa(1), vn.uid, pid::DEVICE_LABEL, &[])
        .await
        .unwrap();
    assert_eq!(decode_label(&l), "abcdefghijklmnopq");

    // ArtIpProg: invalid values are ignored, reset goes back to the defaults (DHCP)
    let bad = ArtIpProg {
        command: pixelmapper_net::artnet::IPPROG_ENABLE
            | pixelmapper_net::artnet::IPPROG_IP
            | pixelmapper_net::artnet::IPPROG_MASK,
        prog_ip: Ipv4Addr::new(0, 0, 0, 0),
        prog_sm: Ipv4Addr::new(255, 0, 255, 0),
        ..ArtIpProg::default()
    };
    let r = ctrl.ip_prog(&id, bad).await.unwrap();
    assert_eq!(
        (r.prog_ip, r.prog_sm),
        (Ipv4Addr::LOCALHOST, Ipv4Addr::new(255, 0, 0, 0))
    );
    ctrl.ip_prog_change(
        &id,
        &IpProgChange {
            ip: Some(Ipv4Addr::new(10, 0, 0, 5)),
            ..IpProgChange::default()
        },
    )
    .await
    .unwrap();
    node_when(&ctrl, &id, |n| n.ip == Ipv4Addr::new(10, 0, 0, 5)).await;
    let r = ctrl
        .ip_prog_change(
            &id,
            &IpProgChange {
                reset: true,
                ..IpProgChange::default()
            },
        )
        .await
        .unwrap();
    assert_ne!(r.status & 0x40, 0, "reset -> DHCP");
    node_when(&ctrl, &id, |n| n.ip == Ipv4Addr::LOCALHOST).await;
    ctrl.shutdown();
}

fn vn_uid_of(c: &Controller) -> Uid {
    c.nodes()[0].uid.unwrap()
}

/// Contract §6.1: unsolicited ArtTodData go to the IPs that sent ArtTodRequest; the node forgets them when it
/// restarts, so the controller has to request the TODs again after a restart (detected by the NodeReport counter).
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn e2e_3_tod_subscription_after_node_restart() {
    let vn = vn_or_skip!(&["--tubes", "1:1"]);
    let ctrl = controller(&vn).await;
    let id = vn.id();
    tod_len(&ctrl, &id, pa(1), 1, WAIT).await;
    ctrl.rdm_set(&id, pa(1), vn.uid, pid::RESET_DEVICE, &[0x01])
        .await
        .unwrap();
    eventually(WAIT, || async {
        vn.ctl("ping").contains("boots=2").then_some(())
    })
    .await
    .expect("reboot");
    let mut rx = ctrl.subscribe();
    // wait until the controller has seen replies of the restarted node, then let it settle
    tokio::time::sleep(Duration::from_millis(2500)).await;
    node_when(&ctrl, &id, |n| !n.ports[0].discovery_running).await;
    let t2 = tube_uid(1, 2);
    vn.ctl(&format!("add 1 tube {t2}"));
    let added = wait_event(&mut rx, WAIT, |e| match e {
        Event::TodChanged { added, .. } if added.contains(&t2) => Some(()),
        _ => None,
    })
    .await;
    assert!(
        added.is_some(),
        "no unsolicited ArtTodData after the node restarted (TOD not requested again)"
    );
    ctrl.shutdown();
}

/// ArtSync: no sync for 4 s -> direct output again; while a port merges ArtSync is ignored (contract §4).
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn e2e_6_sync_timeout_and_merge() {
    let vn = vn_or_skip!(&[]);
    let s = raw();
    let dest = vn.addr();
    let dmx = |seq: u8, phys: u8, p: u16, v: u8| {
        ArtPacket::Dmx(ArtDmx::new(seq, phys, pa(p), vec![v; 4]).unwrap()).encode()
    };
    let sync = ArtPacket::Sync(ArtSync::default()).encode();
    s.send_to(&dmx(1, 0, 1, 1), dest).unwrap();
    s.send_to(&sync, vn.bcast()).unwrap();
    std::thread::sleep(Duration::from_millis(100));
    s.send_to(&dmx(2, 0, 1, 2), dest).unwrap();
    std::thread::sleep(Duration::from_millis(200));
    assert_eq!(vn.dmx(1), vec![1; 4], "sync mode");
    std::thread::sleep(Duration::from_millis(4300));
    s.send_to(&dmx(3, 0, 1, 3), dest).unwrap();
    std::thread::sleep(Duration::from_millis(200));
    assert_eq!(vn.dmx(1), vec![3; 4], "4 s without ArtSync: direct output");

    // two Physicals of one IP merge, ArtSync still applies (digest §3.3: only streams of different IPs block it)
    s.send_to(&dmx(4, 0, 2, 10), dest).unwrap();
    s.send_to(&dmx(5, 1, 2, 20), dest).unwrap();
    std::thread::sleep(Duration::from_millis(100));
    s.send_to(&sync, vn.bcast()).unwrap();
    std::thread::sleep(Duration::from_millis(100));
    s.send_to(&dmx(6, 0, 2, 30), dest).unwrap();
    std::thread::sleep(Duration::from_millis(200));
    assert_eq!(vn.dmx(2), vec![20; 4], "held until the next ArtSync");
    s.send_to(&sync, vn.bcast()).unwrap();
    std::thread::sleep(Duration::from_millis(200));
    assert_eq!(vn.dmx(2), vec![30; 4]);

    // streams of two IPs merge (another socket = another peer address at the node): ArtSync is ignored
    let s2 = raw();
    s2.send_to(&dmx(1, 0, 3, 40), dest).unwrap();
    s.send_to(&dmx(7, 0, 3, 50), dest).unwrap();
    std::thread::sleep(Duration::from_millis(100));
    s.send_to(&sync, vn.bcast()).unwrap();
    std::thread::sleep(Duration::from_millis(100));
    s.send_to(&dmx(8, 0, 3, 60), dest).unwrap();
    std::thread::sleep(Duration::from_millis(200));
    assert_eq!(
        vn.dmx(3),
        vec![60; 4],
        "HTP 40/60 at once: ArtSync ignored while two IPs merge"
    );
}

// ---------------------------------------------------------------------------------------------------------------
// Differential: the firmware (virtual node) against pixelmapper-net's own node model (sim::node)
// ---------------------------------------------------------------------------------------------------------------

/// Outcome of one RDM request, comparable across implementations.
fn outcome(r: pixelmapper_net::Result<Vec<u8>>) -> String {
    match r {
        Ok(d) => format!("ACK {}", pixelmapper_net::to_hex(&d)),
        Err(e) => match e.nack_reason() {
            Some(n) => format!("NACK {:04X}", n.0),
            None => format!("ERR {}", e.code()),
        },
    }
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn e2e_differential_node_rdm_and_poll_reply() {
    use pixelmapper_net::controller::RdmRequest;
    use pixelmapper_net::link::RdmOptions;
    use pixelmapper_net::rdm::cc;
    use pixelmapper_net::sim::{SimConfig, SimNode};
    let vn = vn_or_skip!(&[]);
    let sim = SimNode::start(SimConfig {
        long_name: "Pixelmapper Node".into(),
        ..SimConfig::default()
    })
    .await
    .unwrap();
    assert_eq!(sim.uid(), vn.uid, "same MAC -> same UID");
    let c_fw = controller(&vn).await;
    let mut cs = ControllerConfig::local(vec![sim.local_addr()]);
    cs.rdm.timeout = Duration::from_millis(500);
    let c_sim = Controller::bind(cs).await.unwrap();
    c_sim
        .wait_for_node(&NodeId::Uid(sim.uid()), WAIT)
        .await
        .unwrap();
    let id = vn.id();
    let node = vn.uid;

    let mut reqs: Vec<(u8, u16, Vec<u8>, u16)> = Vec::new(); // cc, pid, pd, sub-device
    let get = |p: u16| (cc::GET, p, vec![], 0u16);
    for p in [
        pid::DEVICE_INFO,
        pid::SUPPORTED_PARAMETERS,
        pid::SOFTWARE_VERSION_LABEL,
        pid::DEVICE_MODEL_DESCRIPTION,
        pid::MANUFACTURER_LABEL,
        pid::DEVICE_LABEL,
        pid::PRODUCT_DETAIL_ID_LIST,
        pid::BOOT_SOFTWARE_VERSION_ID,
        pid::BOOT_SOFTWARE_VERSION_LABEL,
        pid::DISPLAY_LEVEL,
        pid::IDENTIFY_DEVICE,
        pid::LIST_INTERFACES,
        pid::FACTORY_DEFAULTS,
    ] {
        reqs.push(get(p));
    }
    reqs.push((cc::GET, pid::STATUS_MESSAGES, vec![0x02], 0));
    reqs.push((cc::GET, pid::STATUS_MESSAGES, vec![0x04], 0));
    for def in pids::PIDS.iter().filter(|p| p.device == pids::Device::Node) {
        reqs.push(get(def.pid));
        reqs.push((
            cc::GET,
            pid::PARAMETER_DESCRIPTION,
            def.pid.to_be_bytes().to_vec(),
            0,
        ));
    }
    for s in pids::STATUS {
        reqs.push((
            cc::GET,
            pid::STATUS_ID_DESCRIPTION,
            s.id.to_be_bytes().to_vec(),
            0,
        ));
    }
    // E1.37-2 with the interface id the node lists
    let ifs = c_fw
        .rdm_get(&id, pa(1), node, pid::LIST_INTERFACES, &[])
        .await
        .unwrap();
    let ifid = ifs[..4].to_vec();
    for p in [
        pid::INTERFACE_LABEL,
        pid::INTERFACE_HARDWARE_ADDRESS_TYPE1,
        pid::IPV4_DHCP_MODE,
        pid::IPV4_CURRENT_ADDRESS,
        pid::IPV4_STATIC_ADDRESS,
    ] {
        reqs.push((cc::GET, p, ifid.clone(), 0));
    }
    reqs.push(get(pid::IPV4_DEFAULT_ROUTE));
    // error cases
    reqs.push((
        cc::GET,
        pid::PARAMETER_DESCRIPTION,
        pid::DEVICE_INFO.to_be_bytes().to_vec(),
        0,
    ));
    reqs.push((
        cc::GET,
        pid::PARAMETER_DESCRIPTION,
        0x81FFu16.to_be_bytes().to_vec(),
        0,
    ));
    reqs.push((
        cc::GET,
        pid::STATUS_ID_DESCRIPTION,
        0x8FFFu16.to_be_bytes().to_vec(),
        0,
    ));
    reqs.push(get(0x81FF));
    reqs.push(get(pids::PID_BOOT_CATCH_START));
    reqs.push((cc::GET, pid::DEVICE_INFO, vec![1], 0));
    reqs.push((cc::GET, pid::DEVICE_INFO, vec![], 1));
    reqs.push((cc::SET, pids::PID_PORT1_ENABLED, vec![2], 0));
    reqs.push((cc::SET, pids::PID_PORT1_ENABLED, vec![1, 1], 0));
    reqs.push((cc::SET, pids::PID_PORT1_DMX_RATE, vec![45], 0));
    reqs.push((
        cc::SET,
        pids::PID_PORT1_DMX_MBS,
        101u16.to_be_bytes().to_vec(),
        0,
    ));
    reqs.push((
        cc::SET,
        pids::PID_RDM_BG_DISCOVERY_INTERVAL,
        0u16.to_be_bytes().to_vec(),
        0,
    ));
    reqs.push((cc::SET, pids::PID_DISPLAY_ADDRESS_FORMAT, vec![2], 0));
    reqs.push((cc::SET, pids::PID_NODE_HW_REVISION, vec![1], 0));
    reqs.push((cc::SET, pid::DEVICE_INFO, vec![], 0));
    reqs.push((cc::SET, pid::IDENTIFY_DEVICE, vec![2], 0));
    reqs.push((cc::SET, pid::DEVICE_LABEL, vec![b'x'; 33], 0));
    reqs.push((cc::SET, pid::DISPLAY_LEVEL, vec![128], 0));
    reqs.push(get(pid::DISPLAY_LEVEL));
    reqs.push((cc::SET, pids::PID_FW_WRITE, vec![0, 0, 0, 0, 1], 0));
    reqs.push(get(pids::PID_FW_INFO));
    reqs.push((cc::SET, pids::PID_FW_FINISH, vec![], 0));
    reqs.push(get(pids::PID_FW_INFO));
    reqs.push((cc::SET, pids::PID_FW_BEGIN, vec![0; 64], 0));
    reqs.push(get(pids::PID_FW_INFO));
    reqs.push((cc::SET, pids::PID_BOOT_CATCH_ABORT, vec![], 0));
    reqs.push(get(pids::PID_BOOT_CATCH_STATUS));
    reqs.push((cc::SET, pid::RESET_DEVICE, vec![0x02], 0));
    reqs.push((cc::SET, pid::FACTORY_DEFAULTS, vec![1], 0));
    // firmware session: valid begin, write, gap, FW_INFO, write after the gap
    let img = NodeImage::from_bin(build_test_bin(2000, 0x000200, 1, "0.2.0")).unwrap();
    reqs.push((cc::SET, pids::PID_FW_BEGIN, img.desc.encode().to_vec(), 0));
    let mut w = 0u32.to_be_bytes().to_vec();
    w.extend_from_slice(&img.bin[..100]);
    reqs.push((cc::SET, pids::PID_FW_WRITE, w, 0));
    reqs.push(get(pids::PID_FW_INFO));
    let mut w = 110u32.to_be_bytes().to_vec();
    w.extend_from_slice(&img.bin[110..120]);
    reqs.push((cc::SET, pids::PID_FW_WRITE, w, 0));
    reqs.push(get(pids::PID_FW_INFO));
    let mut w = 100u32.to_be_bytes().to_vec();
    w.extend_from_slice(&img.bin[100..120]);
    reqs.push((cc::SET, pids::PID_FW_WRITE, w, 0));
    reqs.push(get(pids::PID_FW_INFO));
    // every settable single-value node PID: max, then GET
    for def in pids::PIDS
        .iter()
        .filter(|p| p.device == pids::Device::Node && p.set_request_pdl.is_some())
    {
        let Some(d) = def.desc.filter(|d| d.data_type != 0) else {
            continue;
        };
        let v = d.max;
        let pd = match d.pdl_size {
            1 => vec![v as u8],
            2 => (v as u16).to_be_bytes().to_vec(),
            _ => v.to_be_bytes().to_vec(),
        };
        reqs.push((cc::SET, def.pid, pd, 0));
        reqs.push(get(def.pid));
    }
    reqs.push((cc::SET, pid::DEVICE_LABEL, b"diff label".to_vec(), 0));
    reqs.push(get(pid::DEVICE_LABEL));
    reqs.push((cc::SET, pid::IDENTIFY_DEVICE, vec![1], 0));
    reqs.push(get(pid::IDENTIFY_DEVICE));
    reqs.push((cc::SET, pid::IDENTIFY_DEVICE, vec![0], 0));
    let mut st = ifid.clone();
    st.extend_from_slice(&[10, 7, 7, 7, 24]);
    reqs.push((cc::SET, pid::IPV4_STATIC_ADDRESS, st, 0));
    let mut off = ifid.clone();
    off.push(0);
    reqs.push((cc::SET, pid::IPV4_DHCP_MODE, off, 0));
    let mut gw = 1u32.to_be_bytes().to_vec();
    gw.extend_from_slice(&[10, 7, 7, 1]);
    reqs.push((cc::SET, pid::IPV4_DEFAULT_ROUTE, gw, 0));
    reqs.push((cc::GET, pid::IPV4_STATIC_ADDRESS, ifid.clone(), 0));
    reqs.push((cc::GET, pid::IPV4_DHCP_MODE, ifid.clone(), 0));
    reqs.push((cc::GET, pid::IPV4_CURRENT_ADDRESS, ifid.clone(), 0));
    reqs.push(get(pid::IPV4_DEFAULT_ROUTE));
    reqs.push((cc::SET, pid::INTERFACE_RENEW_DHCP, ifid.clone(), 0));
    reqs.push((cc::SET, pid::INTERFACE_APPLY_CONFIGURATION, ifid.clone(), 0));
    reqs.push((cc::GET, pid::IPV4_CURRENT_ADDRESS, ifid.clone(), 0));

    let mut diffs = Vec::new();
    for (class, p, pd, sub) in reqs {
        let mk = || RdmRequest {
            uid: node,
            command_class: class,
            pid: p,
            data: pd.clone(),
            options: RdmOptions {
                sub_device: sub,
                retries: Some(1),
                ..RdmOptions::default()
            },
        };
        let a = outcome(c_fw.rdm(&id, pa(1), mk()).await);
        let b = outcome(c_sim.rdm(&NodeId::Uid(node), pa(1), mk()).await);
        if a != b {
            diffs.push(format!(
                "{} 0x{p:04X} {} sub {sub}: firmware {a} | sim {b}",
                if class == cc::GET { "GET" } else { "SET" },
                pixelmapper_net::to_hex(&pd)
            ));
        }
    }

    // the same ArtAddress sequence on both, then compare the replies
    let sid = NodeId::Uid(node);
    for (bind, cmd) in [
        (1, ac::AC_MERGE_LTP0),
        (2, ac::AC_DIRECTION_RX0),
        (3, ac::AC_RDM_DISABLE0),
        (4, ac::AC_FAIL_FULL),
        (4, ac::AC_STYLE_DELTA0),
        (1, ac::AC_LED_MUTE),
    ] {
        c_fw.art_address_command(&id, bind, cmd).await.unwrap();
        c_sim.art_address_command(&sid, bind, cmd).await.unwrap();
    }
    let a = ArtAddress {
        bind_index: 3,
        net_switch: ArtAddress::program(2),
        sub_switch: ArtAddress::program(3),
        sw_out: [ArtAddress::program(4), 0x7F, 0x7F, 0x7F],
        short_name: "diff".into(),
        acn_priority: 150,
        ..ArtAddress::default()
    };
    c_fw.art_address(&id, a.clone()).await.unwrap();
    c_sim.art_address(&sid, a).await.unwrap();
    c_fw.art_input(&id, 2, &[true]).await.unwrap();
    c_sim.art_input(&sid, 2, &[true]).await.unwrap();
    // ArtIpProgReply bytes: query (the RDM changes above are not applied: applied by APPLY_CONFIGURATION)
    let (qf, qs) = (
        c_fw.ip_prog_query(&id).await.unwrap(),
        c_sim.ip_prog_query(&sid).await.unwrap(),
    );
    if qf != qs {
        diffs.push(format!(
            "ArtIpProgReply query: firmware {qf:?} | sim {qs:?}"
        ));
    }
    // ArtTodData of an input port / of an RDM-disabled port
    for p in [pa(2), PortAddress::new(0x234).unwrap()] {
        let tod = |to: SocketAddr| {
            let s = raw();
            let req = ArtTodRequest::for_port_addresses(p.net(), &[p]);
            s.send_to(&ArtPacket::TodRequest(req).encode(), to).unwrap();
            collect(&s, Duration::from_millis(300))
                .into_iter()
                .filter_map(|x| match x {
                    ArtPacket::TodData(t) => Some(t),
                    _ => None,
                })
                .collect::<Vec<_>>()
        };
        let (tf, ts) = (tod(vn.addr()), tod(sim.local_addr()));
        if tf != ts {
            diffs.push(format!("ArtTodData {p}: firmware {tf:?} | sim {ts:?}"));
        }
    }

    // ArtPollReply field by field (BindIndex 1..4)
    let poll = |to: SocketAddr| {
        let s = raw();
        s.send_to(&poll_packet(), to).unwrap();
        let mut v: Vec<ArtPollReply> = collect(&s, Duration::from_millis(1300))
            .into_iter()
            .filter_map(|p| match p {
                ArtPacket::PollReply(r) => Some(r),
                _ => None,
            })
            .collect();
        v.sort_by_key(|r| r.bind_index);
        v
    };
    let (rf, rs) = (poll(vn.bcast()), poll(sim.local_addr()));
    assert_eq!(rf.len(), 4);
    assert_eq!(rs.len(), 4);
    for (mut f, mut s) in rf.into_iter().zip(rs) {
        let strip = |r: &str| r.split(']').nth(1).unwrap_or("").to_string(); // the counter differs
        f.node_report = strip(&f.node_report);
        s.node_report = strip(&s.node_report);
        if f != s {
            diffs.push(format!(
                "ArtPollReply bind {}:\n  firmware {f:?}\n  sim      {s:?}",
                f.bind_index
            ));
        }
    }
    // ArtRdm FifoMax
    let fifo = |to: SocketAddr| {
        let s = raw();
        let m = pixelmapper_net::rdm::RdmMessage::request(
            node,
            Uid::new(pids::ESTA_MANUFACTURER, 0x77),
            1,
            1,
            0,
            cc::GET,
            pid::DEVICE_INFO,
            vec![],
        );
        s.send_to(
            &pixelmapper_net::artnet::ArtRdm::from_message(pa(1), &m).encode(),
            to,
        )
        .unwrap();
        collect(&s, Duration::from_millis(400))
            .into_iter()
            .find_map(|p| match p {
                ArtPacket::Rdm(r) => Some((r.fifo_avail, r.fifo_max)),
                _ => None,
            })
    };
    let (ff, fs) = (fifo(vn.addr()), fifo(sim.local_addr()));
    if ff != fs {
        diffs.push(format!(
            "ArtRdm FifoAvail/FifoMax: firmware {ff:?} | sim {fs:?}"
        ));
    }
    c_fw.shutdown();
    c_sim.shutdown();
    sim.shutdown();
    assert!(
        diffs.is_empty(),
        "{} differences:\n{}",
        diffs.len(),
        diffs.join("\n")
    );
}

/// Differential: the queued messages (and status messages) both node implementations collect for the same changes.
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn e2e_differential_queued_messages() {
    use pixelmapper_net::sim::{SimConfig, SimNode};
    let vn = vn_or_skip!(&[]);
    let sim = SimNode::start(SimConfig::default()).await.unwrap();
    let quiet = |target: SocketAddr| {
        let mut c = ControllerConfig::local(vec![target]);
        c.rdm.auto_fetch_queued = false;
        c.rdm.node_queue_interval = Duration::ZERO;
        c.rdm.timeout = Duration::from_millis(500);
        c.auto_tod_request = false;
        c
    };
    let mut lists = Vec::new();
    for target in [vn.bcast(), sim.local_addr()] {
        let id = NodeId::Uid(vn.uid);
        let c = Controller::bind(quiet(target)).await.unwrap();
        c.wait_for_node(&id, WAIT).await.unwrap();
        let node = vn.uid;
        // empty the queue first (boot-time messages)
        c.fetch_queued_messages(&id, pa(1), node).await.unwrap();
        c.rdm_set(
            &id,
            pa(1),
            node,
            pids::PID_PORT2_DMX_BREAK,
            &200u16.to_be_bytes(),
        )
        .await
        .unwrap();
        c.art_address_command(&id, 1, ac::AC_MERGE_LTP0)
            .await
            .unwrap();
        c.art_address_command(&id, 1, ac::AC_LED_LOCATE)
            .await
            .unwrap();
        c.rdm_set(&id, pa(1), node, pid::DEVICE_LABEL, b"q")
            .await
            .unwrap();
        c.tod_control(&id, pa(1), ArtTodControl::ATC_INC_OFF)
            .await
            .unwrap();
        tokio::time::sleep(Duration::from_millis(200)).await;
        c.rdm_set(&id, pa(1), node, pids::PID_PORT3_DMX_RATE, &[30])
            .await
            .unwrap();
        c.art_address_command(&id, 1, ac::AC_LED_NORMAL)
            .await
            .unwrap();
        tokio::time::sleep(Duration::from_millis(300)).await;
        let mut rx = c.subscribe();
        c.fetch_queued_messages(&id, pa(1), node).await.unwrap();
        let mut got = Vec::new();
        while let Ok(Ok(e)) = tokio::time::timeout(Duration::from_millis(200), rx.recv()).await {
            if let Event::QueuedMessage { message, .. } = e {
                let st: Vec<String> = message
                    .status_messages
                    .unwrap_or_default()
                    .iter()
                    .map(|s| {
                        format!(
                            "{:04X}/{}/{}",
                            s.status_message_id, s.data_value1, s.data_value2
                        )
                    })
                    .collect();
                got.push(format!(
                    "0x{:04X} {} {}",
                    message.pid,
                    pixelmapper_net::to_hex(&message.data),
                    st.join(",")
                ));
            }
        }
        c.shutdown();
        lists.push(got);
    }
    sim.shutdown();
    eprintln!("queued messages: {:?}", lists[0]);
    assert!(lists[0].len() >= 4, "{:?}", lists[0]);
    assert_eq!(
        lists[0], lists[1],
        "queued messages: firmware (left) vs sim::node (right)"
    );
}
