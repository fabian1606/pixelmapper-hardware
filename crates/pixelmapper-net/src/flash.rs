//! Firmware update flows.
//!
//! [`tube`] updates LED tubes over RDM through the node, [`node`] updates the node itself. Image formats and
//! the update sequence are defined in `protocol/contract.md` and `protocol/image-format.md`.

pub mod node;
pub mod tube;
