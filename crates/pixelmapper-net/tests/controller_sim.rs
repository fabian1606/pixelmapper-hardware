//! Integration tests: the async [`Controller`] against the simulated node over real UDP on localhost.
//! Every test uses ephemeral ports, so the tests run in parallel.

use pixelmapper_net::artnet::{
    address_command, ArtAddress, ArtIpProg, PortAddress, IPPROG_ENABLE, IPPROG_IP, IPPROG_MASK,
};
use pixelmapper_net::controller::{Controller, ControllerConfig, RdmRequest};
use pixelmapper_net::flash::node::{NodeFlashMethod, NodeFlashOptions};
use pixelmapper_net::flash::tube::TubeFlashOptions;
use pixelmapper_net::flash::FlashPhase;
use pixelmapper_net::image::node::build_test_bin;
use pixelmapper_net::image::tube::build_test_image;
use pixelmapper_net::image::{NodeImage, TubeImage};
use pixelmapper_net::link::RdmOptions;
use pixelmapper_net::rdm::pd::{decode_label, DeviceInfo};
use pixelmapper_net::rdm::{cc, pid, NackReason, Uid};
use pixelmapper_net::sim::tube::TubeTiming;
use pixelmapper_net::sim::{SimConfig, SimFixture, SimNode, SimTube, TubeMode};
use pixelmapper_net::{pids, Error, Event, NodeId};
use std::time::Duration;
use tokio::sync::broadcast;

const WAIT: Duration = Duration::from_secs(5);

fn pa(n: u16) -> PortAddress {
    PortAddress::new(n).unwrap()
}

fn fast_tube(uid: Uid, app: Option<TubeImage>) -> SimTube {
    SimTube::new(uid, app).with_timing(TubeTiming {
        app_start_delay: Duration::from_millis(50),
        ..TubeTiming::default()
    })
}

fn tube_image(version: u32) -> TubeImage {
    let app: Vec<u8> = (0..1500u32).map(|i| (i * 13 + version) as u8).collect();
    build_test_image(&app, pids::ESTA_MANUFACTURER, pids::MODEL_TUBE, version)
}

fn ctrl_config(sim: &SimNode) -> ControllerConfig {
    let mut c = ControllerConfig::local(vec![sim.local_addr()]);
    c.poll_interval = Duration::from_millis(300);
    c.reply_timeout = Duration::from_millis(800);
    c.rdm.timeout = Duration::from_millis(150);
    c.rdm.node_queue_interval = Duration::from_millis(300);
    c
}

async fn setup(cfg: SimConfig) -> (SimNode, Controller, NodeId) {
    let sim = SimNode::start(cfg).await.unwrap();
    let ctrl = Controller::bind(ctrl_config(&sim)).await.unwrap();
    let id = NodeId::Uid(sim.uid());
    ctrl.wait_for_node(&id, WAIT).await.unwrap();
    (sim, ctrl, id)
}

