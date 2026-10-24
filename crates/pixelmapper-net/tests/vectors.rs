//! Checks every file under `protocol/vectors/` against the schema basics and decodes the packets this crate
//! implements (ArtPoll, ArtDmx, basic RDM messages, DUB responses).

use pixelmapper_net::artnet::{ArtDmx, ArtPoll, Header};
use pixelmapper_net::rdm::{DubResponse, RdmMessage, Uid};
use pixelmapper_net::{parse_hex, to_hex};
use serde_json::Value;
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

#[test]
fn artnet_artpoll() {
    let mut n = 0;
    for v in load_all()
        .iter()
        .filter(|v| v.protocol == "artnet" && v.packet == "ArtPoll")
    {
        n += 1;
        let r = ArtPoll::decode(&v.bytes);
        if !v.valid() {
            assert!(r.is_err(), "{} must be rejected", v.id());
            continue;
        }
        let p = r.unwrap_or_else(|e| panic!("{}: {e}", v.id()));
        let h = Header::decode(&v.bytes).unwrap();
        assert_eq!(u64::from(h.op_code), v.num("op_code"), "{}", v.id());
        assert_eq!(u64::from(h.prot_ver), v.num("prot_ver"), "{}", v.id());
        assert_eq!(u64::from(p.flags), v.num("flags"), "{}", v.id());
        assert_eq!(
            u64::from(p.diag_priority),
            v.num("diag_priority"),
            "{}",
            v.id()
        );
        assert_eq!(
            u64::from(p.target_port_address_top),
            v.num("target_port_address_top"),
            "{}",
            v.id()
        );
        assert_eq!(
            u64::from(p.target_port_address_bottom),
            v.num("target_port_address_bottom"),
            "{}",
            v.id()
        );
        assert_eq!(u64::from(p.esta_man), v.num("esta_man"), "{}", v.id());
        assert_eq!(u64::from(p.oem), v.num("oem"), "{}", v.id());
        if let Some(t) = v.field("targeted_mode").as_bool() {
            assert_eq!(p.targeted_mode(), t, "{}", v.id());
        }
        if let Some(t) = v.field("reply_on_change").as_bool() {
            assert_eq!(p.reply_on_change(), t, "{}", v.id());
        }
        if v.bytes.len() == ArtPoll::LEN {
            assert_eq!(p.encode(), v.bytes, "{} re-encode", v.id());
        }
    }
    assert!(n > 0, "no ArtPoll vectors");
}

#[test]
fn artnet_artdmx() {
    let mut n = 0;
    for v in load_all()
        .iter()
        .filter(|v| v.protocol == "artnet" && v.packet == "ArtDmx")
    {
        n += 1;
        let r = ArtDmx::decode(&v.bytes);
        if !v.valid() {
            assert!(r.is_err(), "{} must be rejected", v.id());
            continue;
        }
        let d = r.unwrap_or_else(|e| panic!("{}: {e}", v.id()));
        let h = Header::decode(&v.bytes).unwrap();
        assert_eq!(u64::from(h.op_code), v.num("op_code"), "{}", v.id());
        assert_eq!(u64::from(h.prot_ver), v.num("prot_ver"), "{}", v.id());
        assert_eq!(u64::from(d.sequence), v.num("sequence"), "{}", v.id());
        assert_eq!(u64::from(d.physical), v.num("physical"), "{}", v.id());
        let pa = d.port_address;
        assert_eq!(u64::from(pa.raw()), v.num("port_address"), "{}", v.id());
        assert_eq!(u64::from(pa.sub_uni()), v.num("sub_uni"), "{}", v.id());
        assert_eq!(u64::from(pa.net()), v.num("net"), "{}", v.id());
        assert_eq!(u64::from(pa.sub_net()), v.num("sub_net"), "{}", v.id());
        assert_eq!(u64::from(pa.universe()), v.num("universe"), "{}", v.id());
        assert_eq!(d.data.len() as u64, v.num("length"), "{}", v.id());
        assert_eq!(
            to_hex(&d.data),
            v.field("data").as_str().unwrap(),
            "{}",
            v.id()
        );
        assert_eq!(d.encode(), v.bytes, "{} re-encode", v.id());
    }
    assert!(n > 0, "no ArtDmx vectors");
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
