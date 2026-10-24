//! RDM Table of Devices handling via Art-Net.
//!
//! Will hold: ArtTodRequest / ArtTodData / ArtTodControl exchange and TOD bookkeeping per Port-Address
//! (`protocol/artnet4-digest.md` §5.1-5.3), plus ArtRdm transport of RDM messages to tubes (§5.4).
