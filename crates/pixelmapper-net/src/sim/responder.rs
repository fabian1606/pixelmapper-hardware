//! The interface between the simulated node and the simulated RDM devices behind its ports.

use crate::rdm::{RdmMessage, Uid};
use std::any::Any;
use std::time::Instant;

/// A simulated RDM responder on a DMX line (tube app/bootloader, third-party fixture, ...).
///
/// The simulated node calls [`SimResponder::handle`] for every RDM request on the port whose destination is the
/// responder's UID, the all-devices broadcast or its vendorcast; the responder decides itself whether it answers
/// (never to broadcasts). Discovery is simulated by the node: every responder that is
/// [`SimResponder::discoverable`] is in the port's TOD.
pub trait SimResponder: Send + 'static {
    /// The responder's UID.
    fn uid(&self) -> Uid;

    /// Handle one request (CC GET/SET/DISCOVERY). `None` = no answer on the line.
    fn handle(&mut self, request: &RdmMessage, now: Instant) -> Option<RdmMessage>;

    /// Advance timers (bootloader timeout, ACK_TIMER completion, ...). Called at least every 10 ms.
    fn tick(&mut self, _now: Instant) {}

    /// Power was cut and restored at `now` (tube: boot window for the rescue).
    fn power_cycle(&mut self, _now: Instant) {}

    /// Whether the device currently answers discovery (powered, not mid-reset).
    fn discoverable(&self) -> bool {
        true
    }

    /// For inspection in tests (downcast to the concrete type).
    fn as_any(&self) -> &dyn Any;

    /// For manipulation in tests (downcast to the concrete type).
    fn as_any_mut(&mut self) -> &mut dyn Any;
}
