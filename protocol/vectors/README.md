# Test vectors

Shared reference packets for the C firmware (`node/firmware/test_host`) and the Rust crate
(`crates/pixelmapper-net`). Both test suites load **every** file below this directory.
Schema: [`schema.json`](schema.json).

## Layout

```
vectors/
  artnet/<Packet>.json   e.g. artnet/ArtPoll.json, artnet/ArtPollReply.json
  rdm/<Packet>.json      e.g. rdm/RDM_GET_DEVICE_INFO.json, rdm/DUB_RESPONSE.json
  image/<Name>.json      image trailers / description blocks (protocol/image-format.md)
```

## Field conventions (`fields`)

| Kind | JSON representation | Example |
|---|---|---|
| Field names | snake_case of the spec name | `ArtPollReply.NetSwitch` -> `net_switch` |
| Integers (any width) | JSON number, value as decoded (endianness already applied) | `"op_code": 8192` |
| Bit fields / flags | the raw integer; optional extra keys for decoded bits are allowed | `"status1": 208` |
| Arrays of small ints | JSON array | `"sw_out": [0, 1, 2, 3]` |
| ASCII strings | JSON string without trailing NULs | `"short_name": "PM Node"` |
| Binary blobs (DMX data, RDM payload, PD) | lowercase hex string without spaces | `"data": "00ff10"` |
| RDM UID | `"MMMM:DDDDDDDD"` upper-case hex | `"dest_uid": "7FF0:80123456"` |
| IPv4 | dotted quad | `"ip": "2.0.0.10"` |
| MAC | `aa:bb:cc:dd:ee:ff` lower-case | |
| Port-Address (15 bit) | number, plus `net`, `sub_net`, `universe` if useful | |

Rules:
- `hex` is the complete packet: the full UDP payload for Art-Net, the full wire bytes **including start code 0xCC**
  for RDM (DUB responses: preamble + separator + encoded EUID + checksum, no start code).
- Every vector states its `source`. Vectors derived from the spec say table/section; vectors captured from real
  devices say so.
- `valid: false` vectors are packets a decoder must reject (`fields = {"error": "short"|"bad_id"|"bad_checksum"|...}`).
- Never change an existing vector silently: a changed vector means a protocol change -> note it in `protocol/contract.md`.
