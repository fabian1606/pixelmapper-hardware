# Bridge WebSocket Protocol — `pmnet serve` ↔ Pixelmapper

Status: **1.0.0 candidate, revision 2**, 2026-10-26. **This file is the authoritative contract** (milestone M3.1 of the Art-Net/RDM plan, `pixelmapper/architecture/artnet-rdm-implementation-plan.md`). The copy in the Pixelmapper repo (`architecture/artnet-bridge-ws-protocol.md`) points here.

Implementations: the bridge is `crates/pmnet-bridge` (binary `pmnet-bridge serve` until the `pmnet` CLI embeds it as `pmnet serve`; Rust types in `src/protocol/`). The app side is the TypeScript mirror in the Pixelmapper repo, `extensions/connector-artnet/bridge/protocol-model.ts`, `protocol-messages.ts` and `frame-set.ts`. Every change here is made in both, and the other way round.

The bridge owns every Art-Net/RDM packet, timer and procedure (plan D2). Pixelmapper sends **intents** and receives **state**. All state follows the node contract's last-writer-wins rule (D5): events and results report what the network said, never what was requested.

“Node contract” below = [`contract.md`](contract.md) in this folder (with [`artnet4-digest.md`](artnet4-digest.md), [`rdm-digest.md`](rdm-digest.md), [`pids.toml`](pids.toml)). “Plan” = the Art-Net/RDM implementation plan in the Pixelmapper repo.

**Changelog** (the wire version stays `1.0.0` until release; clients compare the major only):

- *Revision 2 (2026-10-26)*, after the end-to-end test against the firmware (`docs/e2e-findings.md`) and the
  `pixelmapper-net` adapter: the failsafe mode is **per port** (`NodeStatus.failsafe` → `PortState.failsafe`,
  `artAddress` `failsafe`/`recordFailsafeScene` moved into `ports[]`); a port without RDM answers TodNak
  (`Tod.available: false`); clarifications of `nodeId` for simulators, `node` events per BindIndex page and
  NodeReport counter, TOD subscription after restarts, `todRequest`/`todControl` results, `artAddress` routing,
  `ipProg` DHCP arguments, `rdmDescribe` cache, `rdmQueued` (one queue for all controllers), ArtSync rule;
  implementation notes of `pmnet-bridge` (§11).
- *Revision 1 (2026-10-24)*: first candidate.

The key words MUST, SHOULD and MAY are used as in RFC 2119. Bit positions of Art-Net fields are given for orientation; the **Art-Net 4 specification and ANSI E1.20 are binding** for positions, polarity and encoding.

---

## 1. Connection

| | |
|---|---|
| URL | `ws://127.0.0.1:7777/` (port: `pmnet serve --port`; today `pmnet-bridge serve --port`). Any path is accepted. The app's default URL is overridable per device (setting “Bridge URL”). |
| Bind | Loopback only (`127.0.0.1`, and `::1` if available). Never a LAN address. |
| Origin | The bridge MUST reject the HTTP upgrade with `403` when an `Origin` header is present and not on the allow-list. Default allow-list: `http://localhost:*`, `http://127.0.0.1:*`, `tauri://localhost`, `http://tauri.localhost`, `https://tauri.localhost` and the hosted Pixelmapper origins (list maintained in `pmnet`, extendable with `--allow-origin`). Requests without `Origin` (CLI tools, tests) are allowed. |
| Clients | Several concurrent connections MUST be supported (e.g. two browser tabs). Events go to every connection that completed the handshake. Poll targets and DMX routes are per connection. |
| Frames | Text frames carry exactly one JSON object (UTF-8, no newline batching). Binary frames carry DMX (§7). |
| Keep-alive | The bridge SHOULD send WebSocket ping frames every 10 s; browsers answer automatically. |
| Size | Messages up to 4 MiB (Intel HEX images travel as text); larger ones end the connection. |
| Slow clients | Everything for a connection goes through one bounded, ordered queue (1024 messages in `pmnet-bridge`). The bridge never blocks on one connection and never drops single messages (that would corrupt the app's state): when a connection's queue is full, it is closed with `4003`. The app reconnects and re-reads `nodes`. |

### Handshake

```
app                                   bridge
 │ ── WebSocket upgrade (Origin) ───────► │
 │ ◄──────────────── event  hello ─────── │  first message, always
 │   app checks protocol major            │
 │ ── command hello {client, protocol} ─► │
 │ ◄──────────── result {} ────────────── │  from now on: events + commands
 │ ── command nodes ────────────────────► │
 │ ◄──────────── result {nodes, tods} ─── │  snapshot; later changes arrive as events
```

1. The bridge sends `hello` immediately after the upgrade.
2. The app compares the semver **major** of `protocol` with its own. On mismatch it closes the connection and reports *“Bridge update needed”* (bridge major lower) or *“App update needed”* (bridge major higher). An app that receives no `hello` within 3 s treats the peer as “not a bridge” (e.g. the old `tools/artnet-bridge`).
3. The app sends the `hello` command. The bridge answers `result` and from then on streams events to this connection. Commands other than `hello` before that are answered with error `helloRequired`. If the client's major does not match (or `protocol` is not semver), the bridge answers error `incompatibleProtocol` (`details.protocol` = the bridge's version) and closes with code `4001`. A repeated `hello` with a compatible version is answered `{}` again.
4. The app reads the current state with `nodes`.

Close codes: `4001` incompatible protocol, `4002` no client `hello` within 10 s, `4003` client too slow (its outbound queue overflowed, see Slow clients), `1001` bridge shutting down. A connection can also end without a close frame (browsers report `1006`).

### Ordering

