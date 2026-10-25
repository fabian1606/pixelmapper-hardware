//! RDM over ArtRdm: transaction matching, per-port flow control (max in flight, FifoMax/FifoAvail), timeout and
//! retry, ACK_OVERFLOW, ACK_TIMER (via QUEUED_MESSAGE) and automatic fetching of queued messages.

use super::{lock, Inner, RdmConfig, RdmRequest};
use crate::artnet::{ArtPacket, ArtRdm, PortAddress};
use crate::error::{Error, Result};
use crate::event::{Event, QueuedMessage};
use crate::rdm::pd::StatusMessage;
use crate::rdm::{cc, pid, status_type, NackReason, RdmMessage, ResponseType, Uid, MAX_PDL};
use crate::state::NodeId;
use std::collections::HashMap;
use std::net::SocketAddr;
use std::sync::atomic::{AtomicBool, AtomicU8, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};
use tokio::sync::{oneshot, Semaphore};

type PendKey = (SocketAddr, u16, Uid, u8);
type UidKey = (SocketAddr, u16, Uid);

struct PortFlow {
    sem: Semaphore,
    limit: AtomicUsize,
    fifo_avail: AtomicU8,
    fifo_max: AtomicU8,
}

pub(crate) struct RdmEngine {
    uid: Uid,
    cfg: RdmConfig,
    tn: AtomicU8,
    closed: AtomicBool,
    pending: Mutex<HashMap<PendKey, oneshot::Sender<RdmMessage>>>,
    ports: Mutex<HashMap<(SocketAddr, u16), Arc<PortFlow>>>,
    queue_locks: Mutex<HashMap<UidKey, Arc<tokio::sync::Mutex<()>>>>,
    timer_waiters: Mutex<Vec<(UidKey, u16, oneshot::Sender<RdmMessage>)>>,
}

impl RdmEngine {
    pub(crate) fn new(uid: Uid, cfg: RdmConfig) -> Self {
        Self {
            uid,
            cfg,
            tn: AtomicU8::new(0),
            closed: AtomicBool::new(false),
            pending: Mutex::new(HashMap::new()),
            ports: Mutex::new(HashMap::new()),
            queue_locks: Mutex::new(HashMap::new()),
            timer_waiters: Mutex::new(Vec::new()),
        }
    }

    pub(crate) fn close(&self) {
        self.closed.store(true, Ordering::Relaxed);
        lock(&self.pending).clear();
        lock(&self.timer_waiters).clear();
    }

    fn flow(&self, addr: SocketAddr, pa: PortAddress) -> Arc<PortFlow> {
        lock(&self.ports)
            .entry((addr, pa.raw()))
            .or_insert_with(|| {
                let n = self.cfg.max_inflight_per_port.max(1);
                Arc::new(PortFlow {
                    sem: Semaphore::new(n),
                    limit: AtomicUsize::new(n),
                    fifo_avail: AtomicU8::new(0),
                    fifo_max: AtomicU8::new(0),
                })
            })
            .clone()
    }

    fn queue_lock(&self, key: UidKey) -> Arc<tokio::sync::Mutex<()>> {
        lock(&self.queue_locks).entry(key).or_default().clone()
    }

    /// Called by the receive loop for every ArtRdm.
    pub(crate) fn on_response(&self, r: ArtRdm, src: SocketAddr) {
        let pa = r.port_address();
        if let Some(flow) = lock(&self.ports).get(&(src, pa.raw())).cloned() {
            flow.fifo_avail.store(r.fifo_avail, Ordering::Relaxed);
            flow.fifo_max.store(r.fifo_max, Ordering::Relaxed);
            // Shrink our window to the node's queue size (never below 1).
            let max = usize::from(r.fifo_max);
            let cur = flow.limit.load(Ordering::Relaxed);
            if max > 0 && max < cur {
                let forgot = flow.sem.forget_permits(cur - max);
                flow.limit.fetch_sub(forgot, Ordering::Relaxed);
            }
        }
        let Ok(msg) = r.message() else {
            return;
        };
        if !msg.is_response() || msg.dest_uid != self.uid {
            return;
        }
        let key = (src, pa.raw(), msg.src_uid, msg.transaction_number);
        if let Some(tx) = lock(&self.pending).remove(&key) {
            let _ = tx.send(msg);
        }
    }
}

