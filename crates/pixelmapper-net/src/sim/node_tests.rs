//! Tests of the simulated node over real UDP on localhost, using only the crate codecs.

use super::node::{SimConfig, SimDirection, SimNode};
use super::SimResponder;
use crate::artnet::{
    address_command as ac, ArtAddress, ArtDmx, ArtFirmwareMaster, ArtFirmwareReply, ArtIpProg,
    ArtPacket, ArtPoll, ArtPollReply, ArtRdm, ArtSync, ArtTodData, ArtTodRequest, PortAddress,
    IPPROG_DHCP, IPPROG_ENABLE, IPPROG_IP, IPPROG_MASK,
};
use crate::image::node::{NodeImage, NodeImageDesc, ALF_OEM_OFFSET, DESC_LEN, DESC_OFFSET};
use crate::pids::{self, boot_catch_state, fw_error, fw_state};
use crate::rdm::pd::{
    decode_label, decode_pid_list, BootCatchStart, BootCatchStatus, BootInfo, DeviceInfo, FwInfo,
    FwWrite, Ipv4CurrentAddress, LocalRdmSet, ParameterDescription, StatusMessage,
};
use crate::rdm::{cc, pid as p, status_type, NackReason, RdmMessage, ResponseType, Uid};
use std::any::Any;
use std::net::Ipv4Addr;
use std::sync::atomic::{AtomicU8, Ordering};
use std::time::{Duration, Instant};
use tokio::net::UdpSocket;

const CONTROLLER: Uid = Uid::new(0x7FF0, 0xA000_0001);
const WAIT: Duration = Duration::from_millis(500);

fn pa(n: u16) -> PortAddress {
    PortAddress::new(n).unwrap()
}

/// A trivial RDM device: answers DEVICE_INFO and DEVICE_LABEL, and BOOT_INFO only within 200 ms after a power
/// cycle. With `catch_only` it answers nothing but that BOOT_INFO.
struct TestDev {
    uid: Uid,
    catch_only: bool,
    boot_window: Option<Instant>,
    label: String,
}

impl TestDev {
    fn new(device: u32) -> Box<Self> {
        Box::new(Self {
            uid: Uid::new(0x7FF0, device),
            catch_only: false,
            boot_window: None,
            label: String::new(),
        })
    }
}

impl SimResponder for TestDev {
    fn uid(&self) -> Uid {
        self.uid
    }
    fn handle(&mut self, req: &RdmMessage, now: Instant) -> Option<RdmMessage> {
        if req.dest_uid != self.uid {
            return None;
        }
        let ack = |d: Vec<u8>| Some(req.response_to(self.uid, ResponseType::Ack, 0, d));
        match (req.command_class, req.parameter_id) {
            (cc::GET, pids::PID_BOOT_INFO) if self.boot_window.is_some_and(|t| now < t) => {
                ack(BootInfo {
                    version: 3,
                    page_size: 64,
                    app_end: 0x1800,
                    busy: 0,
                    last_error: 0,
                    manufacturer: 0x7FF0,
                    model: pids::MODEL_TUBE,
                    boot_sw_version: 0xB000_0003,
                    app_valid: 1,
                    app_fw_version: 0x0500,
                }
                .encode())
            }
            _ if self.catch_only => None,
            (cc::GET, p::DEVICE_INFO) => ack(DeviceInfo {
                rdm_protocol_version: 0x0100,
                device_model_id: pids::MODEL_TUBE,
                product_category: 0x0101,
                software_version_id: 0x0500,
                dmx_footprint: 12,
                current_personality: 1,
                personality_count: 1,
                dmx_start_address: 1,
                sub_device_count: 0,
                sensor_count: 0,
            }
            .encode()),
            (cc::SET, p::DEVICE_LABEL) => {
                let label = decode_label(&req.parameter_data);
                let r = ack(vec![]);
                self.label = label;
                r
            }
            _ => Some(req.response_to(
                self.uid,
                ResponseType::NackReason,
                0,
                NackReason::UNKNOWN_PID.0.to_be_bytes().to_vec(),
            )),
        }
    }
    fn power_cycle(&mut self, now: Instant) {
        self.boot_window = Some(now + Duration::from_millis(200));
    }
    fn as_any(&self) -> &dyn Any {
        self
    }
    fn as_any_mut(&mut self) -> &mut dyn Any {
        self
    }
}

/// A test controller socket talking to one simulated node.
struct Ctl {
    sock: UdpSocket,
    node: SimNode,
    tn: AtomicU8,
}

impl Ctl {
    async fn start(cfg: SimConfig) -> Self {
        let node = SimNode::start(cfg).await.unwrap();
        Self::attach(&node).await
    }

    async fn attach(node: &SimNode) -> Self {
        Self {
            sock: UdpSocket::bind("127.0.0.1:0").await.unwrap(),
            node: node.clone(),
            tn: AtomicU8::new(0),
        }
    }

    async fn send(&self, data: &[u8]) {
        self.sock
            .send_to(data, self.node.local_addr())
            .await
            .unwrap();
    }

    async fn recv(&self, dur: Duration) -> Option<ArtPacket> {
        let mut buf = vec![0u8; 2048];
        let deadline = tokio::time::Instant::now() + dur;
        loop {
            let r = tokio::time::timeout_at(deadline, self.sock.recv_from(&mut buf)).await;
            let (n, _) = r.ok()?.ok()?;
            if let Ok(pkt) = ArtPacket::decode(&buf[..n]) {
                return Some(pkt);
            }
        }
    }

