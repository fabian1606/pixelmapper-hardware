//! The sending side of one client session: a bounded, ordered outbound queue, the close signal shared by the
//! session task, the hub, command tasks and the transport, and [`OutgoingRx`], the end a transport drains.
//!
//! Slow-client policy: everything for a session goes through its bounded queue with `try_send`. When the queue is
//! full the session is closed with code 4003 instead of blocking the hub or dropping single messages (a dropped
//! `node`/`nodeGone` would silently corrupt the app's state; after a reconnect the app re-reads the snapshot).
//! Other sessions are never affected.

use std::sync::Arc;
use tokio::sync::{mpsc, watch};

// ── Close codes ───────────────────────────────────────────────────────────────────────────────────────────────────

/// Client protocol major differs (contract §1).
pub const CLOSE_INCOMPATIBLE: u16 = 4001;
/// No client `hello` within the deadline (contract §1).
pub const CLOSE_NO_HELLO: u16 = 4002;
/// The client did not read its messages fast enough (outbound queue full).
pub const CLOSE_TOO_SLOW: u16 = 4003;
/// Bridge shutting down.
pub const CLOSE_GOING_AWAY: u16 = 1001;

// ── Messages ──────────────────────────────────────────────────────────────────────────────────────────────────────

/// A message from the bridge to one client, in transport-neutral form. Over WebSocket a `Text` is a text frame and
/// a `Close` a close frame; an in-process transport forwards them as it likes. Nothing follows a `Close`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Outgoing {
    /// One JSON message of the contract (event, `result` or `error`).
    Text(String),
    /// The bridge ends the session with this close code (contract §1: 4001, 4002, 4003, 1001) and reason.
    Close {
        /// Close code.
        code: u16,
        /// Human-readable reason.
        reason: String,
    },
}

/// Why a session ends.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum CloseReason {
    /// Deliver a close with this code and reason (skipping anything still queued), then end.
    Code(u16, String),
    /// The client is gone or closed the session itself: nothing more to deliver.
    Gone,
}

// ── ConnTx ────────────────────────────────────────────────────────────────────────────────────────────────────────

/// Cloneable handle to send to one session.
#[derive(Debug, Clone)]
pub struct ConnTx {
    id: u64,
    tx: mpsc::Sender<Outgoing>,
    close: Arc<watch::Sender<Option<CloseReason>>>,
}

impl ConnTx {
    /// Session number (also the source of its ArtDmx `Physical`).
    pub fn id(&self) -> u64 {
        self.id
    }

    /// Queues a message in order. Returns false when the session is closed or closing; a full queue closes the
    /// session with 4003 (see module docs).
    pub fn send(&self, msg: Outgoing) -> bool {
        if self.close.borrow().is_some() {
            return false;
        }
        match self.tx.try_send(msg) {
            Ok(()) => true,
            Err(mpsc::error::TrySendError::Full(_)) => {
                tracing::warn!(conn = self.id, "outbound queue full, closing slow client");
                self.close_now(CloseReason::Code(CLOSE_TOO_SLOW, "client too slow".into()));
                false
            }
            Err(mpsc::error::TrySendError::Closed(_)) => false,
        }
    }

    /// Queues a JSON text message.
    pub fn send_text(&self, text: String) -> bool {
        self.send(Outgoing::Text(text))
    }

    /// Queues a close behind everything already queued (orderly close: 4001, 4002, 1001).
    pub fn close_after_queue(&self, code: u16, reason: &str) {
        self.send(Outgoing::Close {
            code,
            reason: reason.into(),
        });
    }

    /// Ends the session now, skipping the queue. Only the first reason counts.
    pub fn close_now(&self, reason: CloseReason) {
        self.close.send_if_modified(|current| {
            if current.is_none() {
                *current = Some(reason);
                true
            } else {
                false
            }
        });
    }

    /// True once the session is closed or closing.
    pub fn is_closed(&self) -> bool {
        self.close.borrow().is_some()
    }

    /// A receiver that changes when the session is closed.
    pub fn closed(&self) -> watch::Receiver<Option<CloseReason>> {
        self.close.subscribe()
    }
}