The bridge writes events and results to one ordered channel per connection. A result reflects every event sent before it (e.g. a `nodes` snapshot never contains a node whose `nodeGone` was already sent; the `{ node }` of `artAddress`/`artInput` is at least as new as every `node` event sent before it). In the other direction the bridge processes a connection's messages in order: a `dmxRoutes` command applies to every binary message received after it. Commands that wait for the network (`todRequest`, `todControl`, `rdmGet`, `rdmSet`, `rdmDescribe`, `artAddress`, `ipProg`, `artInput`, `flash`) run concurrently and answer when they are done, so their replies can overtake each other and later commands — `id` correlates. At most 256 of them run per connection; further messages are read when one finishes.

### Versioning

`protocol` is semver. **Minor** versions only add (commands, events, optional arguments, fields, enum values in fields documented as open). The app MUST ignore unknown events and unknown fields; the bridge MUST ignore unknown argument fields and answer unknown commands with `unknownCommand`. This document describes **1.0.0**.

---

## 2. Message envelope

| Direction | Shape |
|---|---|
| app → bridge, command | `{ "type": "<command>", "id": <uint32>, ...arguments }` |
| bridge → app, result | `{ "type": "result", "id": <id>, "result": { ... } }` |
| bridge → app, error | `{ "type": "error", "id": <id> \| null, "code": "<errorCode>", "message": "<human text>", "details"?: { ... } }` |
| bridge → app, event | `{ "type": "<event>", ...payload }` (no `id`) |

- `id` is chosen by the app, unique per connection while the command is open (the app counts up from 1). Every command gets exactly one `result` or `error`.
- An `error` with `id: null` reports a message that could not be attributed (malformed JSON, not an object, missing or non-uint32 `id`). A readable `id` with a missing `type` gets `badRequest` with that `id`.
- `badRequest` names the argument in `details.field` as a path: `uid`, `port.index`, `ports[1].port`, `routes[0].ip`.
- `result` is always an object (`{}` when there is nothing to return).

### JSON conventions

| Value | Encoding |
|---|---|
| Names | camelCase |
| Integers | JSON numbers (all values fit in 32 bit; `uint32` fields are non-negative) |
| Binary data (RDM parameter data) | lowercase hex string without separators, `""` = empty |
| Timestamps | ms since the Unix epoch, bridge clock |
| Optional | `null` = known to be absent; a missing key = not applicable/not requested |
| IPv4 | dotted quad string |
| MAC | `"aa:bb:cc:dd:ee:ff"`, lowercase |

---

## 3. Identifiers

| Name | Format | Notes |
|---|---|---|
| `nodeId` | `"7FF0:8A1B2C3D"` (UID), else `"mac:02:7f:f0:1b:2c:3d"`, else `"ip:2.0.0.5"` | Stable identity of a node (plan D6): the RDM UID from `DefaultRespUID`; the MAC when the UID is all-zero; the IP only when both are zero (not stable — documented fallback; a node that does not use UDP port 6454, i.e. a simulator, gets `"ip:127.0.0.1:50000"`). Replies with the same IP are merged across `BindIndex` values. |
| `uid` | `"7FF0:74B6510C"` | RDM UID: manufacturer (4 hex digits) `:` device (8 hex digits), **uppercase**. Broadcasts: `"FFFF:FFFFFFFF"`, manufacturer broadcast `"7FF0:FFFFFFFF"`. |
| `port` | `{ "bind": 1, "index": 0 }` | `bind` = `BindIndex` of the ArtPollReply (1–255; 0 is reported as 1), `index` = port within that reply (0–3). The Pixelmapper Node sends **one port per BindIndex** (node contract §3), so its ports are `{bind: 1…4, index: 0}`; other Art-Net 4 nodes may report up to 4 ports per BindIndex. |
| `portAddress` | integer 0–32767 | 15-bit Port-Address: `net << 8 \| subNet << 4 \| universe`. Port-Address 0 is deprecated in Art-Net 4 (factory setting of the node: 1–4). |
| `pid` | integer 0–65535 | RDM parameter id. |
| `subDevice` | integer 0–512, `65535` = all | Optional in RDM commands, default `0` (root). |
| `jobId` | string | Firmware job, unique for the bridge's lifetime. |
| `route` | integer 1–65535 | DMX route of one connection (§7). |

---

## 4. Shared types

TypeScript notation; `?` = key may be missing.

### Interface, UDP

```ts
interface NetInterface { name: string; ip: string; mask: string; mac: string | null }
interface UdpState { bound: boolean; port: number; error: string | null } // port: the bound local UDP port (6454);
                                 // error e.g. "Address already in use"; an unbound socket is retried, `bridgeStatus` follows
```

### NodeState

One Art-Net node, merged from all its ArtPollReplies (one per `BindIndex`; the Pixelmapper Node sends 4, one per port) by `DefaultRespUID`, else IP + MAC. Node-level fields come from the reply with the lowest `BindIndex`. The bridge never reports its own ArtPollReply as a node.