    async fn collect(&self, dur: Duration) -> Vec<ArtPacket> {
        let deadline = tokio::time::Instant::now() + dur;
        let mut v = Vec::new();
        while let Some(pkt) = self
            .recv(deadline.saturating_duration_since(tokio::time::Instant::now()))
            .await
        {
            v.push(pkt);
        }
        v
    }

    async fn wait_for<T>(
        &self,
        dur: Duration,
        mut f: impl FnMut(ArtPacket) -> Option<T>,
    ) -> Option<T> {
        let deadline = tokio::time::Instant::now() + dur;
        loop {
            let left = deadline.saturating_duration_since(tokio::time::Instant::now());
            if let Some(t) = f(self.recv(left).await?) {
                return Some(t);
            }
        }
    }

    async fn poll_replies(&self, poll: ArtPoll) -> Vec<ArtPollReply> {
        self.send(&poll.encode()).await;
        let mut v = replies(self.collect(Duration::from_millis(200)).await);
        v.sort_by_key(|r| r.bind_index);
        v
    }

    fn request(&self, dest: Uid, class: u8, pid: u16, data: Vec<u8>) -> RdmMessage {
        let tn = self.tn.fetch_add(1, Ordering::Relaxed);
        RdmMessage::request(dest, CONTROLLER, tn, 1, 0, class, pid, data)
    }

    async fn send_rdm(&self, port: PortAddress, req: &RdmMessage) {
        self.send(&ArtRdm::from_message(port, req).encode()).await;
    }

    async fn rdm_at(&self, port: PortAddress, req: RdmMessage) -> Option<(ArtRdm, RdmMessage)> {
        self.send_rdm(port, &req).await;
        self.wait_for(WAIT, |pkt| match pkt {
            ArtPacket::Rdm(r) => {
                let m = r.message().ok()?;
                (m.transaction_number == req.transaction_number && m.src_uid == req.dest_uid)
                    .then_some((r, m))
            }
            _ => None,
        })
        .await
    }

    async fn node(&self, class: u8, pid: u16, data: Vec<u8>) -> RdmMessage {
        let req = self.request(self.node.uid(), class, pid, data);
        self.rdm_at(pa(1), req).await.expect("node answer").1
    }

    async fn get(&self, pid: u16, data: Vec<u8>) -> RdmMessage {
        self.node(cc::GET, pid, data).await
    }

    async fn set(&self, pid: u16, data: Vec<u8>) -> RdmMessage {
        self.node(cc::SET, pid, data).await
    }

    async fn queued(&self) -> RdmMessage {
        self.get(p::QUEUED_MESSAGE, vec![status_type::ADVISORY])
            .await
    }
}

fn replies(pkts: Vec<ArtPacket>) -> Vec<ArtPollReply> {
    pkts.into_iter()
        .filter_map(|p| match p {
            ArtPacket::PollReply(r) => Some(r),
            _ => None,
        })
        .collect()
}

fn assert_ack(m: &RdmMessage) {
    assert_eq!(m.response_type(), Some(ResponseType::Ack), "{m:?}");
}

fn assert_nack(m: &RdmMessage, reason: NackReason) {
    assert_eq!(m.nack_reason(), Some(reason), "{m:?}");
}

// ---------------------------------------------------------------------------------------------------------------
// Discovery and configuration
// ---------------------------------------------------------------------------------------------------------------

#[tokio::test]
async fn poll_gives_four_replies() {
    let cfg = SimConfig::default();
    let mac = cfg.mac;
    let ctl = Ctl::start(cfg).await;
    assert_eq!(ctl.node.uid(), Uid::new(0x7FF0, 0x8012_3456));
    let replies = ctl.poll_replies(ArtPoll::new_reply_on_change()).await;
    assert_eq!(replies.len(), 4);
    for (k, r) in replies.iter().enumerate() {
        assert_eq!(usize::from(r.bind_index), k + 1);
        assert_eq!(r.num_ports, 1);
        assert_eq!(r.port_address_out(0), pa(k as u16 + 1));
        assert_eq!(r.default_resp_uid, ctl.node.uid());
        assert_eq!(r.esta_man, pids::ESTA_MANUFACTURER);
        assert_eq!(r.oem, pids::ARTNET_OEM_NODE);
        assert_eq!(r.mac, mac);
        assert_eq!(r.ip, Ipv4Addr::LOCALHOST);
        assert_eq!(r.bind_ip, Ipv4Addr::LOCALHOST);
        assert_eq!(r.port_types[0], 0x80);
        assert_eq!(r.refresh_rate, 44);
        assert_eq!(r.vers_info, 0x0001);
        assert_eq!(r.status1 & 0xC2, 0xC2, "indicators normal + RDM capable");
        assert_eq!(r.status2 & 0x0E, 0x0E, "15 bit, DHCP capable + active");
        assert_eq!(r.status3 & 0x28, 0x28);
        assert_eq!(r.good_output_b[0] & 0x80, 0, "RDM enabled");
        assert_eq!(r.long_name, "Pixelmapper Node (simulated)");
        assert!(r.node_report.starts_with("#0001 ["), "{}", r.node_report);
    }
    assert_eq!(ctl.node.snapshot().pollers.len(), 1);
    ctl.node.shutdown();
}

