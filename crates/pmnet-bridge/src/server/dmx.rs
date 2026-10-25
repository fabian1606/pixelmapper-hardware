//! Per-connection DMX routing (contract §6 DmxRoute, §7): the route table, rate limiting to the node's
//! `RefreshRate` with coalescing per route (latest data wins), and the 1 s keep-alive refresh of idle routes.
//! Pure state machine over explicit `Instant`s; the connection task drives it and hands batches to the backend.

use crate::backend::{DmxFrame, DmxTarget};
use crate::protocol::frame::FrameSet;
use crate::protocol::DmxRoute;
use bytes::Bytes;
use std::collections::HashMap;
use std::time::{Duration, Instant};

/// ArtDmx rate when a node reports `RefreshRate` 0, or for manual `ip` routes (contract §7 Rate).
pub const DEFAULT_REFRESH_RATE: u16 = 44;

/// Minimum interval between two ArtDmx of one route for a refresh rate in Hz (0 → 44 Hz).
pub fn interval_for_rate(rate: u16) -> Duration {
    let rate = if rate == 0 {
        DEFAULT_REFRESH_RATE
    } else {
        rate
    };
    Duration::from_secs_f64(1.0 / f64::from(rate))
}

/// One `send_dmx` call.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Batch {
    /// Frames to send.
    pub frames: Vec<DmxFrame>,
    /// Send ArtSync after them.
    pub sync: bool,
}

/// State of one route.
#[derive(Debug, Clone)]
struct Route {
    target: DmxTarget,
    port_address: u16,
    /// Minimum interval between ArtDmx (from the node's RefreshRate, refreshed with every frame set).
    interval: Duration,
    /// Last data handed to the backend, and whether its set asked for ArtSync (for the refresh).
    last: Option<(Bytes, bool)>,
    /// When the last ArtDmx of this route went out.
    sent_at: Option<Instant>,
    /// Newer data held back by the rate limit (latest wins).
    pending: Option<(Bytes, bool)>,
}

impl Route {
    /// Frame for `data` to this route's destination.
    fn frame(&self, data: Bytes) -> DmxFrame {
        DmxFrame {
            target: self.target.clone(),
            port_address: self.port_address,
            data,
        }
    }

    /// True when the rate limit allows sending at `now`.
    fn may_send(&self, now: Instant) -> bool {
        self.sent_at.is_none_or(|t| now >= t + self.interval)
    }
}

/// Route table and send scheduling of one connection.
#[derive(Debug)]
pub struct DmxRouter {
    routes: HashMap<u16, Route>,
    refresh: Duration,
}

impl DmxRouter {
    /// Empty table; idle routes are re-sent every `refresh`.
    pub fn new(refresh: Duration) -> Self {
        Self {
            routes: HashMap::new(),
            refresh,
        }
    }

    /// Number of routes.
    pub fn len(&self) -> usize {
        self.routes.len()
    }

    /// True without routes.
    pub fn is_empty(&self) -> bool {
        self.routes.is_empty()
    }

    /// Replaces the table (already validated). Routes whose id, destination and Port-Address are unchanged keep
    /// their last data and timing; all others start fresh.
    pub fn set_routes(&mut self, routes: &[DmxRoute]) {
        let mut old = std::mem::take(&mut self.routes);
        for r in routes {
            let target = match (&r.node_id, r.ip) {
                (Some(node), _) => DmxTarget::Node(node.clone()),
                (None, Some(ip)) => DmxTarget::Ip(ip),
                // Rejected by validation; skipped defensively.
                (None, None) => continue,
            };
            let kept = old
                .remove(&r.route)
                .filter(|o| o.target == target && o.port_address == r.port_address);
            let route = kept.unwrap_or(Route {
                target,
                port_address: r.port_address,
                interval: interval_for_rate(0),
                last: None,
                sent_at: None,
                pending: None,
            });
            self.routes.insert(r.route, route);
        }
    }

    /// Handles a frame set at `now`: entries of known routes that the rate limit allows go into the returned
    /// batch; faster ones are held as pending (latest wins) for `poll_due`. Unknown routes are skipped.
    /// `interval_of` gives the current minimum interval of a destination.
    pub fn on_frame_set(
        &mut self,
        set: FrameSet,
        now: Instant,
        interval_of: impl Fn(&DmxTarget) -> Duration,
    ) -> Option<Batch> {
        let mut frames = Vec::new();
        for entry in set.entries {
            let Some(route) = self.routes.get_mut(&entry.route) else {
                continue;
            };
            route.interval = interval_of(&route.target);
            if route.may_send(now) {
                frames.push(route.frame(entry.data.clone()));
                route.last = Some((entry.data, set.sync));
                route.sent_at = Some(now);
                route.pending = None;
            } else {
                route.pending = Some((entry.data, set.sync));
            }
        }
        (!frames.is_empty()).then_some(Batch {
            frames,
            sync: set.sync,
        })
    }

