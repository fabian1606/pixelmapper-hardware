//! **TODO — adapter onto `crates/pixelmapper-net`** (`PixelmapperNetBackend`).
//!
//! Not implemented yet: `pixelmapper-net` is being rewritten (branch `phase1/rust`) and its API is unstable; on
//! `main` it has codecs only (ArtPoll, ArtDmx, RDM framing) and no network engine. This module documents how
//! [`NetBackend`](super::NetBackend) maps onto the library so the adapter is mechanical once the API is announced.
//! When it lands: add `pixelmapper-net = { path = "../pixelmapper-net" }` to this crate, implement the trait here,
//! and make `serve` use it instead of [`OfflineBackend`](super::offline::OfflineBackend) (keep `--mock`).
//!
//! # Mapping
//!
//! | `NetBackend` | pixelmapper-net (expected) | Contract |
//! |---|---|---|
//! | `status` | interface enumeration + UDP 6454 socket state (`SO_REUSEADDR`/`SO_REUSEPORT`); emit `Event::BridgeStatus` on change | §4 Interface, UDP; §5 `hello`, `bridgeStatus` |
//! | `snapshot`, `subscribe` | own ArtPollReply excluded; one ArtTodRequest per port when a node appears/reappears; node table keyed by `DefaultRespUID` → MAC → IP, replies merged across `BindIndex` (node fields from the lowest `BindIndex`), IP changes followed; TOD assembler (`UidTotal`/`BlockCount`, max 200 UIDs per packet, TodNak → `available: false`); `nodeGone` after 3 silent poll cycles | §3 `nodeId`; §4 `NodeState`, `Tod`; §5 `node`, `nodeGone`, `tod` |
//! | `poll_now` | ArtPoll with Flags bit 1 to every interface's subnet broadcast and to 2.255.255.255 when an interface is in 2.0.0.0/8 (never 255.255.255.255); cycle 2.5–3 s | §6 `poll`; §9 |
//! | `set_poll_targets` | additional unicast ArtPoll destinations | §6 `addPollTarget` |
//! | `tod_request` | ArtTodRequest with Net + Address of the port's output Port-Address (also subscribes to unsolicited ArtTodData; send once per port after a node appears and after it rebooted) | §6 `todRequest`; node contract §6.1 |
//! | `tod_control` | ArtTodControl `AtcFlush`/`AtcEnd`/`AtcIncOn`/`AtcIncOff` (0x01–0x04) | §6 `todControl` |
//! | `rdm_get`, `rdm_set` | ArtRdm without 0xCC start code, per-port transaction queue (≤ 4 in flight and ≤ `FifoAvail`), 500 ms timeout, 2 retries, ACK_TIMER wait + fetch, ACK_OVERFLOW reassembly, QUEUED_MESSAGE fetch when the message count is > 0 → `Event::RdmQueued`; value codec: built-in PIDs (contract §4 `RdmValue` table) + PARAMETER_DESCRIPTION cache for manufacturer PIDs; NACK → `BackendError::Nack(reason)` | §4 `RdmValue`; §6 Semantics; node contract §6.2 |
//! | `rdm_describe` | DEVICE_INFO, SUPPORTED_PARAMETERS, labels, SOFTWARE/BOOT versions, DMX_PERSONALITY_DESCRIPTION ×n, SLOT_INFO/SLOT_DESCRIPTION/DEFAULT_SLOT_VALUE, PARAMETER_DESCRIPTION for 0x8000–0xFFDF; cache per (`uid`, `softwareVersionId`) | §4 `RdmDeviceDescription`; §6 Semantics |
//! | `art_address` | one ArtAddress per Command, addressed by `BindIndex`; names and addresses with the first packet of each `BindIndex`; programming rules (bit 7 = write, 0x7F = no change); wait for the ArtPollReply (1 s, one resend); emit `Event::Node` before returning | §6 `artAddress`; node contract §5 |
//! | `ip_prog` | ArtIpProg → ArtIpProgReply (1 s, one resend) | §6 `ipProg` |
//! | `art_input` | ArtInput for one `BindIndex` → ArtPollReply | §6 `artInput` |
//! | `image_info`, `flash_start`, `flash_cancel`, `flash_jobs` | tube image reader (Intel HEX + 16-byte trailer at 0x17F0) and the M2.2 update procedure (BOOT → BOOT_INFO → pages → APP_CRC → FINISH → verify), one job per (node, port), progress ≤ 10/s | §4 Firmware; §6 `flash`; node contract §9.1 |
//! | `send_dmx` | ArtDmx unicast, even length (pad), sequence 1–255 per (IP, Port-Address), `Physical` as given (per connection); one ArtSync **broadcast** per set on every interface that carried ArtDmx of it, when `sync` | §7; node contract §4 |
//! | events | ArtDiagData → `Event::Diag`; bind errors, malformed packets, retries exhausted → `Event::Log` | §5 `diag`, `log` |
//!
//! The server already validates node/port existence, the BindIndex Net/Sub-Net rule of `artAddress`
//! ([`validate_art_address`](crate::server::validate::validate_art_address)), argument shapes and ranges, so the
//! adapter only reports what the network says (`Timeout`, `Nack`, `BadResponse`, …).