/// One request/response exchange with timeout + retries; `Ok(None)` for broadcast/vendorcast destinations.
#[allow(clippy::too_many_arguments)]
async fn transact(
    inner: &Inner,
    addr: SocketAddr,
    pa: PortAddress,
    uid: Uid,
    command_class: u8,
    pid: u16,
    data: &[u8],
    sub_device: u16,
    timeout: Duration,
    retries: u32,
) -> Result<Option<RdmMessage>> {
    let eng = &inner.rdm;
    if eng.closed.load(Ordering::Relaxed) {
        return Err(Error::Closed);
    }
    if data.len() > MAX_PDL {
        return Err(Error::InvalidArgument(format!(
            "parameter data {} > {MAX_PDL} bytes",
            data.len()
        )));
    }
    let flow = eng.flow(addr, pa);
    let _permit = flow.sem.acquire().await.map_err(|_| Error::Closed)?;
    let tn = eng.tn.fetch_add(1, Ordering::Relaxed).wrapping_add(1);
    let msg = RdmMessage::request(
        uid,
        eng.uid,
        tn,
        1,
        sub_device,
        command_class,
        pid,
        data.to_vec(),
    );
    let pkt = ArtPacket::Rdm(ArtRdm::from_message(pa, &msg)).encode();
    if uid.is_broadcast() {
        inner.send_raw(&pkt, addr).await?;
        return Ok(None);
    }
    let key = (addr, pa.raw(), uid, tn);
    for _ in 0..=retries {
        // The node reported a full queue: give it a moment before adding more.
        if flow.fifo_max.load(Ordering::Relaxed) > 0 && flow.fifo_avail.load(Ordering::Relaxed) == 0
        {
            tokio::time::sleep(Duration::from_millis(5)).await;
        }
        let (tx, rx) = oneshot::channel();
        lock(&eng.pending).insert(key, tx);
        inner.send_raw(&pkt, addr).await?;
        match tokio::time::timeout(timeout, rx).await {
            Ok(Ok(m)) => return Ok(Some(m)),
            Ok(Err(_)) => return Err(Error::Closed),
            Err(_) => {
                lock(&eng.pending).remove(&key);
            }
        }
    }
    Err(Error::Timeout {
        uid,
        pid,
        attempts: retries + 1,
    })
}

/// Full request: ACK_OVERFLOW concatenation, ACK_TIMER resolution, BUFFER_FULL retries, NACK → error, and
/// automatic queued-message fetching when Message Count > 0.
pub(crate) async fn request(
    inner: &Arc<Inner>,
    node: NodeId,
    addr: SocketAddr,
    pa: PortAddress,
    req: RdmRequest,
) -> Result<Vec<u8>> {
    let cfg = &inner.rdm.cfg;
    let timeout = req.options.timeout.unwrap_or(cfg.timeout);
    let retries = req.options.retries.unwrap_or(cfg.retries);
    let mut buf = Vec::new();
    let mut overflows = 0u32;
    let mut busy = 0u32;
    loop {
        let first = transact(
            inner,
            addr,
            pa,
            req.uid,
            req.command_class,
            req.pid,
            &req.data,
            req.options.sub_device,
            timeout,
            retries,
        )
        .await?;
        let Some(mut m) = first else {
            return Ok(Vec::new());
        };
        loop {
            maybe_drain(inner, node, addr, pa, req.uid, &m);
            if m.pid_mismatch(req.pid) {
                return Err(Error::UnexpectedResponse(format!(
                    "answer for PID 0x{:04X} to a request for 0x{:04X}",
                    m.parameter_id, req.pid
                )));
            }
            match m.response_type() {
                Some(ResponseType::Ack) => {
                    buf.extend_from_slice(&m.parameter_data);
                    return Ok(buf);
                }
                Some(ResponseType::AckOverflow) => {
                    buf.extend_from_slice(&m.parameter_data);
                    overflows += 1;
                    if overflows > cfg.max_overflow {
                        return Err(Error::UnexpectedResponse(format!(
                            "more than {} ACK_OVERFLOW responses",
                            cfg.max_overflow
                        )));
                    }
                    break; // repeat the same request
                }
                Some(ResponseType::AckTimer) => {
                    let delay = m.ack_timer_delay().unwrap_or(Duration::from_millis(100));
                    m = resolve_ack_timer(inner, node, addr, pa, &req, delay).await?;
                    continue;
                }
                Some(ResponseType::NackReason) => {
                    let reason = m.nack_reason().unwrap_or(NackReason(0xFFFF));
                    let full = reason == NackReason::BUFFER_FULL
                        || reason == NackReason::PROXY_BUFFER_FULL;
                    if full && busy < req.options.buffer_full_retries {
                        busy += 1;
                        tokio::time::sleep(req.options.buffer_full_delay).await;
                        break;
                    }
                    return Err(Error::Nack {
                        uid: req.uid,
                        pid: req.pid,
                        reason,
                    });
                }
                None => {
                    return Err(Error::UnexpectedResponse(format!(
                        "response type {}",
                        m.port_id_or_response_type
                    )))
                }
            }
        }
    }
}