    /// Everything due at `now`: pending data whose interval elapsed, and the last data of routes idle for the
    /// refresh period. One batch; ArtSync when any of its data came from a set that asked for it.
    pub fn poll_due(&mut self, now: Instant) -> Option<Batch> {
        let mut frames = Vec::new();
        let mut sync = false;
        for route in self.routes.values_mut() {
            if let Some((data, s)) = route.pending.clone() {
                if route.may_send(now) {
                    frames.push(route.frame(data.clone()));
                    sync |= s;
                    route.last = Some((data, s));
                    route.sent_at = Some(now);
                    route.pending = None;
                }
            } else if let (Some((data, s)), Some(at)) = (route.last.clone(), route.sent_at) {
                if now >= at + self.refresh {
                    frames.push(route.frame(data));
                    sync |= s;
                    route.sent_at = Some(now);
                }
            }
        }
        (!frames.is_empty()).then_some(Batch { frames, sync })
    }

    /// The earliest instant at which `poll_due` has something to do, if any.
    pub fn next_deadline(&self) -> Option<Instant> {
        self.routes
            .values()
            .filter_map(|r| {
                let at = r.sent_at?;
                if r.pending.is_some() {
                    Some(at + r.interval)
                } else if r.last.is_some() {
                    Some(at + self.refresh)
                } else {
                    None
                }
            })
            .min()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::protocol::frame::FrameEntry;
    use crate::protocol::NodeId;
    use std::net::Ipv4Addr;

    fn routes() -> Vec<DmxRoute> {
        vec![
            DmxRoute {
                route: 1,
                node_id: Some(NodeId::from("n")),
                ip: None,
                port_address: 1,
            },
            DmxRoute {
                route: 2,
                node_id: None,
                ip: Some(Ipv4Addr::new(2, 0, 0, 9)),
                port_address: 2,
            },
        ]
    }

    fn set(entries: &[(u16, u8)], sync: bool) -> FrameSet {
        FrameSet {
            sync,
            entries: entries
                .iter()
                .map(|&(route, v)| FrameEntry {
                    route,
                    data: Bytes::from(vec![v; 4]),
                })
                .collect(),
        }
    }

    const TEN_HZ: Duration = Duration::from_millis(100);

    #[test]
    fn sends_and_skips_unknown_routes() {
        let mut r = DmxRouter::new(Duration::from_secs(1));
        r.set_routes(&routes());
        let t0 = Instant::now();
        let b = r
            .on_frame_set(set(&[(1, 1), (2, 2), (9, 9)], true), t0, |_| TEN_HZ)
            .unwrap();
        assert!(b.sync);
        assert_eq!(b.frames.len(), 2);
        assert_eq!(b.frames[0].target, DmxTarget::Node(NodeId::from("n")));
        assert_eq!(b.frames[0].port_address, 1);
        assert_eq!(b.frames[1].target, DmxTarget::Ip(Ipv4Addr::new(2, 0, 0, 9)));
        assert!(r
            .on_frame_set(set(&[(9, 9)], true), t0, |_| TEN_HZ)
            .is_none());
    }

    #[test]
    fn rate_limit_coalesces_latest() {
        let mut r = DmxRouter::new(Duration::from_secs(1));
        r.set_routes(&routes());
        let t0 = Instant::now();
        assert!(r
            .on_frame_set(set(&[(1, 1)], true), t0, |_| TEN_HZ)
            .is_some());
        // Two faster sets: held back, the later one wins.
        assert!(r
            .on_frame_set(set(&[(1, 2)], true), t0 + Duration::from_millis(30), |_| {
                TEN_HZ
            })
            .is_none());
        assert!(r
            .on_frame_set(
                set(&[(1, 3)], false),
                t0 + Duration::from_millis(60),
                |_| TEN_HZ
            )
            .is_none());
        assert_eq!(r.next_deadline(), Some(t0 + TEN_HZ));
        assert!(r.poll_due(t0 + Duration::from_millis(90)).is_none());
        let b = r.poll_due(t0 + TEN_HZ).unwrap();
        assert_eq!(b.frames.len(), 1);
        assert_eq!(b.frames[0].data[0], 3);
        assert!(!b.sync);
        assert!(r.poll_due(t0 + TEN_HZ).is_none());
    }

    #[test]
    fn idle_refresh_and_route_replacement() {
        let mut r = DmxRouter::new(Duration::from_secs(1));
        r.set_routes(&routes());
        let t0 = Instant::now();
        r.on_frame_set(set(&[(1, 7)], true), t0, |_| TEN_HZ);
        assert_eq!(r.next_deadline(), Some(t0 + Duration::from_secs(1)));
        assert!(r.poll_due(t0 + Duration::from_millis(999)).is_none());
        let b = r.poll_due(t0 + Duration::from_secs(1)).unwrap();
        assert_eq!((b.frames.len(), b.frames[0].data[0], b.sync), (1, 7, true));
        assert_eq!(r.next_deadline(), Some(t0 + Duration::from_secs(2)));

        // Same route definition keeps its state; a changed one starts fresh.
        r.set_routes(&routes());
        assert!(r.next_deadline().is_some());
        let mut moved = routes();
        moved[0].port_address = 5;
        r.set_routes(&moved);
        assert!(r.next_deadline().is_none());
        r.set_routes(&[]);
        assert!(r.is_empty());
    }

    #[test]
    fn rates() {
        assert_eq!(interval_for_rate(0), interval_for_rate(44));
        assert_eq!(interval_for_rate(10), TEN_HZ);
    }
}
