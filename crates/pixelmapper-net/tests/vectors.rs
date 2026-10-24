//! Checks every file under `protocol/vectors/` against the schema basics and decodes the packets this crate
//! implements: every Art-Net packet (all fields, rejection of invalid vectors, byte-identical re-encode), basic
//! RDM messages and DUB responses.

use pixelmapper_net::artnet::{
    ArtAddress, ArtDmx, ArtFirmwareMaster, ArtFirmwareReply, ArtInput, ArtIpProg, ArtIpProgReply,
    ArtNetError, ArtPacket, ArtPoll, ArtPollReply, ArtRdm, ArtSync, ArtTodControl, ArtTodData,
    ArtTodRequest, Header, PortAddress,
};
use pixelmapper_net::rdm::{DubResponse, RdmMessage, Uid};
use pixelmapper_net::{parse_hex, to_hex};
use serde_json::{json, Map, Value};
use std::path::{Path, PathBuf};

const VECTOR_KEYS: [&str; 6] = ["name", "description", "source", "hex", "valid", "fields"];

fn vectors_dir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../protocol/vectors")
}

fn collect_json(dir: &Path, out: &mut Vec<PathBuf>) {
    let mut entries: Vec<_> = std::fs::read_dir(dir)
        .unwrap_or_else(|e| panic!("read_dir {}: {e}", dir.display()))
        .map(|e| e.unwrap().path())
        .collect();
    entries.sort();
    for p in entries {
        if p.is_dir() {
            collect_json(&p, out);
        } else if p.extension().is_some_and(|x| x == "json")
            && p.file_name().unwrap() != "schema.json"
        {
            out.push(p);
        }
    }
}

struct Vector {
    file: String,
    protocol: String,
    packet: String,
    v: Value,
    bytes: Vec<u8>,
}

impl Vector {
    fn id(&self) -> String {
        format!("{}:{}", self.file, self.v["name"].as_str().unwrap())
    }
    fn valid(&self) -> bool {
        self.v["valid"].as_bool().unwrap()
    }
    fn field(&self, k: &str) -> &Value {
        &self.v["fields"][k]
    }
    fn num(&self, k: &str) -> u64 {
        self.field(k)
            .as_u64()
            .unwrap_or_else(|| panic!("{}: field {k} missing", self.id()))
    }
    fn opt_num(&self, k: &str) -> Option<u64> {
        self.field(k).as_u64()
    }
}

fn load_all() -> Vec<Vector> {
    let mut files = Vec::new();
    collect_json(&vectors_dir(), &mut files);
    assert!(!files.is_empty(), "no vector files found");
    let mut out = Vec::new();
    for path in files {
        let file = path
            .strip_prefix(vectors_dir())
            .unwrap()
            .display()
            .to_string();
        let doc: Value = serde_json::from_str(&std::fs::read_to_string(&path).unwrap())
            .unwrap_or_else(|e| panic!("{file}: {e}"));
        let protocol = doc["protocol"]
            .as_str()
            .unwrap_or_else(|| panic!("{file}: protocol"))
            .to_owned();
        assert!(
            ["artnet", "rdm", "image"].contains(&protocol.as_str()),
            "{file}: protocol {protocol}"
        );
        let packet = doc["packet"]
            .as_str()
            .unwrap_or_else(|| panic!("{file}: packet"))
            .to_owned();
        let vectors = doc["vectors"]
            .as_array()
            .unwrap_or_else(|| panic!("{file}: vectors"));
        assert!(!vectors.is_empty(), "{file}: empty vectors");
        let mut names = std::collections::HashSet::new();
        for v in vectors {
            for k in VECTOR_KEYS {
                assert!(v.get(k).is_some(), "{file}: vector missing key {k}");
            }
            let name = v["name"].as_str().unwrap();
            assert!(
                names.insert(name.to_owned()),
                "{file}: duplicate name {name}"
            );
            assert!(v["valid"].is_boolean(), "{file}:{name}: valid");
            assert!(v["fields"].is_object(), "{file}:{name}: fields");
            if !v["valid"].as_bool().unwrap() {
                assert!(
                    v["fields"]["error"].is_string(),
                    "{file}:{name}: invalid vector needs fields.error"
                );
            }
            let bytes = parse_hex(v["hex"].as_str().unwrap())
                .unwrap_or_else(|| panic!("{file}:{name}: bad hex"));
            out.push(Vector {
                file: file.clone(),
                protocol: protocol.clone(),
                packet: packet.clone(),
                v: v.clone(),
                bytes,
            });
        }
    }
    out
}