#[tokio::test]
async fn targeted_poll_answers_only_matching_binds() {
    let ctl = Ctl::start(SimConfig::default()).await;
    let r = ctl.poll_replies(ArtPoll::new_targeted(pa(2), pa(3))).await;
    let binds: Vec<u8> = r.iter().map(|r| r.bind_index).collect();
    assert_eq!(binds, vec![2, 3]);
    assert!(ctl
        .poll_replies(ArtPoll::new_targeted(pa(100), pa(200)))
        .await
        .is_empty());
}

#[tokio::test]
async fn plain_poll_is_not_remembered() {
    let ctl = Ctl::start(SimConfig::default()).await;
    assert_eq!(ctl.poll_replies(ArtPoll::default()).await.len(), 4);
    assert!(ctl.node.snapshot().pollers.is_empty());
}

#[tokio::test]
async fn art_address_answers_sender_and_informs_pollers() {
    let a = Ctl::start(SimConfig::default()).await;
    let b = Ctl::attach(&a.node).await;
    assert_eq!(
        a.poll_replies(ArtPoll::new_reply_on_change()).await.len(),
        4
    );

    let addr = ArtAddress {
        bind_index: 2,
        short_name: "Stage L".into(),
        sw_out: [ArtAddress::program(7), 0x7F, 0x7F, 0x7F],
        command: ac::AC_LED_LOCATE,
        ..Default::default()
    };
    b.send(&addr.encode()).await;
    let to_b = replies(b.collect(Duration::from_millis(150)).await);
    let reply_b2 = to_b
        .iter()
        .find(|r| r.bind_index == 2)
        .expect("reply to the sender");
    assert_eq!(reply_b2.short_name, "Stage L");
    assert_eq!(reply_b2.port_address_out(0), pa(7));
    assert_eq!(reply_b2.status1 >> 6, 0b01, "locate");

    let to_a = replies(a.collect(Duration::from_millis(150)).await);
    assert_eq!(to_a.len(), 4, "name and indicators are node-wide");
    assert!(to_a.iter().all(|r| r.short_name == "Stage L"));

    // RDM off on bind 3 -> only that bind changes
    a.send(&ArtAddress::command(3, ac::AC_RDM_DISABLE0).encode())
        .await;
    let r = replies(a.collect(Duration::from_millis(150)).await);
    assert_eq!(r.len(), 1);
    assert_eq!(r[0].bind_index, 3);
    assert_eq!(r[0].good_output_b[0] & 0x80, 0x80);

    // reset of the universe nibble with 0x00
    a.send(
        &ArtAddress {
            bind_index: 2,
            sw_out: [ArtAddress::RESET, 0x7F, 0x7F, 0x7F],
            ..Default::default()
        }
        .encode(),
    )
    .await;
    a.collect(Duration::from_millis(100)).await;
    let snap = a.node.snapshot();
    assert_eq!(snap.ports[1].port_address, pa(2));
    assert!(!snap.ports[2].rdm_enabled);

    // direction to input
    a.send(&ArtAddress::command(4, ac::AC_DIRECTION_RX0).encode())
        .await;
    let r = a
        .wait_for(WAIT, |p| match p {
            ArtPacket::PollReply(r) if r.bind_index == 4 => Some(r),
            _ => None,
        })
        .await
        .unwrap();
    assert_eq!(r.port_types[0], 0x40);
    assert_eq!(a.node.snapshot().ports[3].direction, SimDirection::Input);
}

#[tokio::test]
async fn ip_prog_reply_and_new_ip() {
    let ctl = Ctl::start(SimConfig::default()).await;
    let ip_reply = |p| match p {
        ArtPacket::IpProgReply(r) => Some(r),
        _ => None,
    };
    ctl.send(&ArtIpProg::default().encode()).await;
    let r = ctl.wait_for(WAIT, ip_reply).await.unwrap();
    assert_eq!(r.prog_ip, Ipv4Addr::LOCALHOST);
    assert!(r.dhcp_enabled());

    ctl.send(
        &ArtIpProg {
            command: IPPROG_ENABLE | IPPROG_IP | IPPROG_MASK,
            prog_ip: Ipv4Addr::new(2, 0, 0, 20),
            prog_sm: Ipv4Addr::new(255, 0, 0, 0),
            ..Default::default()
        }
        .encode(),
    )
    .await;
    let r = ctl.wait_for(WAIT, ip_reply).await.unwrap();
    assert_eq!(r.prog_ip, Ipv4Addr::new(2, 0, 0, 20));
    assert_eq!(r.prog_sm, Ipv4Addr::new(255, 0, 0, 0));
    assert!(!r.dhcp_enabled());
    let rs = ctl.poll_replies(ArtPoll::default()).await;
    assert_eq!(rs.len(), 4);
    assert!(rs
        .iter()
        .all(|r| r.ip == Ipv4Addr::new(2, 0, 0, 20) && r.status2 & 0x02 == 0));
    let cur = Ipv4CurrentAddress::decode(
        &ctl.get(p::IPV4_CURRENT_ADDRESS, 1u32.to_be_bytes().to_vec())
            .await
            .parameter_data,
    )
    .unwrap();
    assert_eq!(
        (cur.address, cur.netmask, cur.dhcp_status),
        (Ipv4Addr::new(2, 0, 0, 20), 8, 0)
    );

    ctl.send(
        &ArtIpProg {
            command: IPPROG_ENABLE | IPPROG_DHCP,
            ..Default::default()
        }
        .encode(),
    )
    .await;
    let r = ctl.wait_for(WAIT, ip_reply).await.unwrap();
    assert_eq!(r.prog_ip, Ipv4Addr::LOCALHOST);
    assert!(r.dhcp_enabled());
}