```ts
interface NodeState {
  nodeId: string;
  uid: string | null;           // DefaultRespUID; null when all-zero
  mac: string | null;           // null when all-zero
  ip: string;                   // source IP of the replies (follows ArtIpProg/DHCP changes)
  interface: string | null;     // local interface the replies arrive on
  shortName: string;            // ShortName (PortName), ≤ 17 chars
  longName: string;             // LongName, ≤ 63 chars
  nodeReport: string;           // NodeReport text
  firmware: number;             // VersInfoH << 8 | VersInfoL
  estaMan: number;              // EstaMan (0x7FF0 for the Pixelmapper Node prototype)
  oem: number;                  // OemHi << 8 | Oem
  ubeaVersion: number;
  style: 'node' | 'controller' | 'media' | 'route' | 'backup' | 'config' | 'visual' | 'unknown'; // Style (StNode …)
  refreshRate: number;          // RefreshRate as reported (0 = not reported)
  status: NodeStatus;
  raw: { status1: number; status2: number; status3: number };
  ports: PortState[];           // every port of every BindIndex, ordered by bind, then index
  lastSeen: number;             // last reply (ms epoch)
}

interface NodeStatus {
  indicator: 'unknown' | 'locate' | 'mute' | 'normal';   // Status1 bits 7–6
  portAddressAuthority: 'unknown' | 'panel' | 'network'; // Status1 bits 5–4
  bootedFromRom: boolean;        // Status1 bit 2
  rdmCapable: boolean;           // Status1 bit 1
  ubeaPresent: boolean;          // Status1 bit 0
  webConfig: boolean;            // Status2 bit 0
  dhcpConfigured: boolean;       // Status2 bit 1 (IP obtained by DHCP)
  dhcpCapable: boolean;          // Status2 bit 2
  portAddress15Bit: boolean;     // Status2 bit 3
  sacnSwitchable: boolean;       // Status2 bit 4
  squawking: boolean;            // Status2 bit 5
  outputStyleSwitchable: boolean;// Status2 bit 6 (output style via ArtAddress)
  rdmSwitchable: boolean;        // Status2 bit 7 (RDM on/off via ArtAddress)
  failsafeProgrammable: boolean; // Status3 bit 5 (the failsafe mode itself is per port: PortState.failsafe)
  llrp: boolean;                 // Status3 bit 4
  portDirectionSwitchable: boolean; // Status3 bit 3
  rdmnet: boolean;               // Status3 bit 2
  backgroundQueue: boolean;      // Status3 bit 1
  backgroundDiscoveryControl: boolean; // Status3 bit 0
}

interface PortState {
  port: { bind: number; index: number };
  protocol: 'dmx512' | 'midi' | 'avab' | 'cmx' | 'adb' | 'artnet' | 'dali' | 'unknown'; // PortTypes bits 5–0
  canOutput: boolean;            // PortTypes bit 7 (Art-Net → DMX)
  canInput: boolean;             // PortTypes bit 6 (DMX → Art-Net)
  direction: 'output' | 'input' | 'both' | 'none'; // from PortTypes bits 7/6; with Status3 bit 3 set they show the
                                 // CURRENT direction (node contract §3), so a switchable port is 'output' or 'input'
  outputPortAddress: number;     // NetSwitch, SubSwitch, SwOut[i]
  inputPortAddress: number;      // NetSwitch, SubSwitch, SwIn[i]; the Pixelmapper Node fills only the field of the
                                 // current direction, the other has universe 0 (node contract §3)
  failsafe: 'hold' | 'zero' | 'full' | 'scene' | null; // Status3 bits 7–6 of this port's ArtPollReply (the failsafe
                                 // mode is per BindIndex, i.e. per port on the Pixelmapper Node); null = not known
  output: {
    data: boolean;               // GoodOutputA bit 7: ArtDmx/sACN is being output as DMX
    merging: boolean;            // GoodOutputA bit 3
    merge: 'htp' | 'ltp';        // GoodOutputA bit 1 (set = LTP)
    shortCircuit: boolean;       // GoodOutputA bit 2: DMX short detected on power-up
    sacn: boolean;               // GoodOutputA bit 0 (set = output from sACN)
    rdm: boolean;                // GoodOutputB bit 7 CLEAR (set = RDM disabled)
    style: 'delta' | 'continuous'; // GoodOutputB bit 6 (set = continuous)
    discoveryRunning: boolean;   // GoodOutputB bit 5 CLEAR (set = discovery not running)
    backgroundDiscovery: boolean;// GoodOutputB bit 4 CLEAR (set = background discovery off)
  };
  input: {
    data: boolean;               // GoodInput bit 7: data received (clear = no signal)
    disabled: boolean;           // GoodInput bit 3
    errors: boolean;             // GoodInput bit 2
    sacn: boolean;               // GoodInput bit 0 (set = input converted to sACN)
  };
}
```

### Tod

```ts
interface Tod {
  nodeId: string;
  port: { bind: number; index: number };
  portAddress: number;           // Port-Address the ArtTodData was sent for
  available: boolean;            // false when the node answered TodNak: TOD not available, e.g. a port without RDM
                                 // (port off, input, or RDM disabled; node contract §6.1)
  uids: string[];                // complete list over all blocks (UidTotal), sorted ascending; [] when !available
}
```

### RDM

