//! Checks the parameter-data (PD) level fields of `protocol/vectors/rdm/*.json` against the codecs in
//! `pixelmapper_net::rdm::pd` and re-encodes each decoded PD. Header-level fields are covered by `vectors.rs`.

use pixelmapper_net::rdm::pd::{
    decode_u16, decode_u8, BootInfo, DeviceInfo, DiscMuteResponse, DiscUniqueBranch,
    ParameterDescription, StatusMessage,
};
use pixelmapper_net::rdm::{pid, RdmMessage, Uid};
use pixelmapper_net::{parse_hex, to_hex};
use serde_json::{Map, Value};
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

fn rdm_dir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../protocol/vectors/rdm")
}

struct Vector {
    id: String,
    v: Value,
    bytes: Vec<u8>,
}

impl Vector {
    fn valid(&self) -> bool {
        self.v["valid"].as_bool().unwrap()
    }
    fn fields(&self) -> &Map<String, Value> {
        self.v["fields"].as_object().unwrap()
    }
    fn field(&self, k: &str) -> Option<&Value> {
        self.fields().get(k)
    }
}

fn load_all() -> Vec<Vector> {
    let mut files: Vec<_> = std::fs::read_dir(rdm_dir())
        .unwrap_or_else(|e| panic!("read_dir {}: {e}", rdm_dir().display()))
        .map(|e| e.unwrap().path())
        .filter(|p| p.extension().is_some_and(|x| x == "json"))
        .collect();
    files.sort();
    assert!(!files.is_empty(), "no RDM vector files");
    let mut out = Vec::new();
    for path in files {
        let file = path.file_name().unwrap().to_string_lossy().into_owned();
        let doc: Value = serde_json::from_str(&std::fs::read_to_string(&path).unwrap())
            .unwrap_or_else(|e| panic!("{file}: {e}"));
        for v in doc["vectors"].as_array().unwrap() {
            let id = format!("{file}:{}", v["name"].as_str().unwrap());
            let bytes =
                parse_hex(v["hex"].as_str().unwrap()).unwrap_or_else(|| panic!("{id}: bad hex"));
            out.push(Vector {
                id,
                v: v.clone(),
                bytes,
            });
        }
    }
    out
}

/// Valid vectors that are complete RDM messages (start code 0xCC), decoded.
fn messages() -> Vec<(Vector, RdmMessage)> {
    load_all()
        .into_iter()
        .filter(|v| v.valid() && v.bytes.first() == Some(&0xCC))
        .map(|v| {
            let m = RdmMessage::decode(&v.bytes).unwrap_or_else(|e| panic!("{}: {e}", v.id));
            (v, m)
        })
        .collect()
}

fn num(id: &str, obj: &Value, keys: &[&str]) -> u64 {
    keys.iter()
        .find_map(|k| obj.get(*k))
        .and_then(Value::as_u64)
        .unwrap_or_else(|| panic!("{id}: none of {keys:?} is a number"))
}

fn uid(id: &str, v: &Value) -> Uid {
    v.as_str()
        .and_then(|s| s.parse().ok())
        .unwrap_or_else(|| panic!("{id}: bad UID {v}"))
}

/// Header-level keys checked by `vectors.rs`.
const HEADER_KEYS: [&str; 16] = [
    "start_code",
    "sub_start_code",
    "message_length",
    "dest_uid",
    "src_uid",
    "transaction_number",
    "port_id",
    "response_type",
    "message_count",
    "sub_device",
    "command_class",
    "parameter_id",
    "parameter_data_length",
    "parameter_data",
    "checksum",
    "error",
];

/// PD-level keys this file checks.
const PD_KEYS: [&str; 13] = [
    "device_info",
    "parameter_description",
    "boot_info",
    "control_field",
    "managed_proxy",
    "boot_loader",
    "proxied_device",
    "sub_device_flag",
    "binding_uid",
    "nack_reason",
    "status_messages",
    "status_type",
    "requested_pid",
];