#[tokio::test]
async fn dmx_merge_and_sync() {
    let a = Ctl::start(SimConfig::default()).await;
    let b = Ctl::attach(&a.node).await;
    a.poll_replies(ArtPoll::new_reply_on_change()).await;
    a.send(
        &ArtDmx::new(1, 0, pa(1), vec![10, 0, 30, 0])
            .unwrap()
            .encode(),
    )
    .await;
    a.send(&ArtSync::default().encode()).await;
    let bind1 = |p| match p {
        ArtPacket::PollReply(r) if r.bind_index == 1 => Some(r),
        _ => None,
    };
    let r = a.wait_for(WAIT, bind1).await.unwrap();
    assert_eq!(r.good_output_a[0] & 0x80, 0x80, "outputting");
    assert_eq!(a.node.sync_count(), 1);
    assert_eq!(a.node.last_dmx(1).unwrap().data, vec![10, 0, 30, 0]);

    // same IP, other Physical = second source -> HTP merge, unsolicited reply with the merge bit
    b.send(
        &ArtDmx::new(1, 1, pa(1), vec![0, 20, 5, 0])
            .unwrap()
            .encode(),
    )
    .await;
    let r = a.wait_for(WAIT, bind1).await.unwrap();
    assert_eq!(r.good_output_a[0] & 0x08, 0x08, "merging");
    assert_eq!(a.node.last_dmx(1).unwrap().data, vec![10, 20, 30, 0]);
    a.send(&ArtSync::default().encode()).await;
    tokio::time::sleep(Duration::from_millis(50)).await;
    assert_eq!(a.node.sync_count(), 1, "ArtSync ignored while merging");
}

// ---------------------------------------------------------------------------------------------------------------
// TOD and RDM proxying
// ---------------------------------------------------------------------------------------------------------------

fn tod_packets(pkts: Vec<ArtPacket>, bind: u8) -> Vec<ArtTodData> {
    let mut v: Vec<ArtTodData> = pkts
        .into_iter()
        .filter_map(|p| match p {
            ArtPacket::TodData(t) if t.bind_index == bind => Some(t),
            _ => None,
        })
        .collect();
    v.sort_by_key(|t| t.block_count);
    v
}

#[tokio::test]
async fn tod_request_and_unsolicited_tod_data() {
    let ctl = Ctl::start(SimConfig::default()).await;
    for d in 0..250 {
        ctl.node.add_responder(1, TestDev::new(0x100 + d));
    }
    ctl.send(&ArtTodRequest::for_port_addresses(0, &[pa(1), pa(2)]).encode())
        .await;
    let pkts = ctl.collect(Duration::from_millis(150)).await;
    let port1 = tod_packets(pkts.clone(), 1);
    assert_eq!(port1.len(), 2);
    assert_eq!(port1[0].uids.len(), 200);
    assert_eq!(port1[1].uids.len(), 50);
    assert!(port1.iter().all(|t| t.uid_total == 250 && t.port == 1));
    assert_eq!(port1[1].block_count, 1);
    let port2 = tod_packets(pkts, 2);
    assert_eq!(port2.len(), 1);
    assert_eq!((port2[0].uid_total, port2[0].uids.len()), (0, 0));

    // new device -> unsolicited ArtTodData to the requester
    ctl.node.add_responder(1, TestDev::new(0x9999));
    let port1 = tod_packets(ctl.collect(Duration::from_millis(150)).await, 1);
    assert_eq!(port1.len(), 2);
    assert!(port1.iter().all(|t| t.uid_total == 251));
    assert!(port1[1].uids.contains(&Uid::new(0x7FF0, 0x9999)));

    // removed -> again
    assert!(ctl
        .node
        .remove_responder(1, Uid::new(0x7FF0, 0x9999))
        .is_some());
    let port1 = tod_packets(ctl.collect(Duration::from_millis(150)).await, 1);
    assert!(!port1.is_empty());
    assert!(port1.iter().all(|t| t.uid_total == 250));
}

#[tokio::test]
async fn rdm_fifo_limit_and_lost_answers() {
    let ctl = Ctl::start(SimConfig {
        rdm_latency: Duration::from_millis(40),
        ..Default::default()
    })
    .await;
    let dev = Uid::new(0x7FF0, 0x42);
    ctl.node.add_responder(1, TestDev::new(0x42));
    for _ in 0..6 {
        let req = ctl.request(dev, cc::GET, p::DEVICE_INFO, vec![]);
        ctl.send_rdm(pa(1), &req).await;
    }
    let answers: Vec<ArtRdm> = ctl
        .collect(Duration::from_millis(500))
        .await
        .into_iter()
        .filter_map(|p| match p {
            ArtPacket::Rdm(r) => Some(r),
            _ => None,
        })
        .collect();
    assert_eq!(answers.len(), 4, "FifoMax 4, the rest is dropped");
    let avail: Vec<u8> = answers.iter().map(|r| r.fifo_avail).collect();
    assert_eq!(avail, vec![1, 2, 3, 4]);
    assert!(answers.iter().all(|r| r.fifo_max == 4));
    let m = answers[0].message().unwrap();
    assert_eq!(m.src_uid, dev);
    assert_eq!(
        DeviceInfo::decode(&m.parameter_data)
            .unwrap()
            .device_model_id,
        pids::MODEL_TUBE
    );

    ctl.node.drop_rdm_responses(1, 1);
    let req = ctl.request(dev, cc::GET, p::DEVICE_INFO, vec![]);
    assert!(ctl.rdm_at(pa(1), req).await.is_none(), "answer lost");
    let req = ctl.request(dev, cc::GET, p::DEVICE_INFO, vec![]);
    assert!(ctl.rdm_at(pa(1), req).await.is_some());
    // RDM disabled on the port -> nothing goes to the line
    ctl.send(&ArtAddress::command(1, ac::AC_RDM_DISABLE0).encode())
        .await;
    let req = ctl.request(dev, cc::GET, p::DEVICE_INFO, vec![]);
    assert!(ctl.rdm_at(pa(1), req).await.is_none());
}