```ts
interface RdmDeviceInfo {        // DEVICE_INFO (0x0060), E1.20 §10.5.1
  rdmProtocolVersion: number;    // 0x0100 = 1.0
  deviceModelId: number;
  productCategory: number;       // E1.20 Table A-5
  softwareVersionId: number;     // uint32
  dmxFootprint: number;
  currentPersonality: number;    // 1-based
  personalityCount: number;
  dmxStartAddress: number;       // 1–512; 65535 = none (footprint 0)
  subDeviceCount: number;
  sensorCount: number;
}

interface RdmPersonality {       // DMX_PERSONALITY_DESCRIPTION (0x00E1)
  index: number;                 // 1-based
  footprint: number;
  description: string | null;    // null when the PID is not supported
}

interface RdmSlot {              // SLOT_INFO (0x0120) + SLOT_DESCRIPTION (0x0121) + DEFAULT_SLOT_VALUE (0x0122)
  offset: number;                // 0-based slot offset in the current personality
  type: number;                  // slot type, E1.20 Table C-1 (ST_PRIMARY = 0, ST_SEC_FINE = 1, …)
  labelId: number;               // slot label id, E1.20 Table C-2 (SD_INTENSITY = 0x0001, …) or the primary slot for secondary types
  description: string | null;
  defaultValue: number | null;
}

interface RdmParameterDescription { // PARAMETER_DESCRIPTION (0x0051), E1.20 §10.3.2
  pid: number;
  pdlSize: number;
  dataType: number;              // raw, E1.20 Table A-15
  dataTypeName: 'notDefined' | 'bitField' | 'ascii' | 'uint8' | 'int8' | 'uint16' | 'int16' | 'uint32' | 'int32' | 'manufacturer';
  commandClass: 'get' | 'set' | 'getSet'; // CC_GET = 0x01, CC_SET = 0x02, CC_GET_SET = 0x03
  type: number;                  // raw "Type" field
  unit: number;                  // raw, E1.20 Table A-13 (UNITS_NONE = 0, …)
  prefix: number;                // raw, E1.20 Table A-14 (PREFIX_NONE = 0, …)
  minValue: number;              // int32 for signed data types, uint32 otherwise
  maxValue: number;
  defaultValue: number;
  description: string;
}

interface RdmStatusMessage {     // STATUS_MESSAGES (0x0030)
  subDevice: number;
  statusType: 'none' | 'advisory' | 'warning' | 'error' | 'advisoryCleared' | 'warningCleared' | 'errorCleared';
  statusMessageId: number;
  dataValue1: number;
  dataValue2: number;
  text: string | null;           // E1.20 Table B-2 text, or STATUS_ID_DESCRIPTION for manufacturer ids
}

interface RdmDeviceDescription {
  uid: string;
  deviceInfo: RdmDeviceInfo;
  manufacturerLabel: string | null;       // MANUFACTURER_LABEL (0x0081)
  deviceModelDescription: string | null;  // DEVICE_MODEL_DESCRIPTION (0x0080)
  deviceLabel: string | null;             // DEVICE_LABEL (0x0082)
  softwareVersionLabel: string | null;    // SOFTWARE_VERSION_LABEL (0x00C0)
  bootSoftwareVersionId: number | null;   // BOOT_SOFTWARE_VERSION_ID (0x00C1)
  bootSoftwareVersionLabel: string | null;// BOOT_SOFTWARE_VERSION_LABEL (0x00C2)
  personalities: RdmPersonality[];        // index 1…personalityCount
  slots: RdmSlot[];                       // current personality; [] when SLOT_INFO is not supported
  supportedParameters: number[];          // SUPPORTED_PARAMETERS (0x0050) as reported
  parameterDescriptions: RdmParameterDescription[]; // every manufacturer PID (0x8000–0xFFDF) in supportedParameters
  errors: { pid: number; code: ErrorCode; message: string; nackReason?: number }[]; // PIDs that failed; the rest is still returned
}
```

**`RdmValue`** — decoded parameter data, used by `rdmGet`/`rdmSet`/`rdmQueued`:

