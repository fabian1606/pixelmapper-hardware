//! `rdm get|set|describe`.

use crate::decode::{decode, pid_label};
use crate::net;
use crate::out::{pretty, to_value, Out};
use crate::{Global, RdmCmd};
use pixelmapper_net::link::RdmOptions;
use pixelmapper_net::rdm::{cc, pid};
use pixelmapper_net::{Error, RdmRequest, Result};
use serde_json::json;

/// Parse a PID argument (name, hex, decimal).
pub fn parse_pid(s: &str) -> Result<u16> {
    pid::parse(s).ok_or_else(|| Error::InvalidArgument(format!("unknown PID {s:?}")))
}

/// Parse hex parameter data (`None` = empty).
pub fn parse_data(s: Option<&str>) -> Result<Vec<u8>> {
    match s {
        None => Ok(Vec::new()),
        Some(h) => {
            let t = h.trim();
            let t = t.strip_prefix("0x").unwrap_or(t);
            pixelmapper_net::parse_hex(t)
                .ok_or_else(|| Error::InvalidArgument(format!("bad hex data {h:?}")))
        }
    }
}

/// Print the result of a GET/SET.
pub fn print_response(o: &Out, uid: &str, p: u16, set: bool, data: &[u8]) {
    let decoded = decode(p, data);
    let hex = pixelmapper_net::to_hex(data);
    o.emit(
        json!({
            "type": "rdm",
            "uid": uid,
            "commandClass": if set { "set" } else { "get" },
            "pid": p,
            "pidName": pid::name(p),
            "data": hex,
            "decoded": decoded,
        }),
        || {
            let mut s = format!(
                "{uid} {} {}: {} byte(s) {}",
                if set { "SET" } else { "GET" },
                pid_label(p),
                data.len(),
                if hex.is_empty() { "(ack)" } else { &hex }
            );
            if let Some(d) = &decoded {
                match d {
                    serde_json::Value::String(t) => s.push_str(&format!("\n  \"{t}\"")),
                    serde_json::Value::Number(n) => s.push_str(&format!("\n  = {n}")),
                    v => {
                        s.push('\n');
                        s.push_str(&pretty(v, 2));
                    }
                }
            }
            s
        },
    );
}

/// `rdm get|set <uid> <pid> [hex]`.
pub async fn get_set(g: &Global, o: &Out, c: RdmCmd, set: bool) -> Result<()> {
    let uid = net::parse_uid(&c.uid)?;
    let p = parse_pid(&c.pid)?;
    let data = parse_data(c.data.as_deref())?;
    let ctrl = net::controller(g).await?;
    let node = net::select_node(&ctrl, g).await?;
    let pa = net::port_for_uid(&ctrl, g, &node, uid).await?;
    let req = RdmRequest {
        uid,
        command_class: if set { cc::SET } else { cc::GET },
        pid: p,
        data,
        options: RdmOptions {
            sub_device: c.sub_device,
            ..RdmOptions::default()
        },
    };
    let r = ctrl.rdm(&node.id, pa, req).await;
    ctrl.shutdown();
    let resp = r?;
    print_response(o, &uid.to_string(), p, set, &resp);
    Ok(())
}

/// `rdm describe <uid>`.
pub async fn describe(g: &Global, o: &Out, uid: &str) -> Result<()> {
    let uid = net::parse_uid(uid)?;
    let ctrl = net::controller(g).await?;
    let node = net::select_node(&ctrl, g).await?;
    let pa = net::port_for_uid(&ctrl, g, &node, uid).await?;
    let r = ctrl.describe_device(&node.id, pa, uid).await;
    ctrl.shutdown();
    let d = r?;
    o.emit(
        json!({"type": "device", "node": node.id, "portAddress": pa, "device": to_value(&d)}),
        || {
            let i = &d.device_info;
            let opt = |v: &Option<String>| v.clone().unwrap_or_else(|| "-".into());
            let mut s = format!(
                "{} on {} port {pa}\n  manufacturer {}\n  model {} (0x{:04X}, category 0x{:04X})\n  label {}\n  software {} (0x{:08X})  boot {}\n  footprint {}  start {}  personality {}/{}  sub-devices {}  sensors {}",
                d.uid,
                node.id,
                opt(&d.manufacturer_label),
                opt(&d.model_description),
                i.device_model_id,
                i.product_category,
                opt(&d.device_label),
                opt(&d.software_version_label),
                i.software_version_id,
                opt(&d.boot_software_version_label),
                i.dmx_footprint,
                i.dmx_start_address,
                i.current_personality,
                i.personality_count,
                i.sub_device_count,
                i.sensor_count,
            );
            s.push_str("\n  supported parameters:");
            for p in &d.supported_parameters {
                s.push_str(&format!("\n    {}", pid_label(*p)));
            }
            for pd in &d.parameter_descriptions {
                s.push_str(&format!(
                    "\n  0x{:04X} \"{}\": pdl {} type {} cc {} range {}..{} default {}",
                    pd.pid,
                    pd.description,
                    pd.pdl_size,
                    pixelmapper_net::rdm::pd::data_type_name(pd.data_type).unwrap_or("?"),
                    pd.command_class,
                    pd.min_valid_value,
                    pd.max_valid_value,
                    pd.default_value
                ));
            }
            for p in &d.personalities {
                s.push_str(&format!("\n  personality {}", to_value(p)));
            }
            for sl in &d.slots {
                s.push_str(&format!("\n  slot {}", to_value(sl)));
            }
            for se in &d.sensors {
                s.push_str(&format!("\n  sensor {}", to_value(se)));
            }
            for w in &d.warnings {
                s.push_str(&format!("\n  warning: {w}"));
            }
            s
        },
    );
    Ok(())
}
