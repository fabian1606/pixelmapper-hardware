//! The bridge WebSocket contract (`protocol/bridge-ws.md`) as Rust types: identifiers, decoded state, events,
//! commands, results, error codes and the binary DMX frame set. Nothing here touches the network.

pub mod error;
pub mod events;
pub mod frame;
pub mod ids;
pub mod messages;
pub mod model;

pub use error::{nack_name, CommandError, ErrorCode};
pub use events::{Diag, DiagPriority, Event, HelloEvent, LogEntry, LogLevel, RdmQueued};
pub use ids::{HexData, MacAddress, NodeId, PortRef, RdmUid, MAX_PORT_ADDRESS};
pub use messages::{
    ArtAddressArgs, ArtAddressPort, IpProgArgs, IpProgReply, OutputProtocol, SetDirection,
    SetIndicator, TodAction, PROTOCOL_VERSION,
};
pub use model::*;