#[test]
fn all_vector_files_well_formed() {
    let all = load_all();
    eprintln!("{} vectors checked", all.len());
}

/// Every Art-Net packet type that has vectors and is checked by [`artnet_vectors`].
const ARTNET_PACKETS: [&str; 14] = [
    "ArtPoll",
    "ArtPollReply",
    "ArtDmx",
    "ArtSync",
    "ArtAddress",
    "ArtInput",
    "ArtIpProg",
    "ArtIpProgReply",
    "ArtTodRequest",
    "ArtTodData",
    "ArtTodControl",
    "ArtRdm",
    "ArtFirmwareMaster",
    "ArtFirmwareReply",
];

fn mac_str(m: &[u8; 6]) -> String {
    m.iter()
        .map(|b| format!("{b:02x}"))
        .collect::<Vec<_>>()
        .join(":")
}

/// Decode `bytes` as `packet` with the packet-specific decoder, wrapped in [`ArtPacket`].
fn decode_as(packet: &str, bytes: &[u8]) -> Result<ArtPacket, ArtNetError> {
    Ok(match packet {
        "ArtPoll" => ArtPacket::Poll(ArtPoll::decode(bytes)?),
        "ArtPollReply" => ArtPacket::PollReply(ArtPollReply::decode(bytes)?),
        "ArtDmx" => ArtPacket::Dmx(ArtDmx::decode(bytes)?),
        "ArtSync" => ArtPacket::Sync(ArtSync::decode(bytes)?),
        "ArtAddress" => ArtPacket::Address(ArtAddress::decode(bytes)?),
        "ArtInput" => ArtPacket::Input(ArtInput::decode(bytes)?),
        "ArtIpProg" => ArtPacket::IpProg(ArtIpProg::decode(bytes)?),
        "ArtIpProgReply" => ArtPacket::IpProgReply(ArtIpProgReply::decode(bytes)?),
        "ArtTodRequest" => ArtPacket::TodRequest(ArtTodRequest::decode(bytes)?),
        "ArtTodData" => ArtPacket::TodData(ArtTodData::decode(bytes)?),
        "ArtTodControl" => ArtPacket::TodControl(ArtTodControl::decode(bytes)?),
        "ArtRdm" => ArtPacket::Rdm(ArtRdm::decode(bytes)?),
        "ArtFirmwareMaster" => ArtPacket::FirmwareMaster(ArtFirmwareMaster::decode(bytes)?),
        "ArtFirmwareReply" => ArtPacket::FirmwareReply(ArtFirmwareReply::decode(bytes)?),
        other => panic!("no decoder for Art-Net packet {other}"),
    })
}

