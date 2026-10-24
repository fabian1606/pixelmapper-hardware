//! Firmware image formats (`protocol/image-format.md`): tube Intel HEX with trailer, node ESP-IDF binary with
//! description block and the Art-Net `.alf` container.

pub mod ihex;
pub mod node;
pub mod tube;

pub use node::{NodeImage, NodeImageDesc, NodeImageInfo};
pub use tube::{TrailerInfo, TubeImage};