| PID | GET argument `value` | GET result `value` | SET `value` |
|---|---|---|---|
| `DEVICE_INFO` 0x0060 | – | `RdmDeviceInfo` | – |
| `SUPPORTED_PARAMETERS` 0x0050 | – | `number[]` | – |
| `PARAMETER_DESCRIPTION` 0x0051 | pid (`number`) | `RdmParameterDescription` | – |
| `DEVICE_MODEL_DESCRIPTION` 0x0080, `MANUFACTURER_LABEL` 0x0081, `SOFTWARE_VERSION_LABEL` 0x00C0, `BOOT_SOFTWARE_VERSION_LABEL` 0x00C2 | – | `string` | – |
| `DEVICE_LABEL` 0x0082 | – | `string` | `string` (≤ 32 chars) |
| `BOOT_SOFTWARE_VERSION_ID` 0x00C1 | – | `number` | – |
| `DMX_PERSONALITY` 0x00E0 | – | `{ current: number; count: number }` | `number` (1-based) |
| `DMX_PERSONALITY_DESCRIPTION` 0x00E1 | personality (`number`) | `RdmPersonality` | – |
| `DMX_START_ADDRESS` 0x00F0 | – | `number` | `number` (1–512) |
| `SLOT_INFO` 0x0120 | – | `{ offset; type; labelId }[]` | – |
| `SLOT_DESCRIPTION` 0x0121 | slot offset (`number`) | `{ offset: number; description: string }` | – |
| `DEFAULT_SLOT_VALUE` 0x0122 | – | `{ offset: number; value: number }[]` | – |
| `IDENTIFY_DEVICE` 0x1000 | – | `boolean` | `boolean` |
| `STATUS_MESSAGES` 0x0030 | status type (`'advisory' \| 'warning' \| 'error' \| 'none'`) | `RdmStatusMessage[]` | – |
| Manufacturer PID with a single-value PARAMETER_DESCRIPTION known to the bridge (read by `rdmDescribe`, or generated from `pids.toml`, e.g. the node's 0x8100–0x81FF) | – | integer types: `number` (raw, unscaled; the app applies `prefix`), `bool` fields: `boolean`, `ascii`: `string`, `bitField`: `number` | same as GET |

Other PIDs — structured manufacturer PIDs (data type `notDefined`, e.g. BOOT_CATCH_*, FW_*, LOCAL_RDM_SET) and the tube bootloader PIDs, which the bridge drives itself — use `data` (hex) only. The bridge MUST always return `data` as well, so the app can fall back.

### Firmware

```ts
interface FirmwareImageInfo {    // tube image trailer at 0x17F0 (node contract §9.1)
  format: number;                // trailer format (1)
  manufacturerId: number;        // 0x7FF0
  modelId: number;               // 0x0001 = XBAR500 tube
  versionId: number;             // FW_VERSION_ID 0x00MMmmpp
  version: string;               // "M.m.p"
  length: number;                // app length in bytes
}

type FlashPhase = 'entering' | 'writing' | 'verifying' | 'finishing' | 'confirming' | 'done' | 'failed' | 'cancelled';
// entering: checking the device and entering the bootloader; verifying includes rewriting pages whose CRC differed

interface FlashJob {
  jobId: string;
  nodeId: string;
  port: { bind: number; index: number };
  uid: string;
  phase: FlashPhase;
  pagesDone: number;             // pages written (all-0xFF pages are skipped and not counted)
  pagesTotal: number;
  rewrites: number;              // pages rewritten after an APP_CRC mismatch (all pages of the differing chunks)
  inBootloader: boolean;         // true while the tube runs its bootloader (also after failed/cancelled)
  image: FirmwareImageInfo;
  error: { code: ErrorCode; message: string } | null; // set when phase = failed
  result: { softwareVersionId: number; softwareVersionLabel: string | null } | null; // set when phase = done
}
```

---

## 5. Events (bridge → app)

Events are sent only to connections that completed the handshake.

| Type | Payload | When |
|---|---|---|
| `hello` | `bridge: string` (pmnet semver), `protocol: string` (semver), `interfaces: NetInterface[]`, `udp: UdpState` | first message of every connection |
| `bridgeStatus` | `interfaces: NetInterface[]`, `udp: UdpState` | an interface or the UDP socket state changed |
| `node` | `node: NodeState` | node first seen, and whenever any field other than `lastSeen` changes (unsolicited ArtPollReply, IP change, merged BindIndex reply). The BindIndex pages of a node arrive one by one, so a new node's ports usually come in several `node` events. `nodeReport` carries the node's ArtPollReply counter (`#0001 [0123] …`), so a node typically changes once per poll cycle. The bridge sends one ArtTodRequest per RDM output port when the node appears or reappears after `nodeGone`, when a port appears, changes its Port-Address or gets RDM switched on, and after a restart (other firmware, or the NodeReport counter starting over): the node only sends unsolicited ArtTodData to IPs that requested a TOD before (node contract §6.1) |
| `nodeGone` | `nodeId: string`, `lastSeen: number` | no reply for 3 consecutive poll cycles; the node and its TODs leave the bridge's table |
| `tod` | `Tod` fields (`nodeId`, `port`, `portAddress`, `available`, `uids`) | every completely assembled ArtTodData set (requested or unsolicited); the app diffs |
| `rdmQueued` | `nodeId`, `port`, `uid`, `subDevice`, `pid`, `data` (hex), `value?: RdmValue` | a queued message fetched with QUEUED_MESSAGE (the bridge fetches whenever a response's message count is > 0, node contract §6.3). Its content is the GET response of the changed PID — e.g. `LOCAL_RDM_SET` (0x8150) after an RDM SET from the node's display, upon which the app re-reads that PID at the device (node contract §7). The node has **one** queue for all controllers (node contract §6.3): another controller may fetch a message first, so `rdmQueued` is best effort — the app also re-reads the node's settings PIDs on `node` events that change the node and every 30–60 s |
| `diag` | `nodeId: string \| null`, `ip`, `priority: 'low' \| 'medium' \| 'high' \| 'critical' \| 'volatile'`, `logicalPort: number`, `text` | ArtDiagData received (DpLow 0x10, DpMed 0x40, DpHigh 0x80, DpCritical 0xE0, DpVolatile 0xF0) |
| `flashProgress` | `job: FlashJob` | every phase change, and at most 10×/s while writing/verifying |
| `log` | `level: 'debug' \| 'info' \| 'warn' \| 'error'`, `text`, `nodeId?: string` | bridge diagnostics worth showing (bind errors, malformed packets, retries exhausted, event stream resynchronized). Warnings about a connection's own malformed binary messages go to that connection only, at most one per second |

Examples:

```jsonc
{ "type": "hello", "bridge": "0.1.0", "protocol": "1.0.0",
  "interfaces": [{ "name": "en7", "ip": "2.0.0.1", "mask": "255.0.0.0", "mac": "a0:ce:c8:01:02:03" }],
  "udp": { "bound": true, "port": 6454, "error": null } }

{ "type": "node", "node": { "nodeId": "7FF0:8A1B2C3D", "uid": "7FF0:8A1B2C3D", "mac": "02:7f:f0:1b:2c:3d",
  "ip": "2.27.44.61", "interface": "en7", "shortName": "PM Node", "longName": "Pixelmapper Node",
  "nodeReport": "#0001 [0012] OK", "firmware": 256, "estaMan": 32752, "oem": 255, "ubeaVersion": 0,
  "style": "node", "refreshRate": 44, "status": { "indicator": "normal", "...": "…" },
  "raw": { "status1": 226, "status2": 14, "status3": 40 },
  "ports": [{ "port": { "bind": 1, "index": 0 }, "protocol": "dmx512", "canOutput": true, "canInput": false,
              "direction": "output", "outputPortAddress": 1, "inputPortAddress": 0, "failsafe": "hold",
              "output": { "data": true, "merging": false, "merge": "htp", "shortCircuit": false, "sacn": false,
                          "rdm": true, "style": "continuous", "discoveryRunning": false, "backgroundDiscovery": true },
              "input": { "data": false, "disabled": false, "errors": false, "sacn": false } },
            { "port": { "bind": 2, "index": 0 }, "...": "…" }, { "port": { "bind": 3, "index": 0 }, "...": "…" },
            { "port": { "bind": 4, "index": 0 }, "...": "…" }],
  "lastSeen": 1761336000000 } }

{ "type": "tod", "nodeId": "7FF0:8A1B2C3D", "port": { "bind": 1, "index": 0 }, "portAddress": 1,
  "available": true, "uids": ["7FF0:74B6510C", "7FF0:74B6510D"] }

{ "type": "flashProgress", "job": { "jobId": "f3", "nodeId": "7FF0:8A1B2C3D", "port": { "bind": 3, "index": 0 },
  "uid": "7FF0:74B6510C", "phase": "writing", "pagesDone": 12, "pagesTotal": 94, "rewrites": 0, "inBootloader": true,
  "image": { "format": 1, "manufacturerId": 32752, "modelId": 1, "versionId": 66048, "version": "1.2.0", "length": 6016 },
  "error": null, "result": null } }
```

---

## 6. Commands (app → bridge)

Common RDM addressing arguments: `nodeId`, `port`, `uid`, `subDevice?` (default 0). For RDM to the node itself use the node's `uid` and any of its ports (node contract §6.3).

| Command | Arguments | Result | Errors (besides `badRequest`) |
|---|---|---|---|
| `hello` | `client: string` (`"pixelmapper"`), `clientVersion: string`, `protocol: string` (`client`/`clientVersion` are informational and may be left out) | `{}` | `incompatibleProtocol` |
| `nodes` | – | `{ nodes: NodeState[], tods: Tod[] }` | – |
| `poll` | – | `{}` — sends an ArtPoll now (on top of the 2.5–3 s cycle) | `udpUnavailable` |
| `addPollTarget` | `ip` | `{}` — the bridge also unicasts every ArtPoll to `ip` while this connection lives (manual add by IP, nodes outside the broadcast domain) | – |
| `removePollTarget` | `ip` | `{}` | – |
| `todRequest` | `nodeId`, `port` | `{}` — ArtTodRequest; the answer arrives as `tod`, also when the TOD did not change | `unknownNode`, `unknownPort` |
| `todControl` | `nodeId`, `port`, `action: 'flush' \| 'end' \| 'incOn' \| 'incOff'` | `{}` — ArtTodControl `AtcFlush` (clear TOD + full discovery; the new TOD arrives as `tod` — when it did not change, the bridge re-requests it after ~3 s and sends that), `AtcEnd`, `AtcIncOn`/`AtcIncOff` (background discovery of the port on/off) | `unknownNode`, `unknownPort` |
| `rdmGet` | RDM addressing, `pid`, `data?` (hex) or `value?` (argument, see `RdmValue`) | `{ data: string, value?: RdmValue }` | `unknownNode`, `unknownPort`, `timeout`, `nack`, `badResponse` |
| `rdmSet` | RDM addressing, `pid`, exactly one of `data` (hex) / `value` | `{ data: string, value?: RdmValue, broadcast?: true }` | as `rdmGet` |
| `rdmDescribe` | RDM addressing (unicast `uid`), `refresh?: boolean` | `RdmDeviceDescription` | `unknownNode`, `unknownPort`, and the DEVICE_INFO failure (`timeout`, `nack`, `badResponse`) |
| `artAddress` | `nodeId`, see below | `{ node: NodeState }` | `unknownNode`, `unknownPort`, `invalidPortAddress`, `timeout` |
| `ipProg` | `nodeId`, `dhcp?: boolean`, `ip?`, `mask?`, `gateway?`, `resetToDefault?: boolean` | `{ ip, mask, gateway, dhcp: boolean }` (ArtIpProgReply) | `unknownNode`, `timeout` |
| `artInput` | `nodeId`, `bind`, `disabled: boolean[]` (one per port of that BindIndex) | `{ node: NodeState }` — affects **input** ports only (node contract §5); output ports are enabled/disabled with the node's RDM PID `PORTn_ENABLED` (`pids.toml`) | `unknownNode`, `unknownPort`, `timeout` |
| `imageInfo` | `hex` (Intel HEX file content) | `FirmwareImageInfo` | `invalidImage` |
| `flash` | RDM addressing (unicast `uid`), `hex`, `allowDowngrade: boolean` (default `false`) | `{ job: FlashJob }` | `unknownNode`, `unknownPort`, `invalidImage`, `deviceMismatch`, `downgrade`, `busy`, `timeout`, `nack` |
| `flashCancel` | `jobId` | `{}` — the job ends with phase `cancelled` | `unknownJob` |
| `flashJobs` | – | `{ jobs: FlashJob[] }` — running and finished jobs of the last 10 minutes | – |
| `dmxRoutes` | `routes: DmxRoute[]` | `{}` — replaces this connection's route table (§7); an invalid table is rejected as a whole and the old one stays | – |

### Semantics

- **RDM transactions** (node contract §6.2, plan M1.5): per port at most min(4, `FifoAvail` of the node's last ArtRdm) requests in flight, 500 ms response timeout, 2 retries → `timeout`. `ACK_TIMER` is waited out and the response fetched as E1.20 requires; `ACK_OVERFLOW` is reassembled; the app only sees the final ACK or NACK. Requests to broadcast UIDs return `{ data: "", broadcast: true }` right after sending. A NACK reports `details.nackReason` (number) and `details.nackName` (`UNKNOWN_PID`, `FORMAT_ERROR`, `HARDWARE_FAULT`, `PROXY_REJECT`, `WRITE_PROTECT`, `UNSUPPORTED_COMMAND_CLASS`, `DATA_OUT_OF_RANGE`, `BUFFER_FULL`, `PACKET_SIZE_UNSUPPORTED`, `SUB_DEVICE_OUT_OF_RANGE`, `PROXY_BUFFER_FULL`, `ACTION_NOT_SUPPORTED`, else `"0x…"`).
- **`rdmDescribe`** reads DEVICE_INFO, SUPPORTED_PARAMETERS, the labels, software/boot versions, DMX_PERSONALITY_DESCRIPTION for every personality, SLOT_INFO/SLOT_DESCRIPTION/DEFAULT_SLOT_VALUE for the current personality and PARAMETER_DESCRIPTION for every manufacturer PID. Only a DEVICE_INFO failure fails the command; other failures go to `errors` and the field is `null`/`[]`. Descriptions of personalities and parameters MAY be cached per (`uid`, `softwareVersionId`); `refresh: true` bypasses the cache (`pmnet-bridge` keys the cache by `uid` and `subDevice` and reuses it while `softwareVersionId` and SUPPORTED_PARAMETERS are unchanged). Without DMX_PERSONALITY_DESCRIPTION, `personalities` still lists 1…`personalityCount` with `description: null`; the `footprint` is then only known for the current personality (DEVICE_INFO), else 0. After `rdmDescribe`, the bridge knows the PARAMETER_DESCRIPTIONs of that device and decodes `value` for its manufacturer PIDs.
- **`artAddress`** arguments (all optional except `nodeId`):

  ```ts
  {
    nodeId: string;
    shortName?: string; longName?: string;
    ports?: {
      port: { bind: number; index: number };
      outputPortAddress?: number; inputPortAddress?: number;  // writes NetSwitch/SubSwitch + SwOut/SwIn
      direction?: 'output' | 'input';                         // AcDirectionTx/Rx
      merge?: 'htp' | 'ltp';                                  // AcMergeHtp/Ltp
      rdm?: boolean;                                          // AcRdmEnable/Disable
      outputStyle?: 'delta' | 'continuous';                   // AcStyleDelta/Const
      protocol?: 'artnet' | 'sacn';                           // AcArtNetSel/AcAcnSel
      clearOutput?: true;                                     // AcClearOp
      failsafe?: 'hold' | 'zero' | 'full' | 'scene';          // AcFailHold/Zero/Full/Scene to the port's BindIndex
      recordFailsafeScene?: true;                             // AcFailRecord to the port's BindIndex
    }[];
    indicator?: 'normal' | 'mute' | 'locate';                 // AcLedNormal/Mute/Locate
    cancelMerge?: true;                                       // AcCancelMerge
    resetRxFlags?: true;                                      // AcResetRxFlags
  }
  ```

  `shortName` ≤ 17 and `longName` ≤ 63 bytes, else `badRequest`; each port at most once. The bridge sends as many ArtAddress packets as needed (one Command per packet; names and addresses with the first packet of each BindIndex) and answers with the node state from the node's ArtPollReply to the last packet (`timeout` after 1 s and one resend). Ports of one BindIndex share NetSwitch and SubSwitch: every written address and the current address of every port of that BindIndex that is *not* written must have the same bits 14–4 — otherwise the bridge sends nothing and answers `invalidPortAddress` (to move all ports to another Sub-Net, write all of them in one command). Fields left out are sent as “no change”. Names and `indicator` go to the lowest BindIndex of the node. `failsafe` and `recordFailsafeScene` go to the port's BindIndex (the AcFail* commands carry no port number: a node with several ports per BindIndex applies them to all ports of that BindIndex). `cancelMerge` and `resetRxFlags` go to every BindIndex of the node. An `artAddress` without any change sends nothing and answers with the current state.
- **`ipProg`**: ArtIpProg with the corresponding Command bits; the answer is the ArtIpProgReply. The node then replies to polls from its new IP with the same `nodeId` (`node` event). An `ipProg` with only `nodeId` (no other field) is a **read**: the bridge sends ArtIpProg with Command = 0 (no programming, per the Art-Net spec) and answers with the node's current IP, mask, gateway and DHCP state from the ArtIpProgReply — this is how clients read mask and gateway, which ArtPollReply does not carry. `dhcp: true` cannot be combined with `ip`, `mask`, `gateway` or `resetToDefault` (the node ignores them when DHCP is switched on) and `dhcp: false` needs `ip` (Art-Net switches DHCP off by programming a static IP); both are answered `badRequest` with `details.field: "dhcp"`.
- **`flash`** (plan M2.2, node contract §9.1): before answering, the bridge parses the image (`invalidImage`), reads DEVICE_INFO (manufacturer and model must match the trailer → else `deviceMismatch`; `softwareVersionId` > image `versionId` without `allowDowngrade` → `downgrade` with `details: { current, image }`). A tube already in its bootloader (no DEVICE_INFO, but BOOT_INFO answers with matching manufacturer/model) is accepted and continues at “enter” step 4. One job per (`nodeId`, `port`) at a time (`busy`); ports run in parallel. The job belongs to the bridge: it continues when the connection drops, and progress goes to every connection.

### DmxRoute

```ts
type DmxRoute =
  | { route: number; nodeId: string; portAddress: number }  // unicast to the node's current IP
  | { route: number; ip: string; portAddress: number };     // manual target, unicast IP only
```

Route ids are unique within a table; each route has exactly one of `nodeId` / `ip`; `portAddress` 0–32767. Routes to nodes that are not (yet) known are kept; their frames are dropped until the node appears and follow the node's IP when it changes. An `ip` route to a broadcast address (255.255.255.255 or an interface's directed broadcast) is rejected with `badRequest` — Art-Net 4 forbids broadcast ArtDmx. A route whose id, destination and Port-Address stay the same across `dmxRoutes` keeps its last data (refresh) and rate-limit state.

---

## 7. DMX (binary, app → bridge)

One binary message carries one **frame set**: all universes of one rendered frame.

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | magic `0x50 0x4D` (`"PM"`) |
| 2 | 1 | kind `0x01` = DMX frame set |
| 3 | 1 | flags: bit 0 = send ArtSync after the set; other bits 0 (ignored by the bridge) |
| 4 | 2 | `count` (u16 LE) |
| 6 | … | `count` entries: `route` (u16 LE), `length` (u16 LE, 1–512), `length` DMX slot bytes (slot 1 first, no start code) |

The bridge, for every entry: looks up `route` in this connection's table (unknown routes are skipped), pads an odd `length` with one zero byte (ArtDmx needs an even length, 2–512), and sends **ArtDmx unicast** to the route's node IP (or `ip`) with the route's Port-Address and a sequence number per (destination IP, Port-Address) counting 1–255 (never 0). `Physical` is constant per connection (the bridge numbers its connections 0, 1, 2, … mod 256): merge sources are IP + Physical (node contract §4), so two app windows are two merge sources instead of one flickering source. After the set, if flag bit 0 is set, it sends **ArtSync as a broadcast** (Art-Net 4 forbids unicast ArtSync) on every interface that carried ArtDmx of the set. Nodes ignore ArtSync while they merge streams of **different IPs** on a port (two connections of one bridge are one IP with two Physicals and do not block it) or when it does not come from the IP of the last ArtDmx (node contract §4). Malformed messages (bad magic, unknown kind, truncated entries, `length` 0 or > 512) are dropped as a whole with a `log` warning; bytes after the last entry are ignored.

- **Refresh:** if the app sends no new data for a route for 1 s, the bridge re-sends that route's last data (followed by ArtSync when the set that carried it asked for one), every 1 s while idle, so the node's failsafe (~4 s) does not trigger while Pixelmapper is open but idle. Closing the connection drops its routes and stops the re-sending.
- **Rate:** the app sends one frame set per rendered frame (its render rate, up to ~60 sets/s) and SHOULD drop sets while the WebSocket's send buffer is backed up (never queue). The bridge sends ArtDmx to a node at most at the node's `RefreshRate` (44 Hz when it reports 0), coalescing faster sets per route (latest data wins), and at most one ArtSync per sent set. Binary messages before the handshake completed are ignored.
- Kinds other than `0x01` are reserved (`0x02` = DMX input, bridge → app, planned).

### Legacy clients (optional)

Not implemented by `pmnet-bridge` 0.1 (a legacy `config` message gets a `badRequest` error, the connection is closed with `4002`). The old `tools/artnet-bridge` protocol MAY be served for the existing “ArtNet (manual target)” connector: a connection that sends `{ "type": "config", "targetIp", "artnetPort" }` before any `hello`, and binary frames `[universe u16 LE][512 bytes]` (1-based app universe → Port-Address `universe − 1`), gets ArtDmx to `targetIp:artnetPort` and no events.

---

## 8. Error codes

| Code | Meaning |
|---|---|
| `badRequest` | malformed JSON, missing or invalid arguments (`details.field` names the argument) |
| `unknownCommand` | command type not implemented by this bridge |
| `helloRequired` | command before the client `hello` |
| `incompatibleProtocol` | client protocol major differs |
| `unknownNode` | `nodeId` is not in the node table (never seen or gone) |
| `unknownPort` | the node has no such `port` |
| `timeout` | no answer from the network after the retries (RDM, ArtAddress, ArtIpProg, ArtInput) |
| `nack` | RDM NACK; `details.nackReason`, `details.nackName` |
| `badResponse` | malformed or unexpected answer after the retries |
| `invalidPortAddress` | port-addresses violate the shared Net/SubNet of a BindIndex, or are out of range |
| `invalidImage` | Intel HEX or trailer invalid (`message` says why) |
| `deviceMismatch` | device manufacturer/model do not match the image |
| `downgrade` | image older than the device firmware and `allowDowngrade` is false; `details: { current, image }` (version ids) |
| `busy` | a firmware job already runs on that port |
| `unknownJob` | `jobId` unknown |
| `udpUnavailable` | the Art-Net UDP socket is not bound (see `hello.udp`) |
| `internal` | bridge bug; `message` has details |

App-side codes (never sent by the bridge, used by the TypeScript client): `notConnected` (no handshaken connection), `disconnected` (connection lost while the command was open), `requestTimeout` (no `result`/`error` in time).

---

## 9. Timing summary (bridge)

| What | Value |
|---|---|
| ArtPoll | broadcast to every interface's subnet broadcast **and** `2.255.255.255` when an interface is in 2.0.0.0/8 (never 255.255.255.255), every 2.5–3 s, `Flags` bit 1 set; replies arrive unicast with up to 1 s random delay (node contract §3) |
| Node gone | no reply for 3 poll cycles |
| RDM | 500 ms timeout, 2 retries, ≤ min(4, FifoAvail) in flight per port |
| TOD | one ArtTodRequest per RDM output port when a node or port appears/reappears and after a node restart (subscribes to unsolicited ArtTodData); after `AtcFlush` an unchanged TOD is re-requested after ~3 s |
| ArtAddress / ArtIpProg / ArtInput | answer within 1 s, one resend |
| DMX refresh | last data re-sent after 1 s without new data, then every 1 s |
| DMX rate | ArtDmx per route at most at the node's `RefreshRate` (44 Hz for 0 and for `ip` routes); faster sets coalesce, latest data wins |
| Handshake | client `hello` within 10 s (else `4002`); WebSocket ping every 10 s |

## 10. Open

- DMX input (node input ports → Pixelmapper) as binary kind `0x02`.
- Authentication beyond `Origin` (token in the URL?) for multi-user machines.
- The hosted app origins for the default allow-list (`HOSTED_ORIGINS` in `crates/pmnet-bridge/src/server/origin.rs`; until then `--allow-origin`).

## 11. Implementation notes (`pmnet-bridge` 0.1 on `pixelmapper-net`)

Where the bridge reports less than this contract describes, because `pixelmapper-net` does not keep the data yet:

- `NodeState.ubeaVersion` is 0; `NetInterface.mac` is `null`.
- `PortState.failsafe` is known only for the ports of the lowest BindIndex (the library keeps Status3 of that reply);
  the other ports report `null`.
- A port carries one Port-Address: the field of its current direction gets it, the other one Net/Sub-Net with
  universe 0 (exact for the Pixelmapper Node, node contract §3).
- `lastSeen` is the time the bridge last processed an ArtPollReply that changed the node (with the NodeReport counter
  that is about every poll cycle).
- ArtSync goes to every broadcast target of the bridge (each interface's directed broadcast and 2.255.255.255, or the
  `--broadcast` targets), not only to the interfaces that carried ArtDmx of the set.
- `RdmStatusMessage.text` is filled for the manufacturer status ids of `pids.toml`; E1.20 Table B-2 texts are not
  available yet (`null`).