trait PidCheck {
    fn pid_mismatch(&self, pid: u16) -> bool;
}

impl PidCheck for RdmMessage {
    fn pid_mismatch(&self, pid: u16) -> bool {
        self.parameter_id != pid
    }
}

async fn resolve_ack_timer(
    inner: &Arc<Inner>,
    node: NodeId,
    addr: SocketAddr,
    pa: PortAddress,
    req: &RdmRequest,
    first_delay: Duration,
) -> Result<RdmMessage> {
    let eng = &inner.rdm;
    let key = (addr, pa.raw(), req.uid);
    let (tx, mut rx) = oneshot::channel();
    lock(&eng.timer_waiters).push((key, req.pid, tx));
    let deadline = Instant::now() + eng.cfg.ack_timer_limit;
    let mut delay = first_delay.max(Duration::from_millis(10));
    loop {
        tokio::time::sleep(delay).await;
        drain_queue(inner, node, addr, pa, req.uid).await;
        if let Ok(m) = rx.try_recv() {
            return Ok(m);
        }
        if Instant::now() >= deadline {
            lock(&eng.timer_waiters).retain(|(k, p, _)| !(*k == key && *p == req.pid));
            return Err(Error::Timeout {
                uid: req.uid,
                pid: req.pid,
                attempts: 1,
            });
        }
        delay = Duration::from_millis(100);
    }
}

fn maybe_drain(
    inner: &Arc<Inner>,
    node: NodeId,
    addr: SocketAddr,
    pa: PortAddress,
    uid: Uid,
    m: &RdmMessage,
) {
    if m.message_count == 0
        || !inner.rdm.cfg.auto_fetch_queued
        || m.parameter_id == pid::QUEUED_MESSAGE
    {
        return;
    }
    let me = inner.clone();
    tokio::spawn(async move { drain_queue(&me, node, addr, pa, uid).await });
}

/// Fetch queued messages until the device reports none. Messages that answer a pending ACK_TIMER request go to
/// that request; all others become [`Event::QueuedMessage`]. Only one drain per device at a time.
pub(crate) async fn drain_queue(
    inner: &Arc<Inner>,
    node: NodeId,
    addr: SocketAddr,
    pa: PortAddress,
    uid: Uid,
) {
    let key = (addr, pa.raw(), uid);
    let qlock = inner.rdm.queue_lock(key);
    let _g = qlock.lock().await;
    let cfg = &inner.rdm.cfg;
    for _ in 0..256 {
        let r = transact(
            inner,
            addr,
            pa,
            uid,
            cc::GET,
            pid::QUEUED_MESSAGE,
            &[status_type::ADVISORY],
            0,
            cfg.timeout,
            cfg.retries,
        )
        .await;
        let Ok(Some(m)) = r else {
            return;
        };
        match m.response_type() {
            Some(ResponseType::Ack) | Some(ResponseType::NackReason) => {}
            _ => return,
        }
        let more = m.message_count > 0;
        if m.parameter_id == pid::STATUS_MESSAGES && m.response_type() == Some(ResponseType::Ack) {
            let list = StatusMessage::decode_list(&m.parameter_data).unwrap_or_default();
            if list.is_empty() {
                return;
            }
            emit_queued(inner, node, pa, uid, &m, Some(list));
        } else if m.parameter_id == pid::QUEUED_MESSAGE {
            return; // NACK to QUEUED_MESSAGE itself: not supported
        } else {
            let waiter = {
                let mut w = lock(&inner.rdm.timer_waiters);
                w.iter()
                    .position(|(k, p, _)| *k == key && *p == m.parameter_id)
                    .map(|i| w.remove(i))
            };
            match waiter {
                Some((_, _, tx)) => {
                    let _ = tx.send(m);
                }
                None => emit_queued(inner, node, pa, uid, &m, None),
            }
        }
        if !more {
            return;
        }
    }
}

fn emit_queued(
    inner: &Inner,
    node: NodeId,
    pa: PortAddress,
    uid: Uid,
    m: &RdmMessage,
    status: Option<Vec<StatusMessage>>,
) {
    inner.emit(Event::QueuedMessage {
        node,
        port_address: pa,
        uid,
        message: QueuedMessage {
            pid: m.parameter_id,
            pid_name: pid::name(m.parameter_id),
            command_class: m.command_class,
            sub_device: m.sub_device,
            data: m.parameter_data.clone(),
            status_messages: status,
        },
    });
}