#[tokio::test]
async fn local_rdm_set_is_reported() {
    let ctl = Ctl::start(SimConfig::default()).await;
    let dev = Uid::new(0x7FF0, 0x77);
    ctl.node.add_responder(3, TestDev::new(0x77));
    assert!(ctl
        .node
        .local_rdm_set(3, dev, p::DEVICE_LABEL, b"Front".to_vec()));
    assert_eq!(
        ctl.node
            .with_responder::<TestDev, _>(dev, |d| d.label.clone())
            .as_deref(),
        Some("Front")
    );
    let m = ctl.queued().await;
    assert_eq!(m.parameter_id, pids::PID_LOCAL_RDM_SET);
    let l = LocalRdmSet::decode(&m.parameter_data).unwrap();
    assert_eq!(
        (l.sequence, l.port, l.uid, l.pid),
        (1, 3, dev, p::DEVICE_LABEL)
    );
    let m = ctl.queued().await;
    let s = StatusMessage::decode_list(&m.parameter_data).unwrap();
    assert_eq!(s.len(), 1);
    assert_eq!(s[0].status_message_id, pids::status::LOCAL_RDM_SET);
    assert_eq!(
        (s[0].data_value1, s[0].data_value2),
        (3, p::DEVICE_LABEL as i16)
    );
}

// ---------------------------------------------------------------------------------------------------------------
// The node's own responder
// ---------------------------------------------------------------------------------------------------------------

#[tokio::test]
async fn node_responder_device_info_and_descriptions() {
    let ctl = Ctl::start(SimConfig::default()).await;
    let m = ctl.get(p::DEVICE_INFO, vec![]).await;
    assert_ack(&m);
    let di = DeviceInfo::decode(&m.parameter_data).unwrap();
    assert_eq!(di.device_model_id, pids::MODEL_NODE);
    assert_eq!(di.product_category, pids::NODE_PRODUCT_CATEGORY);
    assert_eq!(di.software_version_id, 0x0000_0100);
    assert_eq!((di.dmx_footprint, di.dmx_start_address), (0, 0xFFFF));

    let list = decode_pid_list(
        &ctl.get(p::SUPPORTED_PARAMETERS, vec![])
            .await
            .parameter_data,
    )
    .unwrap();
    for std in pids::NODE_STD_PIDS_LISTED {
        assert!(list.contains(std));
    }
    let manufacturer: Vec<u16> = list.iter().copied().filter(|&x| x >= 0x8000).collect();
    let expected = pids::PIDS
        .iter()
        .filter(|d| d.device == pids::Device::Node && d.listed)
        .count();
    assert_eq!(manufacturer.len(), expected);
    for pid in manufacturer {
        let m = ctl
            .get(p::PARAMETER_DESCRIPTION, pid.to_be_bytes().to_vec())
            .await;
        assert_ack(&m);
        let d = ParameterDescription::decode(&m.parameter_data).unwrap();
        assert_eq!(d.pid, pid);
        assert!(!d.description.is_empty());
    }
    let m = ctl
        .get(
            p::PARAMETER_DESCRIPTION,
            p::DEVICE_INFO.to_be_bytes().to_vec(),
        )
        .await;
    assert_nack(&m, NackReason::DATA_OUT_OF_RANGE);

    // NACK order
    assert_nack(
        &ctl.get(p::DEVICE_HOURS, vec![]).await,
        NackReason::UNKNOWN_PID,
    );
    assert_nack(
        &ctl.set(p::DEVICE_INFO, vec![]).await,
        NackReason::UNSUPPORTED_COMMAND_CLASS,
    );
    assert_nack(
        &ctl.get(p::DEVICE_INFO, vec![1]).await,
        NackReason::FORMAT_ERROR,
    );
    assert_nack(
        &ctl.set(pids::PID_PORT1_DMX_RATE, vec![45]).await,
        NackReason::DATA_OUT_OF_RANGE,
    );
    let mut req = ctl.request(ctl.node.uid(), cc::GET, p::DEVICE_INFO, vec![]);
    req.sub_device = 1;
    let (_, m) = ctl.rdm_at(pa(1), req).await.unwrap();
    assert_nack(&m, NackReason::SUB_DEVICE_OUT_OF_RANGE);

    // reachable via any port's Port-Address, even a disabled one
    assert_ack(&ctl.set(pids::PID_PORT4_ENABLED, vec![0]).await);
    let req = ctl.request(ctl.node.uid(), cc::GET, p::DEVICE_LABEL, vec![]);
    let (_, m) = ctl.rdm_at(pa(4), req).await.unwrap();
    assert_eq!(decode_label(&m.parameter_data), "PM Node");

    // DEVICE_LABEL = short name (17 characters) -> visible in ArtPollReply
    assert_ack(
        &ctl.set(p::DEVICE_LABEL, b"A rather long label!".to_vec())
            .await,
    );
    let rs = ctl.poll_replies(ArtPoll::default()).await;
    assert_eq!(rs[0].short_name, "A rather long lab");
    assert_ack(&ctl.set(p::IDENTIFY_DEVICE, vec![1]).await);
    let rs = ctl.poll_replies(ArtPoll::default()).await;
    assert_eq!(rs[0].status1 >> 6, 0b01);
}