/// All fields of a decoded packet in the vector JSON conventions (vectors/README.md), including derived values.
fn artnet_fields(p: &ArtPacket, bytes: &[u8]) -> Map<String, Value> {
    let mut m = Map::new();
    let mut put = |k: &str, v: Value| {
        m.insert(k.to_owned(), v);
    };
    put("op_code", json!(p.op_code()));
    if !matches!(p, ArtPacket::PollReply(_)) {
        put("prot_ver", json!(Header::decode(bytes).unwrap().prot_ver));
    }
    let pas = |v: Vec<PortAddress>| json!(v.iter().map(|p| p.raw()).collect::<Vec<_>>());
    match p {
        ArtPacket::Poll(x) => {
            put("flags", json!(x.flags));
            put("diag_priority", json!(x.diag_priority));
            put("target_port_address_top", json!(x.target_port_address_top));
            put(
                "target_port_address_bottom",
                json!(x.target_port_address_bottom),
            );
            put("esta_man", json!(x.esta_man));
            put("oem", json!(x.oem));
            put("targeted_mode", json!(x.targeted_mode()));
            put("reply_on_change", json!(x.reply_on_change()));
        }
        ArtPacket::PollReply(x) => {
            put("ip", json!(x.ip.to_string()));
            put("port", json!(x.port));
            put("vers_info", json!(x.vers_info));
            put("net_switch", json!(x.net_switch));
            put("sub_switch", json!(x.sub_switch));
            put("oem", json!(x.oem));
            put("ubea_version", json!(x.ubea_version));
            put("status1", json!(x.status1));
            put("esta_man", json!(x.esta_man));
            put("short_name", json!(x.short_name));
            put("long_name", json!(x.long_name));
            put("node_report", json!(x.node_report));
            put("num_ports", json!(x.num_ports));
            put("port_types", json!(x.port_types));
            put("good_input", json!(x.good_input));
            put("good_output_a", json!(x.good_output_a));
            put("sw_in", json!(x.sw_in));
            put("sw_out", json!(x.sw_out));
            put(
                "port_addresses_out",
                pas((0..4).map(|i| x.port_address_out(i)).collect()),
            );
            put(
                "port_addresses_in",
                pas((0..4).map(|i| x.port_address_in(i)).collect()),
            );
            put("acn_priority", json!(x.acn_priority));
            put("sw_macro", json!(x.sw_macro));
            put("sw_remote", json!(x.sw_remote));
            put("style", json!(x.style));
            put("mac", json!(mac_str(&x.mac)));
            put("bind_ip", json!(x.bind_ip.to_string()));
            put("bind_index", json!(x.bind_index));
            put("status2", json!(x.status2));
            put("good_output_b", json!(x.good_output_b));
            put("status3", json!(x.status3));
            put("default_resp_uid", json!(x.default_resp_uid.to_string()));
            put("user", json!(x.user));
            put("refresh_rate", json!(x.refresh_rate));
            put("background_queue_policy", json!(x.background_queue_policy));
        }
        ArtPacket::Dmx(x) => {
            let pa = x.port_address;
            put("sequence", json!(x.sequence));
            put("physical", json!(x.physical));
            put("sub_uni", json!(pa.sub_uni()));
            put("net", json!(pa.net()));
            put("port_address", json!(pa.raw()));
            put("sub_net", json!(pa.sub_net()));
            put("universe", json!(pa.universe()));
            put("length", json!(x.data.len()));
            put("data", json!(to_hex(&x.data)));
        }
        ArtPacket::Sync(x) => {
            put("aux1", json!(x.aux1));
            put("aux2", json!(x.aux2));
        }
        ArtPacket::Address(x) => {
            put("net_switch", json!(x.net_switch));
            put("bind_index", json!(x.bind_index));
            put("short_name", json!(x.short_name));
            put("long_name", json!(x.long_name));
            put("sw_in", json!(x.sw_in));
            put("sw_out", json!(x.sw_out));
            put("sub_switch", json!(x.sub_switch));
            put("acn_priority", json!(x.acn_priority));
            put("command", json!(x.command));
            put(
                "port_addresses_out",
                pas((0..4).map(|i| x.port_address_out(i)).collect()),
            );
        }
        ArtPacket::Input(x) => {
            put("bind_index", json!(x.bind_index));
            put("num_ports", json!(x.num_ports));
            put("input", json!(x.input));
        }
        ArtPacket::IpProg(x) => {
            put("command", json!(x.command));
            put("prog_ip", json!(x.prog_ip.to_string()));
            put("prog_sm", json!(x.prog_sm.to_string()));
            put("prog_port", json!(x.prog_port));
            put("prog_dg", json!(x.prog_dg.to_string()));
        }
        ArtPacket::IpProgReply(x) => {
            put("prog_ip", json!(x.prog_ip.to_string()));
            put("prog_sm", json!(x.prog_sm.to_string()));
            put("prog_port", json!(x.prog_port));
            put("status", json!(x.status));
            put("dhcp_enabled", json!(x.dhcp_enabled()));
            put("prog_dg", json!(x.prog_dg.to_string()));
        }
        ArtPacket::TodRequest(x) => {
            put("net", json!(x.net));
            put("command", json!(x.command));
            put("add_count", json!(x.addresses.len()));
            put("address", json!(x.addresses));
            put("port_addresses", pas(x.port_addresses()));
        }
        ArtPacket::TodData(x) => {
            put("rdm_ver", json!(x.rdm_ver));
            put("port", json!(x.port));
            put("bind_index", json!(x.bind_index));
            put("net", json!(x.net));
            put("command_response", json!(x.command_response));
            put("address", json!(x.address));
            put("port_address", json!(x.port_address().raw()));
            put("uid_total", json!(x.uid_total));
            put("block_count", json!(x.block_count));
            put("uid_count", json!(x.uids.len()));
            put(
                "tod",
                json!(x.uids.iter().map(|u| u.to_string()).collect::<Vec<_>>()),
            );
        }
        ArtPacket::TodControl(x) => {
            put("net", json!(x.net));
            put("command", json!(x.command));
            put("address", json!(x.address));
            put("port_address", json!(x.port_address().raw()));
        }
        ArtPacket::Rdm(x) => {
            put("rdm_ver", json!(x.rdm_ver));
            put("fifo_avail", json!(x.fifo_avail));
            put("fifo_max", json!(x.fifo_max));
            put("net", json!(x.net));
            put("command", json!(x.command));
            put("address", json!(x.address));
            put("port_address", json!(x.port_address().raw()));
            put("rdm_packet", json!(to_hex(&x.rdm_packet)));
        }
        ArtPacket::FirmwareMaster(x) => {
            put("type", json!(x.kind));
            put("block_id", json!(x.block_id));
            put("firmware_length", json!(x.firmware_length));
            put("data", json!(to_hex(&x.data)));
            // Derived: .alf file header (digest §6.3) carried in the data blocks. Header bytes 0..1024 are
            // block 0, the file's Length field (offset 1056) lies at offset 32 of block 1.
            let be16 = |o: usize| u16::from_be_bytes([x.data[o], x.data[o + 1]]);
            if x.block_id == 0 {
                put("file_checksum", json!(be16(0)));
                put("file_vers_info", json!(be16(2)));
                let name = &x.data[4..34];
                let end = name.iter().position(|&b| b == 0).unwrap_or(name.len());
                put(
                    "file_user_name",
                    json!(String::from_utf8_lossy(&name[..end])),
                );
                let oem: Vec<u16> = (0..256)
                    .map(|i| be16(34 + 2 * i))
                    .filter(|&w| w != 0)
                    .collect();
                put("file_oem", json!(oem));
            } else if x.block_id == 1 {
                let l = &x.data[32..36];
                put(
                    "file_length",
                    json!(u32::from_be_bytes([l[0], l[1], l[2], l[3]])),
                );
            }
        }
        ArtPacket::FirmwareReply(x) => {
            put("type", json!(x.kind));
        }
        other => panic!("unexpected packet {other:?}"),
    }
    m
}