/// Wait for the first event matching `f`.
async fn wait_event<T>(
    rx: &mut broadcast::Receiver<Event>,
    mut f: impl FnMut(&Event) -> Option<T>,
) -> T {
    tokio::time::timeout(WAIT, async {
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
    .expect("event did not arrive")
}

#[tokio::test]
async fn discovery_merges_four_binds() {
    let (sim, ctrl, id) = setup(SimConfig::default()).await;
    let n = ctrl.node(&id).unwrap();
    assert_eq!(n.uid, Some(sim.uid()));
    assert_eq!(n.address, sim.local_addr());
    assert_eq!(n.ports.len(), 4);
    for (i, p) in n.ports.iter().enumerate() {
        assert_eq!(p.bind_index as usize, i + 1);
        assert_eq!(p.port_number as usize, i + 1);
        assert_eq!(p.port_address, pa(i as u16 + 1));
        assert!(p.rdm_enabled);
    }
    assert_eq!(n.esta_man, pids::ESTA_MANUFACTURER);
    assert_eq!(ctrl.nodes().len(), 1);
    let j = serde_json::to_value(&n).unwrap();
    assert_eq!(j["id"], sim.uid().to_string());
    assert!(j["ports"][0]["portAddress"].is_number());
    // Node gone after shutdown + timeout
    let mut cfg = ctrl_config(&sim);
    cfg.node_timeout = Duration::from_millis(700);
    let ctrl2 = Controller::bind(cfg).await.unwrap();
    let mut rx = ctrl2.subscribe();
    ctrl2.wait_for_node(&id, WAIT).await.unwrap();
    sim.shutdown();
    let gone = wait_event(&mut rx, |e| match e {
        Event::NodeGone { id } => Some(*id),
        _ => None,
    })
    .await;
    assert_eq!(gone, id);
    ctrl.shutdown();
    ctrl2.shutdown();
}

#[tokio::test]
async fn tod_changes_are_reported() {
    let sim = SimNode::start(SimConfig::default()).await.unwrap();
    let t1 = Uid::new(0x7FF0, 1);
    sim.add_responder(1, Box::new(fast_tube(t1, Some(tube_image(0x500)))));
    let ctrl = Controller::bind(ctrl_config(&sim)).await.unwrap();
    let mut rx = ctrl.subscribe();
    let id = NodeId::Uid(sim.uid());
    // auto ArtTodRequest after discovery
    let (uids, added) = wait_event(&mut rx, |e| match e {
        Event::TodChanged {
            port_address,
            uids,
            added,
            ..
        } if *port_address == pa(1) => Some((uids.clone(), added.clone())),
        _ => None,
    })
    .await;
    assert_eq!(uids, vec![t1]);
    assert_eq!(added, vec![t1]);
    // unsolicited update when a device appears / disappears
    let t2 = Uid::new(0x7FF0, 2);
    sim.add_responder(1, Box::new(fast_tube(t2, Some(tube_image(0x500)))));
    let added = wait_event(&mut rx, |e| match e {
        Event::TodChanged {
            port_address,
            added,
            ..
        } if *port_address == pa(1) && !added.is_empty() => Some(added.clone()),
        _ => None,
    })
    .await;
    assert_eq!(added, vec![t2]);
    sim.remove_responder(1, t1);
    let removed = wait_event(&mut rx, |e| match e {
        Event::TodChanged { removed, .. } if !removed.is_empty() => Some(removed.clone()),
        _ => None,
    })
    .await;
    assert_eq!(removed, vec![t1]);
    assert_eq!(ctrl.tod(&id, pa(1)), Some(vec![t2]));
    assert_eq!(ctrl.tod_refresh(&id, pa(1), WAIT).await.unwrap(), vec![t2]);
    assert_eq!(ctrl.tod_refresh(&id, pa(3), WAIT).await.unwrap(), vec![]);
    ctrl.shutdown();
    sim.shutdown();
}

#[tokio::test]
async fn rdm_get_set_timeout_retry_and_nack() {
    let (sim, ctrl, id) = setup(SimConfig::default()).await;
    let t = Uid::new(0x7FF0, 0x10);
    sim.add_responder(1, Box::new(fast_tube(t, Some(tube_image(0x500)))));
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
    assert_eq!(
        sim.with_responder(t, |x: &mut SimTube| x.start_address()),
        Some(100)
    );
    // two lost answers: the third attempt succeeds (retries = 2)
    sim.drop_rdm_responses(1, 2);
    ctrl.rdm_get(&id, pa(1), t, pid::DEVICE_INFO, &[])
        .await
        .unwrap();
    // three lost answers: timeout after 3 attempts
    sim.drop_rdm_responses(1, 3);
    let e = ctrl
        .rdm_get(&id, pa(1), t, pid::DEVICE_INFO, &[])
        .await
        .unwrap_err();
    assert!(matches!(e, Error::Timeout { attempts: 3, .. }), "{e}");
    assert_eq!(e.code(), "rdm_timeout");
    // missing device
    let e = ctrl
        .rdm_get(&id, pa(1), Uid::new(0x7FF0, 0x99), pid::DEVICE_INFO, &[])
        .await
        .unwrap_err();
    assert!(e.is_timeout());
    // NACK
    let e = ctrl.rdm_get(&id, pa(1), t, 0x0400, &[]).await.unwrap_err();
    assert_eq!(e.nack_reason(), Some(NackReason::UNKNOWN_PID));
    // broadcast SET: no answer expected
    assert_eq!(
        ctrl.rdm_set(&id, pa(1), Uid::BROADCAST, pid::IDENTIFY_DEVICE, &[1])
            .await
            .unwrap(),
        Vec::<u8>::new()
    );
    // unknown port / node
    assert_eq!(
        ctrl.rdm_get(&id, pa(77), t, pid::DEVICE_INFO, &[])
            .await
            .unwrap_err()
            .code(),
        "unknown_port"
    );
    let other = NodeId::Uid(Uid::new(1, 1));
    assert_eq!(
        ctrl.rdm_get(&other, pa(1), t, pid::DEVICE_INFO, &[])
            .await
            .unwrap_err()
            .code(),
        "unknown_node"
    );
    ctrl.shutdown();
    sim.shutdown();
}

#[tokio::test]
async fn many_parallel_requests_respect_fifo() {
    let (sim, ctrl, id) = setup(SimConfig::default()).await;
    let mut uids = Vec::new();
    for i in 0..6 {
        let u = Uid::new(0x7FF0, 0x100 + i);
        sim.add_responder(1, Box::new(fast_tube(u, Some(tube_image(0x500)))));
        uids.push(u);
    }
    let mut set = tokio::task::JoinSet::new();
    for round in 0..4 {
        for &u in &uids {
            let c = ctrl.clone();
            set.spawn(async move {
                (
                    round,
                    c.rdm_get(&id, pa(1), u, pid::SOFTWARE_VERSION_LABEL, &[])
                        .await,
                )
            });
        }
    }
    let mut ok = 0;
    while let Some(r) = set.join_next().await {
        let (_, r) = r.unwrap();
        assert_eq!(decode_label(&r.unwrap()), "tube_fw 0.5.0");
        ok += 1;
    }
    assert_eq!(ok, 24);
    ctrl.shutdown();
    sim.shutdown();
}

#[tokio::test]
async fn ack_timer_overflow_and_describe_device() {
    let (sim, ctrl, id) = setup(SimConfig::default()).await;
    let f = Uid::new(0x0001, 0x42);
    sim.add_responder(2, Box::new(SimFixture::default_par(f)));
    // ACK_TIMER on SET DEVICE_LABEL: resolved through QUEUED_MESSAGE
    ctrl.rdm_set(&id, pa(2), f, pid::DEVICE_LABEL, b"Stage left")
        .await
        .unwrap();
    let label = ctrl
        .rdm_get(&id, pa(2), f, pid::DEVICE_LABEL, &[])
        .await
        .unwrap();
    assert_eq!(decode_label(&label), "Stage left");
    // describe: SUPPORTED_PARAMETERS arrives in ACK_OVERFLOW chunks
    let d = ctrl.describe_device(&id, pa(2), f).await.unwrap();
    assert!(
        d.supported_parameters.len() > 10,
        "{:?}",
        d.supported_parameters
    );
    assert_eq!(
        d.personalities.len(),
        usize::from(d.device_info.personality_count)
    );
    assert!(!d.slots.is_empty());
    assert!(d.slots.iter().all(|s| s.description.is_some()));
    assert_eq!(d.sensors.len(), usize::from(d.device_info.sensor_count));
    assert!(d.parameter_descriptions.iter().any(|p| p.pid == 0x8001));
    assert_eq!(d.device_label.as_deref(), Some("Stage left"));
    assert!(d.warnings.is_empty(), "{:?}", d.warnings);
    let j = serde_json::to_value(&d).unwrap();
    assert!(j["deviceInfo"]["deviceModelId"].is_number());
    // the node itself: every listed manufacturer PID has a PARAMETER_DESCRIPTION
    let nd = ctrl.describe_device(&id, pa(1), sim.uid()).await.unwrap();
    assert_eq!(nd.device_info.device_model_id, pids::MODEL_NODE);
    let listed = pids::PIDS
        .iter()
        .filter(|p| p.listed && p.device == pids::Device::Node)
        .count();
    assert_eq!(nd.parameter_descriptions.len(), listed);
    ctrl.shutdown();
    sim.shutdown();
}

#[tokio::test]
async fn queued_messages_from_device_are_fetched() {
    let (sim, ctrl, id) = setup(SimConfig::default()).await;
    let f = Uid::new(0x0001, 0x43);
    sim.add_responder(2, Box::new(SimFixture::default_par(f)));
    let mut rx = ctrl.subscribe();
    sim.with_responder(f, |x: &mut SimFixture| {
        x.queue_message(0x8001, cc::GET_RESPONSE, vec![2])
    });
    // any response with Message Count > 0 triggers the fetch
    ctrl.rdm_get(&id, pa(2), f, pid::DEVICE_INFO, &[])
        .await
        .unwrap();
    let m = wait_event(&mut rx, |e| match e {
        Event::QueuedMessage { uid, message, .. } if *uid == f => Some(message.clone()),
        _ => None,
    })
    .await;
    assert_eq!((m.pid, m.data.clone()), (0x8001, vec![2]));
    ctrl.shutdown();
    sim.shutdown();
}

#[tokio::test]
async fn last_writer_wins_events_from_second_client_and_panel() {
    let (sim, ctrl, id) = setup(SimConfig::default()).await;
    // The node has ONE queue for all controllers: a client that fetches it itself would consume the message.
    // The second client here does not fetch (like a third-party controller that ignores Message Count).
    let mut ocfg = ctrl_config(&sim);
    ocfg.rdm.auto_fetch_queued = false;
    ocfg.rdm.node_queue_interval = Duration::ZERO;
    let other = Controller::bind(ocfg).await.unwrap();
    other.wait_for_node(&id, WAIT).await.unwrap();
    let mut rx = ctrl.subscribe();
    // Art-Net visible change by another client → unsolicited ArtPollReply
    let mut a = ArtAddress::command(1, address_command::AC_NONE);
    a.short_name = "Renamed".into();
    let st = other.art_address(&id, a).await.unwrap().unwrap();
    assert_eq!(st.short_name, "Renamed");
    let name = wait_event(&mut rx, |e| match e {
        Event::NodeUpdated { node } if node.short_name == "Renamed" => {
            Some(node.short_name.clone())
        }
        _ => None,
    })
    .await;
    assert_eq!(name, "Renamed");
    // RDM-only change by another client → queued message at the node UID
    other
        .rdm_set(
            &id,
            pa(1),
            sim.uid(),
            pids::PID_DISPLAY_DIM_TIMEOUT,
            &120u16.to_be_bytes(),
        )
        .await
        .unwrap();
    let m = wait_event(&mut rx, |e| match e {
        Event::QueuedMessage { message, .. } if message.pid == pids::PID_DISPLAY_DIM_TIMEOUT => {
            Some(message.clone())
        }
        _ => None,
    })
    .await;
    assert_eq!(m.data, 120u16.to_be_bytes());
    // change at the panel
    sim.set_short_name_local("Panel");
    wait_event(&mut rx, |e| match e {
        Event::NodeUpdated { node } if node.short_name == "Panel" => Some(()),
        _ => None,
    })
    .await;
    other.shutdown();
    ctrl.shutdown();
    sim.shutdown();
}

#[tokio::test]
async fn node_configuration() {
    let (sim, ctrl, id) = setup(SimConfig::default()).await;
    // port 2 → Port-Address 0x0105, merge LTP
    let mut a = ArtAddress::command(2, address_command::AC_MERGE_LTP0);
    a.net_switch = ArtAddress::program(1);
    a.sub_switch = ArtAddress::program(0);
    a.sw_out[0] = ArtAddress::program(5);
    let st = ctrl.art_address(&id, a).await.unwrap().unwrap();
    let p = st.ports.iter().find(|p| p.bind_index == 2).unwrap();
    assert_eq!(p.port_address, pa(0x105));
    assert_eq!(p.merge_mode, pixelmapper_net::state::MergeMode::Ltp);
    // LED locate
    let st = ctrl
        .art_address_command(&id, 1, address_command::AC_LED_LOCATE)
        .await
        .unwrap()
        .unwrap();
    assert_eq!(
        st.status1.indicators,
        pixelmapper_net::state::Indicators::Locate
    );
    // IP programming
    let r = ctrl
        .ip_prog(
            &id,
            ArtIpProg {
                command: IPPROG_ENABLE | IPPROG_IP | IPPROG_MASK,
                prog_ip: "2.0.0.20".parse().unwrap(),
                prog_sm: "255.0.0.0".parse().unwrap(),
                prog_port: 0,
                prog_dg: "0.0.0.0".parse().unwrap(),
            },
        )
        .await
        .unwrap();
    assert_eq!(r.prog_ip.to_string(), "2.0.0.20");
    assert_eq!(sim.snapshot().ip.to_string(), "2.0.0.20");
    // TOD flush answered with TodData
    ctrl.tod_flush(&id, pa(1)).await.unwrap();
    ctrl.shutdown();
    sim.shutdown();
}

#[tokio::test]
async fn dmx_sequence_and_sync() {
    let (sim, ctrl, _id) = setup(SimConfig::default()).await;
    assert_eq!(ctrl.send_dmx(pa(3), &[1, 2, 3]).await.unwrap(), 1);
    assert_eq!(ctrl.send_dmx(pa(3), &[4, 5]).await.unwrap(), 1);
    assert_eq!(ctrl.send_dmx(pa(99), &[4, 5]).await.unwrap(), 0);
    ctrl.send_sync().await.unwrap();
    tokio::time::sleep(Duration::from_millis(100)).await;
    let f = sim.last_dmx(3).unwrap();
    assert_eq!(f.sequence, 2);
    assert_eq!(&f.data[..2], &[4, 5]);
    assert_eq!(sim.sync_count(), 1);
    assert!(ctrl.send_dmx(pa(3), &[0; 513]).await.is_err());
    ctrl.shutdown();
    sim.shutdown();
}

fn quick_tube_opts() -> TubeFlashOptions {
    TubeFlashOptions {
        enter_wait: Duration::from_millis(100),
        boot_info_retry_wait: Duration::from_millis(50),
        finish_wait: Duration::from_millis(300),
        ..TubeFlashOptions::default()
    }
}

#[tokio::test]
async fn tube_flash_job_and_errors() {
    let (sim, ctrl, id) = setup(SimConfig::default()).await;
    let t = Uid::new(0x7FF0, 0x20);
    sim.add_responder(1, Box::new(fast_tube(t, Some(tube_image(0x500)))));
    let mut rx = ctrl.subscribe();
    let job = ctrl
        .flash_tube_with(&id, pa(1), t, tube_image(0x600), quick_tube_opts())
        .unwrap();
    let report = job.wait().await.unwrap();
    assert_eq!(report.new_version, 0x600);
    assert_eq!(report.old_version, Some(0x500));
    assert_eq!(
        sim.with_responder(t, |x: &mut SimTube| x.app_version()),
        Some(Some(0x600))
    );
    // progress also on the event stream
    wait_event(&mut rx, |e| match e {
        Event::FlashProgress { progress } if progress.phase == FlashPhase::Done => Some(()),
        _ => None,
    })
    .await;
    // downgrade refused, nothing written
    let job = ctrl
        .flash_tube_with(&id, pa(1), t, tube_image(0x500), quick_tube_opts())
        .unwrap();
    let e = job.wait().await.unwrap_err();
    assert!(e.to_string().contains("downgrade"), "{e}");
    assert_eq!(
        sim.with_responder(t, |x: &mut SimTube| x.mode()),
        Some(TubeMode::App)
    );
    // wrong model refused
    let wrong = build_test_image(&[1, 2, 3, 4], pids::ESTA_MANUFACTURER, 0x77, 0x700);
    let job = ctrl
        .flash_tube_with(&id, pa(1), t, wrong, quick_tube_opts())
        .unwrap();
    assert!(job
        .wait()
        .await
        .unwrap_err()
        .to_string()
        .contains("wrong device"));
    // cancel
    let job = ctrl
        .flash_tube_with(&id, pa(1), t, tube_image(0x601), quick_tube_opts())
        .unwrap();
    let mut prog = job.progress();
    prog.wait_for(|p| p.phase == FlashPhase::Writing)
        .await
        .unwrap();
    job.cancel();
    assert!(matches!(job.wait().await, Err(Error::Cancelled)));
    ctrl.shutdown();
    sim.shutdown();
}

#[tokio::test]
async fn tube_rescue_via_boot_catch() {
    let (sim, ctrl, id) = setup(SimConfig::default()).await;
    let t = Uid::new(0x7FF0, 0x21);
    // app is dead: no answer to ENTER_BOOTLOADER, only the boot window after power-on helps
    sim.add_responder(1, Box::new(fast_tube(t, Some(tube_image(0x500)))));
    let mut opts = quick_tube_opts();
    let node = ctrl.node(&id).unwrap();
    opts.rescue = Some(pixelmapper_net::flash::tube::RescueTarget {
        node_uid: node.uid.unwrap(),
        port: node.port(pa(1)).unwrap().port_number,
    });
    opts.catch_poll_interval = Duration::from_millis(50);
    let job = ctrl
        .flash_tube_with(&id, pa(1), t, tube_image(0x700), opts)
        .unwrap();
    let mut prog = job.progress();
    prog.wait_for(|p| p.phase == FlashPhase::Catching)
        .await
        .unwrap();
    tokio::time::sleep(Duration::from_millis(100)).await;
    sim.power_cycle(t);
    let report = job.wait().await.unwrap();
    assert_eq!(report.new_version, 0x700);
    assert_eq!(
        sim.with_responder(t, |x: &mut SimTube| x.app_version()),
        Some(Some(0x700))
    );
    ctrl.shutdown();
    sim.shutdown();
}

#[tokio::test]
async fn node_flash_both_methods() {
    let cfg = SimConfig {
        fw_version: 0x0001_0000,
        hw_revision: 0,
        ..SimConfig::default()
    };
    let (sim, ctrl, id) = setup(cfg).await;
    for (method, ver) in [
        (NodeFlashMethod::Rdm, 0x0001_0200u32),
        (NodeFlashMethod::ArtFirmwareMaster, 0x0001_0300),
    ] {
        let img = NodeImage::from_bin(build_test_bin(20_000, ver, 1, "test")).unwrap();
        let opts = NodeFlashOptions {
            method,
            reboot_timeout: Duration::from_secs(10),
            ..NodeFlashOptions::default()
        };
        let report = ctrl
            .flash_node(&id, img, opts)
            .unwrap()
            .wait()
            .await
            .unwrap();
        assert_eq!(report.new_version, ver, "{method:?}");
        assert_eq!(sim.snapshot().fw_version, ver);
        let n = ctrl.node(&id).unwrap();
        assert_eq!(
            n.firmware_version,
            ((ver >> 16) << 8 | ((ver >> 8) & 0xFF)) as u16
        );
    }
    // hardware revision 0 not in the mask: refused before writing
    let img = NodeImage::from_bin(build_test_bin(20_000, 0x0001_0400, 0b10, "x")).unwrap();
    let e = ctrl
        .flash_node(&id, img, NodeFlashOptions::default())
        .unwrap()
        .wait()
        .await
        .unwrap_err();
    assert!(e.to_string().to_lowercase().contains("revision"), "{e}");
    assert_eq!(sim.snapshot().fw_version, 0x0001_0300);
    ctrl.shutdown();
    sim.shutdown();
}

#[tokio::test]
async fn rdm_request_options_buffer_full() {
    let (sim, ctrl, id) = setup(SimConfig::default()).await;
    let t = Uid::new(0x7FF0, 0x30);
    sim.add_responder(1, Box::new(fast_tube(t, None)));
    // blank tube sits in its bootloader; a page write makes it busy for the next requests
    let page = [0u8; 66];
    ctrl.rdm_set(&id, pa(1), t, pids::PID_BOOT_WRITE_PAGE, &page)
        .await
        .unwrap();
    let e = ctrl
        .rdm_set(&id, pa(1), t, pids::PID_BOOT_WRITE_PAGE, &page)
        .await
        .unwrap_err();
    assert_eq!(e.nack_reason(), Some(NackReason::BUFFER_FULL));
    ctrl.rdm(
        &id,
        pa(1),
        RdmRequest {
            uid: t,
            command_class: cc::SET,
            pid: pids::PID_BOOT_WRITE_PAGE,
            data: page.to_vec(),
            options: RdmOptions::default().with_buffer_full_retries(10),
        },
    )
    .await
    .unwrap();
    ctrl.shutdown();
    sim.shutdown();
}