#[tokio::test]
async fn queued_messages_for_rdm_settings() {
    let ctl = Ctl::start(SimConfig::default()).await;
    let m = ctl.set(pids::PID_PANEL_LOCK, vec![1]).await;
    assert_ack(&m);
    assert_eq!(m.message_count, 1);
    let m = ctl.queued().await;
    assert_eq!(m.parameter_id, pids::PID_PANEL_LOCK);
    assert_eq!(m.command_class, cc::GET_RESPONSE);
    assert_eq!(m.parameter_data, vec![1]);
    assert_eq!(m.message_count, 0);
    let m = ctl.queued().await;
    assert_eq!(
        (m.parameter_id, m.parameter_data.len()),
        (p::STATUS_MESSAGES, 0)
    );

    // unlocked at the device -> PANEL_LOCK + status PANEL_UNLOCKED_LOCAL
    ctl.node.set_panel_lock_local(false);
    let m = ctl.queued().await;
    assert_eq!(m.message_count, 1, "the status message is still waiting");
    assert_eq!(
        (m.parameter_id, m.parameter_data.clone()),
        (pids::PID_PANEL_LOCK, vec![0])
    );
    let m = ctl.queued().await;
    assert_eq!(m.parameter_id, p::STATUS_MESSAGES);
    let s = StatusMessage::decode_list(&m.parameter_data).unwrap();
    assert_eq!(s[0].status_message_id, pids::status::PANEL_UNLOCKED_LOCAL);
    let again = ctl
        .get(p::QUEUED_MESSAGE, vec![status_type::GET_LAST_MESSAGE])
        .await;
    assert_eq!(again.parameter_data, m.parameter_data);
    let d = ctl
        .get(
            p::STATUS_ID_DESCRIPTION,
            pids::status::PANEL_UNLOCKED_LOCAL.to_be_bytes().to_vec(),
        )
        .await;
    assert_eq!(
        decode_label(&d.parameter_data),
        "Panel lock released at device"
    );

    // FACTORY_DEFAULTS
    assert_ack(
        &ctl.set(pids::PID_DISPLAY_DIM_TIMEOUT, 10u16.to_be_bytes().to_vec())
            .await,
    );
    assert_eq!(
        ctl.get(p::FACTORY_DEFAULTS, vec![]).await.parameter_data,
        vec![0]
    );
    assert_ack(&ctl.set(p::FACTORY_DEFAULTS, vec![]).await);
    assert_eq!(
        ctl.get(p::FACTORY_DEFAULTS, vec![]).await.parameter_data,
        vec![1]
    );
    assert_eq!(ctl.node.snapshot().display_dim_timeout, 60);
}

async fn catch_status(ctl: &Ctl) -> BootCatchStatus {
    BootCatchStatus::decode(
        &ctl.get(pids::PID_BOOT_CATCH_STATUS, vec![])
            .await
            .parameter_data,
    )
    .unwrap()
}

#[tokio::test]
async fn boot_catch_catches_after_power_cycle() {
    let ctl = Ctl::start(SimConfig::default()).await;
    let tube = Uid::new(0x7FF0, 0x0BAD_CAFE);
    let mut dev = TestDev::new(0x0BAD_CAFE);
    dev.catch_only = true;
    ctl.node.add_responder(2, dev);
    let start = |port: u8, uid: Uid| {
        BootCatchStart {
            port,
            target_uid: uid,
        }
        .encode()
    };
    assert_nack(
        &ctl.set(pids::PID_BOOT_CATCH_START, start(5, tube)).await,
        NackReason::DATA_OUT_OF_RANGE,
    );
    assert_nack(
        &ctl.set(pids::PID_BOOT_CATCH_START, start(2, Uid::BROADCAST))
            .await,
        NackReason::DATA_OUT_OF_RANGE,
    );
    assert_ack(&ctl.set(pids::PID_BOOT_CATCH_START, start(2, tube)).await);
    tokio::time::sleep(Duration::from_millis(100)).await;
    let st = catch_status(&ctl).await;
    assert_eq!(st.state, boot_catch_state::RUNNING);
    assert!(st.attempts >= 2, "{st:?}");

    ctl.node.power_cycle(tube);
    let mut caught = None;
    for _ in 0..50 {
        let st = catch_status(&ctl).await;
        if st.state != boot_catch_state::RUNNING {
            caught = Some(st);
            break;
        }
        tokio::time::sleep(Duration::from_millis(10)).await;
    }
    let st = caught.expect("caught");
    assert_eq!(
        (st.state, st.port, st.target_uid),
        (boot_catch_state::CAUGHT, 2, tube)
    );
    let m = ctl.queued().await;
    assert_eq!(m.parameter_id, pids::PID_BOOT_CATCH_STATUS);
    assert_eq!(
        BootCatchStatus::decode(&m.parameter_data).unwrap().state,
        boot_catch_state::CAUGHT
    );
    let m = ctl.queued().await;
    let s = StatusMessage::decode_list(&m.parameter_data).unwrap();
    assert_eq!(s.len(), 1);
    assert_eq!(
        (s[0].status_message_id, s[0].data_value1),
        (pids::status::BOOT_CATCH_CAUGHT, 2)
    );

    // abort
    assert_ack(
        &ctl.set(pids::PID_BOOT_CATCH_START, start(2, Uid::new(0x7FF0, 1)))
            .await,
    );
    assert_ack(&ctl.set(pids::PID_BOOT_CATCH_ABORT, vec![]).await);
    assert_eq!(catch_status(&ctl).await.state, boot_catch_state::ABORTED);
}