fn error_name(e: &ArtNetError) -> &'static str {
    match e {
        ArtNetError::Short => "short",
        ArtNetError::BadId => "bad_id",
        ArtNetError::ProtVer(_) => "prot_ver",
        ArtNetError::UnexpectedOpCode(_) => "op_code",
        ArtNetError::InvalidField(_) => "invalid_field",
    }
}

#[test]
fn artnet_vectors() {
    let all = load_all();
    let mut n = 0;
    for v in all.iter().filter(|v| v.protocol == "artnet") {
        assert!(
            ARTNET_PACKETS.contains(&v.packet.as_str()),
            "{}: packet {} has no test",
            v.id(),
            v.packet
        );
        n += 1;
        let specific = decode_as(&v.packet, &v.bytes);
        let generic = ArtPacket::decode(&v.bytes);
        if !v.valid() {
            let want = v.field("error").as_str().unwrap();
            for r in [&specific, &generic] {
                match r {
                    Ok(p) => panic!("{} must be rejected, decoded {p:?}", v.id()),
                    Err(e) => assert_eq!(error_name(e), want, "{}: {e}", v.id()),
                }
            }
            continue;
        }
        let p = specific.unwrap_or_else(|e| panic!("{}: {e}", v.id()));
        assert_eq!(generic.as_ref(), Ok(&p), "{} ArtPacket::decode", v.id());
        let actual = artnet_fields(&p, &v.bytes);
        let expected = v.v["fields"].as_object().unwrap();
        for (k, want) in expected {
            let got = actual
                .get(k)
                .unwrap_or_else(|| panic!("{}: field {k} not compared by the test", v.id()));
            assert_eq!(got, want, "{}: field {k}", v.id());
        }
        let encoded = p.encode();
        if encoded.len() == v.bytes.len() {
            assert_eq!(to_hex(&encoded), to_hex(&v.bytes), "{} re-encode", v.id());
        } else {
            // Minimum-length vectors (e.g. poll_min_14_bytes, min_207_bytes): decode only.
            assert!(
                v.bytes.len() < encoded.len(),
                "{}: vector longer than the encoding",
                v.id()
            );
            assert_eq!(
                encoded[..v.bytes.len()],
                v.bytes[..],
                "{}: prefix of the re-encoding",
                v.id()
            );
        }
    }
    assert!(n > 0, "no Art-Net vectors");
}

