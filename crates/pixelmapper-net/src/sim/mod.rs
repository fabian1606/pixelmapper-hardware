//! Simulated Pixelmapper node: speaks Art-Net 4 over real UDP (localhost or any address), with simulated
//! ports, tubes (app + bootloader) and third-party RDM devices. Used by the integration tests and `pmnet sim`.

pub mod fixture;
pub mod node;
mod node_fw;
mod node_rdm;
pub mod responder;
pub mod tube;

pub use fixture::{FixtureConfig, SimFixture};
pub use node::{
    SimConfig, SimDirection, SimDmxFrame, SimIndicators, SimNode, SimPortConfig, SimPortSnapshot,
    SimSnapshot,
};
pub use responder::SimResponder;
pub use tube::{SimTube, TubeMode};

#[cfg(test)]
mod node_tests;
