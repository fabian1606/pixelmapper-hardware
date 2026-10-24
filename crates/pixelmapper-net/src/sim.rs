//! Simulated devices for tests without hardware.
//!
//! Will hold: a simulated tube bootloader (RDM responder with DEVICE_INFO, BOOT_INFO and the flash PIDs from
//! [`crate::pids`]) so the [`crate::flash::tube`] flow can be tested end to end; behaviour per
//! `protocol/contract.md` and the tube vectors in `protocol/vectors/rdm/`.