#[test]
fn every_artnet_packet_has_vectors() {
    let all = load_all();
    for name in ARTNET_PACKETS {
        assert!(
            all.iter()
                .any(|v| v.protocol == "artnet" && v.packet == name && v.valid()),
            "no valid vector for {name}"
        );
    }
}

fn uid_field(v: &Vector, k: &str) -> Uid {
    v.field(k)
        .as_str()
        .unwrap_or_else(|| panic!("{}: {k}", v.id()))
        .parse()
        .unwrap()
}

#[test]
fn rdm_messages() {
    let mut n = 0;
    let all = load_all();
    // Files whose valid vectors are plain RDM messages (start code 0xCC).
    let rdm: Vec<_> = all
        .iter()
        .filter(|v| v.protocol == "rdm")
        .filter(|v| {
            all.iter()
                .filter(|o| o.file == v.file && o.valid())
                .all(|o| o.bytes.first() == Some(&0xCC))
        })
        .collect();
    for v in rdm {
        n += 1;
        let r = RdmMessage::decode(&v.bytes);
        if !v.valid() {
            assert!(r.is_err(), "{} must be rejected", v.id());
            continue;
        }
        let m = r.unwrap_or_else(|e| panic!("{}: {e}", v.id()));
        assert_eq!(
            u64::from(m.message_length()),
            v.num("message_length"),
            "{}",
            v.id()
        );
        assert_eq!(m.dest_uid, uid_field(v, "dest_uid"), "{}", v.id());
        assert_eq!(m.src_uid, uid_field(v, "src_uid"), "{}", v.id());
        assert_eq!(
            u64::from(m.transaction_number),
            v.num("transaction_number"),
            "{}",
            v.id()
        );
        let byte16 = v
            .opt_num("port_id")
            .or(v.opt_num("response_type"))
            .expect("port_id/response_type");
        assert_eq!(u64::from(m.port_id_or_response_type), byte16, "{}", v.id());
        assert_eq!(
            u64::from(m.message_count),
            v.num("message_count"),
            "{}",
            v.id()
        );
        // RDM_DISC_MUTE.json reuses `sub_device` as a ControlField flag (bool); only compare numeric values.
        // The full re-encode below still checks the sub-device bytes.
        match v.opt_num("sub_device") {
            Some(sd) => assert_eq!(u64::from(m.sub_device), sd, "{}", v.id()),
            None => eprintln!("warning: {}: fields.sub_device is not a number", v.id()),
        }
        assert_eq!(
            u64::from(m.command_class),
            v.num("command_class"),
            "{}",
            v.id()
        );
        assert_eq!(
            u64::from(m.parameter_id),
            v.num("parameter_id"),
            "{}",
            v.id()
        );
        assert_eq!(
            m.parameter_data.len() as u64,
            v.num("parameter_data_length"),
            "{}",
            v.id()
        );
        assert_eq!(
            to_hex(&m.parameter_data),
            v.field("parameter_data").as_str().unwrap().to_lowercase(),
            "{}",
            v.id()
        );
        assert_eq!(m.encode(), v.bytes, "{} re-encode", v.id());
    }
    assert!(n > 0, "no RDM message vectors");
}

#[test]
fn rdm_dub_responses() {
    let mut n = 0;
    for v in load_all()
        .iter()
        .filter(|v| v.protocol == "rdm" && v.packet == "DUB_RESPONSE")
    {
        n += 1;
        let r = DubResponse::decode(&v.bytes);
        if !v.valid() {
            assert!(r.is_err(), "{} must be rejected", v.id());
            continue;
        }
        let d = r.unwrap_or_else(|e| panic!("{}: {e}", v.id()));
        assert_eq!(d.uid, uid_field(v, "uid"), "{}", v.id());
        assert_eq!(
            d.preamble_length as u64,
            v.num("preamble_length"),
            "{}",
            v.id()
        );
        assert_eq!(u64::from(d.checksum), v.num("checksum"), "{}", v.id());
        if d.preamble_length == 7 {
            assert_eq!(DubResponse::encode(d.uid), v.bytes, "{} re-encode", v.id());
        }
    }
    assert!(n > 0, "no DUB vectors");
}