#[test]
fn pd_fields() {
    let mut checked: BTreeMap<&str, usize> = BTreeMap::new();
    let mut hit = |k: &'static str| *checked.entry(k).or_default() += 1;

    for (v, m) in messages() {
        let id = v.id.as_str();
        let pd = m.parameter_data.as_slice();
        assert_eq!(
            to_hex(pd),
            v.field("parameter_data")
                .and_then(Value::as_str)
                .unwrap_or_default()
                .to_lowercase(),
            "{id}"
        );
        for k in v.fields().keys() {
            let known = HEADER_KEYS.contains(&k.as_str())
                || PD_KEYS.contains(&k.as_str())
                || k == "lower_bound_uid"
                || k == "upper_bound_uid";
            if !known {
                eprintln!("warning: {id}: field {k} is not checked by vectors_rdm.rs");
            }
        }

        if let Some(e) = v.field("device_info") {
            let d = DeviceInfo::decode(pd).unwrap_or_else(|e| panic!("{id}: {e}"));
            let n = |k: &str| num(id, e, &[k]);
            assert_eq!(
                u64::from(d.rdm_protocol_version),
                n("rdm_protocol_version"),
                "{id}"
            );
            assert_eq!(u64::from(d.device_model_id), n("device_model_id"), "{id}");
            assert_eq!(u64::from(d.product_category), n("product_category"), "{id}");
            assert_eq!(
                u64::from(d.software_version_id),
                n("software_version_id"),
                "{id}"
            );
            assert_eq!(u64::from(d.dmx_footprint), n("dmx_footprint"), "{id}");
            assert_eq!(
                u64::from(d.current_personality),
                n("current_personality"),
                "{id}"
            );
            assert_eq!(
                u64::from(d.personality_count),
                n("personality_count"),
                "{id}"
            );
            assert_eq!(
                u64::from(d.dmx_start_address),
                n("dmx_start_address"),
                "{id}"
            );
            assert_eq!(u64::from(d.sub_device_count), n("sub_device_count"), "{id}");
            assert_eq!(u64::from(d.sensor_count), n("sensor_count"), "{id}");
            assert_eq!(d.encode(), pd, "{id} re-encode");
            hit("device_info");
        }

        if let Some(e) = v.field("parameter_description") {
            let d = ParameterDescription::decode(pd).unwrap_or_else(|e| panic!("{id}: {e}"));
            let n = |k: &str| num(id, e, &[k]);
            assert_eq!(u64::from(d.pid), n("pid"), "{id}");
            assert_eq!(u64::from(d.pdl_size), n("pdl_size"), "{id}");
            assert_eq!(u64::from(d.data_type), n("data_type"), "{id}");
            assert_eq!(u64::from(d.command_class), n("command_class"), "{id}");
            assert_eq!(u64::from(d.param_type), n("type"), "{id}");
            assert_eq!(u64::from(d.unit), n("unit"), "{id}");
            assert_eq!(u64::from(d.prefix), n("prefix"), "{id}");
            assert_eq!(u64::from(d.min_valid_value), n("min_valid_value"), "{id}");
            assert_eq!(u64::from(d.max_valid_value), n("max_valid_value"), "{id}");
            assert_eq!(u64::from(d.default_value), n("default_value"), "{id}");
            assert_eq!(
                Some(d.description.as_str()),
                e["description"].as_str(),
                "{id}"
            );
            assert_eq!(d.encode(), pd, "{id} re-encode");
            // A manufacturer PID's description must match what pids.toml would answer.
            if let Some(def) = pixelmapper_net::pids::PIDS.iter().find(|p| p.pid == d.pid) {
                assert_eq!(
                    ParameterDescription::from_pid_def(def).as_ref(),
                    Some(&d),
                    "{id}: differs from pids.toml"
                );
            }
            hit("parameter_description");
        }

        if let Some(e) = v.field("boot_info") {
            let b = BootInfo::decode(pd).unwrap_or_else(|e| panic!("{id}: {e}"));
            // Accept both the pids.toml field names and the names used in RDM_TUBE_BOOT_INFO.json.
            let n = |ks: &[&str]| num(id, e, ks);
            assert_eq!(u64::from(b.version), n(&["version"]), "{id}");
            assert_eq!(u64::from(b.page_size), n(&["page_size"]), "{id}");
            assert_eq!(
                u64::from(b.app_end),
                n(&["app_end", "app_space_end"]),
                "{id}"
            );
            assert_eq!(u64::from(b.busy), n(&["busy"]), "{id}");
            assert_eq!(u64::from(b.last_error), n(&["last_error"]), "{id}");
            assert_eq!(
                u64::from(b.manufacturer),
                n(&["manufacturer", "manufacturer_id"]),
                "{id}"
            );
            assert_eq!(u64::from(b.model), n(&["model", "device_model_id"]), "{id}");
            assert_eq!(
                u64::from(b.boot_sw_version),
                n(&["boot_sw_version", "boot_sw_version_id"]),
                "{id}"
            );
            assert_eq!(u64::from(b.app_valid), n(&["app_valid"]), "{id}");
            assert_eq!(
                u64::from(b.app_fw_version),
                n(&["app_fw_version", "app_fw_version_id"]),
                "{id}"
            );
            assert_eq!(b.encode(), pd, "{id} re-encode");
            hit("boot_info");
        }

        let is_mute = m.parameter_id == pid::DISC_MUTE || m.parameter_id == pid::DISC_UN_MUTE;
        if is_mute && m.is_response() {
            let d = DiscMuteResponse::decode(pd).unwrap_or_else(|e| panic!("{id}: {e}"));
            let flag = |k: &str| v.field(k).map(|x| x.as_bool().expect("bool flag"));
            if let Some(cf) = v.field("control_field") {
                assert_eq!(Some(u64::from(d.control_field)), cf.as_u64(), "{id}");
                hit("control_field");
            }
            for (k, got) in [
                ("managed_proxy", d.managed_proxy()),
                ("boot_loader", d.boot_loader()),
                ("proxied_device", d.proxied_device()),
                ("sub_device_flag", d.sub_device()),
            ] {
                if let Some(want) = flag(k) {
                    assert_eq!(got, want, "{id}: {k}");
                    hit("control_field flags");
                }
            }
            match v.field("binding_uid") {
                Some(b) => {
                    assert_eq!(d.binding_uid, Some(uid(id, b)), "{id}");
                    hit("binding_uid");
                }
                None => assert_eq!(d.binding_uid, None, "{id}"),
            }
            assert_eq!(d.encode(), pd, "{id} re-encode");
        }

        if let Some(r) = v.field("nack_reason") {
            let got = m.nack_reason().unwrap_or_else(|| panic!("{id}: no NACK"));
            assert_eq!(Some(u64::from(got.0)), r.as_u64(), "{id}");
            assert!(got.name().is_some(), "{id}: unnamed NACK {got}");
            hit("nack_reason");
        }

        if let Some(list) = v.field("status_messages") {
            let want = list.as_array().expect("status_messages array");
            let got = StatusMessage::decode_list(pd).unwrap_or_else(|e| panic!("{id}: {e}"));
            assert_eq!(got.len(), want.len(), "{id}");
            for (g, w) in got.iter().zip(want) {
                let n = |k: &str| num(id, w, &[k]);
                assert_eq!(u64::from(g.sub_device), n("sub_device"), "{id}");
                assert_eq!(u64::from(g.status_type), n("status_type"), "{id}");
                assert_eq!(
                    u64::from(g.status_message_id),
                    n("status_message_id"),
                    "{id}"
                );
                assert_eq!(Some(i64::from(g.data_value1)), w["data_value1"].as_i64());
                assert_eq!(Some(i64::from(g.data_value2)), w["data_value2"].as_i64());
            }
            assert_eq!(StatusMessage::encode_list(&got), pd, "{id} re-encode");
            hit("status_messages");
        }

        if let Some(t) = v.field("status_type") {
            assert_eq!(decode_u8(pd).ok().map(u64::from), t.as_u64(), "{id}");
            hit("status_type");
        }

        if let Some(p) = v.field("requested_pid") {
            assert_eq!(decode_u16(pd).ok().map(u64::from), p.as_u64(), "{id}");
            hit("requested_pid");
        }

        if v.field("lower_bound_uid").is_some() || v.field("upper_bound_uid").is_some() {
            let d = DiscUniqueBranch::decode(pd).unwrap_or_else(|e| panic!("{id}: {e}"));
            let want = |k: &str| uid(id, v.field(k).unwrap_or(&Value::Null));
            assert_eq!(d.lower_bound_uid, want("lower_bound_uid"), "{id}");
            assert_eq!(d.upper_bound_uid, want("upper_bound_uid"), "{id}");
            assert_eq!(d.encode(), pd, "{id} re-encode");
            hit("dub bounds");
        }
    }

    eprintln!("PD checks: {checked:?}");
    for k in [
        "device_info",
        "parameter_description",
        "boot_info",
        "control_field",
        "control_field flags",
        "binding_uid",
        "nack_reason",
        "status_messages",
        "status_type",
        "requested_pid",
        "dub bounds",
    ] {
        assert!(checked.contains_key(k), "no vector exercised {k}");
    }
}

#[test]
fn response_helpers_match_vectors() {
    // Every response vector must be reproducible from its request with `response_to` (same TN/PID/sub-device),
    // except QUEUED_MESSAGE whose response PID differs by design.
    let msgs = messages();
    let mut n = 0;
    for (v, rsp) in msgs.iter().filter(|(_, m)| m.is_response()) {
        let Some((_, req)) = msgs.iter().find(|(o, q)| {
            !q.is_response()
                && q.transaction_number == rsp.transaction_number
                && q.src_uid == rsp.dest_uid
                && q.dest_uid == rsp.src_uid
                && o.id.split(':').next() == v.id.split(':').next()
        }) else {
            continue;
        };
        if req.parameter_id == pid::QUEUED_MESSAGE {
            continue;
        }
        let rt = rsp.response_type().expect("valid response type");
        let built = req.response_to(
            rsp.src_uid,
            rt,
            rsp.message_count,
            rsp.parameter_data.clone(),
        );
        assert_eq!(&built, rsp, "{}", v.id);
        assert_eq!(built.try_encode().unwrap(), v.bytes, "{}", v.id);
        n += 1;
    }
    assert!(n > 0, "no request/response pairs");
}
