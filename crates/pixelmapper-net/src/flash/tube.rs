//! Tube firmware update over RDM.
//!
//! Will hold: entering the tube bootloader, reading BOOT_INFO ([`crate::pids::PID_BOOT_INFO`]), transferring
//! the image in RDM-sized blocks, verification and the `--rescue` path for tubes stuck in the bootloader;
//! see `protocol/contract.md` and `protocol/rdm-digest.md` §8.