// ---------------------------------------------------------------------------------------------------------------
// Firmware update
// ---------------------------------------------------------------------------------------------------------------

fn test_image(len: usize, fw: u32, hw_rev_mask: u32, label: &str) -> NodeImage {
    let mut bin: Vec<u8> = (0..len).map(|i| (i * 7 + 3) as u8).collect();
    bin[0] = 0xE9;
    let d = NodeImageDesc::new(fw, hw_rev_mask, label).encode();
    bin[DESC_OFFSET..DESC_OFFSET + DESC_LEN].copy_from_slice(&d);
    NodeImage::from_bin(bin).unwrap()
}

async fn fw_info(ctl: &Ctl) -> FwInfo {
    FwInfo::decode(&ctl.get(pids::PID_FW_INFO, vec![]).await.parameter_data).unwrap()
}

async fn fw_write(ctl: &Ctl, offset: usize, data: &[u8]) -> RdmMessage {
    let w = FwWrite::new(offset as u32, data.to_vec()).unwrap();
    ctl.set(pids::PID_FW_WRITE, w.encode()).await
}

#[tokio::test]
async fn rdm_firmware_update() {
    let ctl = Ctl::start(SimConfig {
        reboot_delay: Duration::from_millis(150),
        ..Default::default()
    })
    .await;
    let info = fw_info(&ctl).await;
    assert_eq!(
        (info.state, info.fw_version, info.max_block),
        (fw_state::IDLE, 0x100, 224)
    );

    let img = test_image(5000, 0x0002_0300, 0b1, "2.3.0");
    assert_ack(
        &ctl.set(pids::PID_FW_BEGIN, img.desc.encode().to_vec())
            .await,
    );
    let block = FwWrite::max_block();
    let mut off = 0;
    while off < img.bin.len() {
        let end = (off + block).min(img.bin.len());
        assert_ack(&fw_write(&ctl, off, &img.bin[off..end]).await);
        if off == block {
            // retransmission of an earlier block is ACKed without writing
            assert_ack(&fw_write(&ctl, 0, &img.bin[..block]).await);
            // gap
            assert_nack(
                &fw_write(&ctl, end + 10, &[0; 4]).await,
                NackReason::DATA_OUT_OF_RANGE,
            );
        }
        off = end;
    }
    let info = fw_info(&ctl).await;
    assert_eq!(
        (info.state, info.received, info.total),
        (fw_state::RECEIVING, 5000, 5000)
    );
    assert_ack(&ctl.set(pids::PID_FW_FINISH, vec![]).await);

    // silent during the reboot
    tokio::time::sleep(Duration::from_millis(30)).await;
    assert!(ctl.node.snapshot().rebooting);
    let req = ctl.request(ctl.node.uid(), cc::GET, p::DEVICE_INFO, vec![]);
    assert!(ctl.rdm_at(pa(1), req).await.is_none());
    tokio::time::sleep(Duration::from_millis(150)).await;
    let di = DeviceInfo::decode(&ctl.get(p::DEVICE_INFO, vec![]).await.parameter_data).unwrap();
    assert_eq!(di.software_version_id, 0x0002_0300);
    let label = ctl.get(p::SOFTWARE_VERSION_LABEL, vec![]).await;
    assert_eq!(decode_label(&label.parameter_data), "2.3.0");
    let rs = ctl.poll_replies(ArtPoll::default()).await;
    assert_eq!(rs[0].vers_info, 0x0203);
    assert_eq!(fw_info(&ctl).await.state, fw_state::IDLE);
    assert_eq!(ctl.node.snapshot().boot_count, 1);
}

#[tokio::test]
async fn rdm_firmware_update_errors() {
    let ctl = Ctl::start(SimConfig::default()).await;
    // wrong hardware revision (only rev 1 supported)
    let img = test_image(1000, 0x0002_0300, 0b10, "2.3.0");
    assert_nack(
        &ctl.set(pids::PID_FW_BEGIN, img.desc.encode().to_vec())
            .await,
        NackReason::DATA_OUT_OF_RANGE,
    );
    let info = fw_info(&ctl).await;
    assert_eq!(
        (info.state, info.last_error),
        (fw_state::FAILED, fw_error::DESC_HW_REV)
    );
    // write without session
    assert_nack(
        &fw_write(&ctl, 0, &[1, 2]).await,
        NackReason::DATA_OUT_OF_RANGE,
    );

    // CRC error: a byte changed after the description block
    let img = test_image(1000, 0x0002_0300, 0b1, "2.3.0");
    let mut bad = img.bin.clone();
    bad[900] ^= 0xFF;
    assert_ack(
        &ctl.set(pids::PID_FW_BEGIN, img.desc.encode().to_vec())
            .await,
    );
    for (k, chunk) in bad.chunks(FwWrite::max_block()).enumerate() {
        assert_ack(&fw_write(&ctl, k * FwWrite::max_block(), chunk).await);
    }
    assert_nack(
        &ctl.set(pids::PID_FW_FINISH, vec![]).await,
        NackReason::DATA_OUT_OF_RANGE,
    );
    let info = fw_info(&ctl).await;
    assert_eq!(
        (info.state, info.last_error),
        (fw_state::FAILED, fw_error::CRC)
    );

    // description block inside the image differs from the announced copy
    let other = test_image(1000, 0x0002_0400, 0b1, "2.4.0");
    assert_ack(
        &ctl.set(pids::PID_FW_BEGIN, other.desc.encode().to_vec())
            .await,
    );
    assert_ack(&fw_write(&ctl, 0, &img.bin[..224]).await);
    assert_nack(
        &fw_write(&ctl, 224, &img.bin[224..448]).await,
        NackReason::DATA_OUT_OF_RANGE,
    );
    assert_eq!(fw_info(&ctl).await.last_error, fw_error::DESC_MISMATCH);
    assert_eq!(ctl.node.snapshot().fw_version, 0x100);
}

