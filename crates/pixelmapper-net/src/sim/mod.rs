//! Simulated Pixelmapper node: speaks Art-Net 4 over real UDP (localhost or any address), with simulated
//! ports, tubes (app + bootloader) and third-party RDM devices. Used by the integration tests and `pmnet sim`.

pub mod fixture;
pub mod responder;
pub mod tube;

pub use responder::SimResponder;
