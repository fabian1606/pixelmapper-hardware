//! Pixelmapper node configuration.
//!
//! Will hold: reading node state from ArtPollReply (`protocol/artnet4-digest.md` §2.2) and changing it with
//! ArtAddress (names, Port-Addresses, merge mode, commands; §4.1) and ArtIpProg (IP/DHCP; §4.3-4.4), within
//! the node settings defined in `protocol/contract.md`.
