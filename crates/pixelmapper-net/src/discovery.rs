//! Node and tube discovery.
//!
//! Will hold: Art-Net node discovery (broadcast ArtPoll every 2.5-3 s, collect ArtPollReply, 3 s timeout,
//! targeted mode; `protocol/artnet4-digest.md` §2) and the RDM binary-search discovery (DISC_UNIQUE_BRANCH /
//! DISC_MUTE / DISC_UN_MUTE, collision handling as in OLA's DiscoveryAgent; `protocol/rdm-digest.md` §3),
//! as used via the node per `protocol/contract.md`.