/// Creates the handle of a new session and the receiving end for its transport.
pub(crate) fn channel(id: u64, capacity: usize) -> (ConnTx, OutgoingRx) {
    let (tx, rx) = mpsc::channel(capacity.max(1));
    let (close, close_rx) = watch::channel(None);
    let conn = ConnTx {
        id,
        tx,
        close: Arc::new(close),
    };
    let receiver = OutgoingRx {
        conn: conn.clone(),
        rx,
        close: close_rx,
        done: false,
    };
    (conn, receiver)
}

// ── OutgoingRx ────────────────────────────────────────────────────────────────────────────────────────────────────

/// The receiving end of a session's outbound queue, drained by its transport.
///
/// [`recv`](Self::recv) yields the queued messages in order. When the session ends it yields at most one
/// [`Outgoing::Close`] (a queued orderly close, or the code of an immediate close such as 4003, which skips the
/// rest of the queue) and then `None` forever. A session the client closed itself ends with `None` directly.
/// Dropping the receiver closes the session (the transport is gone).
#[derive(Debug)]
pub struct OutgoingRx {
    conn: ConnTx,
    rx: mpsc::Receiver<Outgoing>,
    close: watch::Receiver<Option<CloseReason>>,
    done: bool,
}

impl OutgoingRx {
    /// Session number this receiver belongs to.
    pub fn id(&self) -> u64 {
        self.conn.id()
    }

    /// The next message for the client; `None` once the session has ended (see type docs).
    pub async fn recv(&mut self) -> Option<Outgoing> {
        if self.done {
            return None;
        }
        loop {
            // An immediate close wins over anything still queued.
            if let Some(reason) = self.close.borrow_and_update().clone() {
                self.done = true;
                return match reason {
                    CloseReason::Code(code, reason) => Some(Outgoing::Close { code, reason }),
                    CloseReason::Gone => None,
                };
            }
            tokio::select! {
                biased;
                changed = self.close.changed() => {
                    if changed.is_err() {
                        self.done = true;
                        return None;
                    }
                }
                msg = self.rx.recv() => match msg {
                    Some(Outgoing::Text(text)) => return Some(Outgoing::Text(text)),
                    Some(close @ Outgoing::Close { .. }) => {
                        // Orderly close: deliver it, then the session is over.
                        self.done = true;
                        self.conn.close_now(CloseReason::Gone);
                        return Some(close);
                    }
                    None => {
                        self.done = true;
                        return None;
                    }
                },
            }
        }
    }

    /// Resolves once the session is closed or closing (lets a transport abort a stuck send).
    pub async fn closed(&self) {
        let mut close = self.conn.closed();
        let _ = close.wait_for(Option::is_some).await;
    }
}

impl Drop for OutgoingRx {
    fn drop(&mut self) {
        self.conn.close_now(CloseReason::Gone);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn orderly_close_follows_the_queue() {
        let (conn, mut rx) = channel(1, 8);
        assert!(conn.send_text("a".into()));
        conn.close_after_queue(CLOSE_NO_HELLO, "no hello");
        assert_eq!(rx.recv().await, Some(Outgoing::Text("a".into())));
        assert_eq!(
            rx.recv().await,
            Some(Outgoing::Close {
                code: CLOSE_NO_HELLO,
                reason: "no hello".into()
            })
        );
        assert_eq!(rx.recv().await, None);
        assert!(conn.is_closed());
        assert!(!conn.send_text("late".into()));
    }

    #[tokio::test]
    async fn full_queue_closes_with_4003_skipping_the_rest() {
        let (conn, mut rx) = channel(1, 2);
        assert!(conn.send_text("a".into()));
        assert!(conn.send_text("b".into()));
        assert!(!conn.send_text("c".into()));
        assert_eq!(
            rx.recv().await,
            Some(Outgoing::Close {
                code: CLOSE_TOO_SLOW,
                reason: "client too slow".into()
            })
        );
        assert_eq!(rx.recv().await, None);
    }

    #[tokio::test]
    async fn gone_ends_without_close_and_drop_closes() {
        let (conn, mut rx) = channel(1, 2);
        conn.close_now(CloseReason::Gone);
        assert_eq!(rx.recv().await, None);
        let (conn, rx) = channel(2, 2);
        drop(rx);
        assert!(conn.is_closed());
    }
}