async fn send_alf(ctl: &Ctl, alf: &[u8], words: u32) -> Vec<u8> {
    let blocks: Vec<&[u8]> = alf.chunks(ArtFirmwareMaster::BLOCK_SIZE).collect();
    let mut kinds = Vec::new();
    for (k, data) in blocks.iter().enumerate() {
        let kind = if k == 0 {
            ArtFirmwareMaster::FIRM_FIRST
        } else if k + 1 == blocks.len() {
            ArtFirmwareMaster::FIRM_LAST
        } else {
            ArtFirmwareMaster::FIRM_CONT
        };
        let pkt = ArtFirmwareMaster::new(kind, k as u8, words, data.to_vec()).unwrap();
        ctl.send(&pkt.encode()).await;
        let r = ctl
            .wait_for(WAIT, |p| match p {
                ArtPacket::FirmwareReply(r) => Some(r.kind),
                _ => None,
            })
            .await
            .expect("ArtFirmwareReply");
        kinds.push(r);
        if r == ArtFirmwareReply::FIRM_FAIL {
            break;
        }
    }
    kinds
}

#[tokio::test]
async fn art_firmware_master_update() {
    let ctl = Ctl::start(SimConfig {
        reboot_delay: Duration::from_millis(50),
        ..Default::default()
    })
    .await;
    let img = test_image(4097, 0x0003_0001, 0b1, "3.0.1");
    let alf = img.to_alf();
    let kinds = send_alf(&ctl, &alf, img.alf_firmware_length()).await;
    assert_eq!(kinds.len(), alf.len().div_ceil(1024));
    assert!(kinds[..kinds.len() - 1]
        .iter()
        .all(|&r| r == ArtFirmwareReply::FIRM_BLOCK_GOOD));
    assert_eq!(kinds.last(), Some(&ArtFirmwareReply::FIRM_ALL_GOOD));
    tokio::time::sleep(Duration::from_millis(120)).await;
    let snap = ctl.node.snapshot();
    assert_eq!(
        (snap.fw_version, snap.version_label.as_str()),
        (0x0003_0001, "3.0.1")
    );
    assert!(!snap.rebooting);

    // OEM code not in the list
    let mut alf = img.to_alf();
    alf[ALF_OEM_OFFSET..ALF_OEM_OFFSET + 2].copy_from_slice(&0x1234u16.to_be_bytes());
    let kinds = send_alf(&ctl, &alf, img.alf_firmware_length()).await;
    assert_eq!(kinds, vec![ArtFirmwareReply::FIRM_FAIL]);
    let info = fw_info(&ctl).await;
    assert_eq!(
        (info.state, info.last_error),
        (fw_state::FAILED, fw_error::OEM)
    );

    // wrong BlockId
    let alf = img.to_alf();
    let words = img.alf_firmware_length();
    let first = ArtFirmwareMaster::new(
        ArtFirmwareMaster::FIRM_FIRST,
        0,
        words,
        alf[..1024].to_vec(),
    )
    .unwrap();
    ctl.send(&first.encode()).await;
    let cont = ArtFirmwareMaster::new(
        ArtFirmwareMaster::FIRM_CONT,
        5,
        words,
        alf[1024..2048].to_vec(),
    )
    .unwrap();
    ctl.send(&cont.encode()).await;
    let kinds: Vec<u8> = ctl
        .collect(Duration::from_millis(150))
        .await
        .into_iter()
        .filter_map(|p| match p {
            ArtPacket::FirmwareReply(r) => Some(r.kind),
            _ => None,
        })
        .collect();
    assert_eq!(
        kinds,
        vec![
            ArtFirmwareReply::FIRM_BLOCK_GOOD,
            ArtFirmwareReply::FIRM_FAIL
        ]
    );
    assert_eq!(fw_info(&ctl).await.last_error, fw_error::SEQUENCE);
}

#[tokio::test]
async fn one_firmware_session_at_a_time() {
    let ctl = Ctl::start(SimConfig::default()).await;
    let other = Ctl::attach(&ctl.node).await;
    let img = test_image(3000, 0x0002_0000, 0b1, "2.0.0");
    assert_ack(
        &ctl.set(pids::PID_FW_BEGIN, img.desc.encode().to_vec())
            .await,
    );
    let kinds = send_alf(&other, &img.to_alf(), img.alf_firmware_length()).await;
    assert_eq!(kinds, vec![ArtFirmwareReply::FIRM_FAIL]);
    assert_eq!(fw_info(&ctl).await.state, fw_state::RECEIVING);
}

#[test]
fn handle_is_send_sync() {
    fn check<T: Send + Sync + Clone>() {}
    check::<SimNode>();
}
