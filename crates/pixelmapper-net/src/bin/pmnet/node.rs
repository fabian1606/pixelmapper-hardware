//! `node config …`: ArtAddress, ArtIpProg, ArtInput and the node's manufacturer PIDs.

use crate::decode::{decode, pid_def};
use crate::net;
use crate::out::{to_value, Out};
use crate::poll::node_text;
use crate::rdm::print_response;
use crate::Global;
use clap::{Subcommand, ValueEnum};
use pixelmapper_net::artnet::{address_command as ac, ArtAddress, IpProgChange};
use pixelmapper_net::pids::{self, Device};
use pixelmapper_net::rdm::Uid;
use pixelmapper_net::{Controller, Error, NodeState, Result};
use serde_json::{json, Value};
use std::net::Ipv4Addr;

/// `node config …`
#[derive(Subcommand, Debug)]
pub enum ConfigAction {
    /// Show the node state and its manufacturer settings.
    Show,
    /// Set short and/or long name (ArtAddress).
    Name {
        /// Short name (≤ 17 characters).
        #[arg(long)]
        short: Option<String>,
        /// Long name (≤ 63 characters).
        #[arg(long)]
        long: Option<String>,
    },
    /// Configure one port (ArtAddress to its BindIndex).
    Port {
        /// BindIndex = port number 1..4.
        #[arg(long)]
        bind: u8,
        /// Port-Address (N:S:U or number).
        #[arg(long)]
        universe: Option<String>,
        /// Direction.
        #[arg(long, value_enum)]
        direction: Option<Direction>,
        /// Merge mode.
        #[arg(long, value_enum)]
        merge: Option<Merge>,
        /// RDM on/off.
        #[arg(long, value_enum)]
        rdm: Option<OnOff>,
        /// Output style.
        #[arg(long, value_enum)]
        style: Option<Style>,
    },
    /// Front panel indicators.
    Led {
        /// Mode.
        #[arg(value_enum)]
        mode: Led,
    },
    /// Failsafe mode (or record the current output as failsafe scene).
    Failsafe {
        /// Mode.
        #[arg(value_enum)]
        mode: Failsafe,
    },
    /// IP configuration (ArtIpProg); without options only the current values are read.
    Ip {
        /// Static IP.
        #[arg(long, conflicts_with_all = ["dhcp", "reset"])]
        ip: Option<Ipv4Addr>,
        /// Subnet mask.
        #[arg(long, conflicts_with_all = ["dhcp", "reset"])]
        mask: Option<Ipv4Addr>,
        /// Default gateway.
        #[arg(long, conflicts_with_all = ["dhcp", "reset"])]
        gateway: Option<Ipv4Addr>,
        /// Enable DHCP.
        #[arg(long, conflicts_with = "reset")]
        dhcp: bool,
        /// Reset IP settings to the defaults.
        #[arg(long)]
        reset: bool,
    },
    /// Enable/disable DMX inputs (ArtInput); inputs not listed in --disable are enabled.
    Input {
        /// BindIndex = port number 1..4.
        #[arg(long)]
        bind: u8,
        /// Inputs (1-based within the BindIndex page) to disable, comma-separated.
        #[arg(long, value_delimiter = ',')]
        disable: Vec<u8>,
    },
    /// Set a node manufacturer PID (RDM SET to the node UID).
    Set {
        /// PID (name, 0xHEX or decimal).
        pid: String,
        /// Value: number, on/off/true/false, text for ASCII PIDs, or `hex:…` for raw data.
        value: String,
    },
}

/// Port direction.
#[derive(ValueEnum, Debug, Clone, Copy)]
pub enum Direction {
    /// DMX input (DMX → Art-Net).
    In,
    /// DMX output (Art-Net → DMX).
    Out,
}

/// Merge mode.
#[derive(ValueEnum, Debug, Clone, Copy)]
pub enum Merge {
    /// Highest takes precedence.
    Htp,
    /// Latest takes precedence.
    Ltp,
}

/// On/off.
#[derive(ValueEnum, Debug, Clone, Copy)]
pub enum OnOff {
    /// On.
    On,
    /// Off.
    Off,
}

/// Output style.
#[derive(ValueEnum, Debug, Clone, Copy)]
pub enum Style {
    /// Output only on ArtDmx.
    Delta,
    /// Continuous output.
    Const,
}

/// Indicator mode.
#[derive(ValueEnum, Debug, Clone, Copy)]
pub enum Led {
    /// Normal.
    Normal,
    /// Display off.
    Mute,
    /// Locate.
    Locate,
}

/// Failsafe mode.
#[derive(ValueEnum, Debug, Clone, Copy)]
pub enum Failsafe {
    /// Hold the last frame.
    Hold,
    /// All zero.
    Zero,
    /// All full.
    Full,
    /// Play the failsafe scene.
    Scene,
    /// Record the current output as scene.
    Record,
}

fn print_node(o: &Out, n: Option<NodeState>) {
    match n {
        Some(n) => o.emit(json!({"type": "node", "node": to_value(&n)}), || {
            node_text(&n)
        }),
        None => o.emit(json!({"type": "node", "node": null}), || {
            "sent; no ArtPollReply received".into()
        }),
    }
}

/// `node config …`
pub async fn config(g: &Global, o: &Out, action: ConfigAction) -> Result<()> {
    let ctrl = net::controller(g).await?;
    let node = net::select_node(&ctrl, g).await?;
    let r = run(&ctrl, g, o, &node, action).await;
    ctrl.shutdown();
    r
}

async fn run(
    ctrl: &Controller,
    g: &Global,
    o: &Out,
    node: &NodeState,
    action: ConfigAction,
) -> Result<()> {
    let id = node.id;
    match action {
        ConfigAction::Show => show(ctrl, g, o, node).await,
        ConfigAction::Name { short, long } => {
            if short.is_none() && long.is_none() {
                return Err(Error::InvalidArgument("give --short and/or --long".into()));
            }
            let p = ArtAddress {
                short_name: short.unwrap_or_default(),
                long_name: long.unwrap_or_default(),
                ..ArtAddress::default()
            };
            print_node(o, ctrl.art_address(&id, p).await?);
            Ok(())
        }
        ConfigAction::Port {
            bind,
            universe,
            direction,
            merge,
            rdm,
            style,
        } => {
            check_bind(node, bind)?;
            let mut last = None;
            // direction first: the universe is programmed into SwIn or SwOut depending on it
            if let Some(d) = direction {
                let c = match d {
                    Direction::In => ac::AC_DIRECTION_RX0,
                    Direction::Out => ac::AC_DIRECTION_TX0,
                };
                last = Some(ctrl.art_address_command(&id, bind, c).await?);
            }
            if let Some(u) = universe {
                let pa = net::parse_pa(&u)?;
                let uni = ArtAddress::program(pa.universe());
                let p = ArtAddress {
                    bind_index: bind,
                    net_switch: ArtAddress::program(pa.net()),
                    sub_switch: ArtAddress::program(pa.sub_net()),
                    sw_in: [
                        uni,
                        ArtAddress::NO_CHANGE,
                        ArtAddress::NO_CHANGE,
                        ArtAddress::NO_CHANGE,
                    ],
                    sw_out: [
                        uni,
                        ArtAddress::NO_CHANGE,
                        ArtAddress::NO_CHANGE,
                        ArtAddress::NO_CHANGE,
                    ],
                    ..ArtAddress::default()
                };
                last = Some(ctrl.art_address(&id, p).await?);
            }
            let mut cmds = Vec::new();
            if let Some(m) = merge {
                cmds.push(match m {
                    Merge::Htp => ac::AC_MERGE_HTP0,
                    Merge::Ltp => ac::AC_MERGE_LTP0,
                });
            }
            if let Some(r) = rdm {
                cmds.push(match r {
                    OnOff::On => ac::AC_RDM_ENABLE0,
                    OnOff::Off => ac::AC_RDM_DISABLE0,
                });
            }
            if let Some(s) = style {
                cmds.push(match s {
                    Style::Delta => ac::AC_STYLE_DELTA0,
                    Style::Const => ac::AC_STYLE_CONST0,
                });
            }
            for c in cmds {
                last = Some(ctrl.art_address_command(&id, bind, c).await?);
            }
            match last {
                Some(n) => print_node(o, n),
                None => {
                    return Err(Error::InvalidArgument(
                        "nothing to change (--universe, --direction, --merge, --rdm, --style)"
                            .into(),
                    ))
                }
            }
            Ok(())
        }
        ConfigAction::Led { mode } => {
            let c = match mode {
                Led::Normal => ac::AC_LED_NORMAL,
                Led::Mute => ac::AC_LED_MUTE,
                Led::Locate => ac::AC_LED_LOCATE,
            };
            print_node(o, ctrl.art_address_command(&id, 1, c).await?);
            Ok(())
        }
        ConfigAction::Failsafe { mode } => {
            let c = match mode {
                Failsafe::Hold => ac::AC_FAIL_HOLD,
                Failsafe::Zero => ac::AC_FAIL_ZERO,
                Failsafe::Full => ac::AC_FAIL_FULL,
                Failsafe::Scene => ac::AC_FAIL_SCENE,
                Failsafe::Record => ac::AC_FAIL_RECORD,
            };
            print_node(o, ctrl.art_address_command(&id, 1, c).await?);
            Ok(())
        }
        ConfigAction::Ip {
            ip,
            mask,
            gateway,
            dhcp,
            reset,
        } => {
            let change = IpProgChange {
                dhcp,
                ip,
                mask,
                gateway,
                reset,
            };
            let r = ctrl.ip_prog_change(&id, &change).await?;
            o.emit(
                json!({
                    "type": "ipProgReply",
                    "ip": r.prog_ip,
                    "mask": r.prog_sm,
                    "gateway": r.prog_dg,
                    "dhcp": r.dhcp_enabled(),
                    "status": r.status,
                }),
                || {
                    format!(
                        "ip {}  mask {}  gateway {}  dhcp {}",
                        r.prog_ip,
                        r.prog_sm,
                        r.prog_dg,
                        r.dhcp_enabled()
                    )
                },
            );
            Ok(())
        }
        ConfigAction::Input { bind, disable } => {
            check_bind(node, bind)?;
            let n = node.bind_ports(bind).count().max(1);
            let mut flags = vec![false; n.min(4)];
            for i in disable {
                let k = usize::from(i);
                if k == 0 || k > flags.len() {
                    return Err(Error::InvalidArgument(format!(
                        "input {i} out of range 1..{}",
                        flags.len()
                    )));
                }
                flags[k - 1] = true;
            }
            print_node(o, ctrl.art_input(&id, bind, &flags).await?);
            Ok(())
        }
        ConfigAction::Set { pid, value } => {
            let p = crate::rdm::parse_pid(&pid)?;
            let data = encode_value(p, &value)?;
            let uid = node_uid(node)?;
            let pa = net::port_for_uid(ctrl, g, node, uid).await?;
            let r = ctrl.rdm_set(&id, pa, uid, p, &data).await?;
            print_response(o, &uid.to_string(), p, true, &r);
            Ok(())
        }
    }
}

fn check_bind(node: &NodeState, bind: u8) -> Result<()> {
    if node.bind_ports(bind).next().is_none() {
        return Err(Error::InvalidArgument(format!(
            "node {} has no BindIndex {bind}",
            node.id
        )));
    }
    Ok(())
}

fn node_uid(node: &NodeState) -> Result<Uid> {
    node.uid
        .ok_or_else(|| Error::InvalidArgument("node reports no RDM UID".into()))
}

/// Encode a value for a SET of a manufacturer PID according to its PARAMETER_DESCRIPTION.
fn encode_value(p: u16, v: &str) -> Result<Vec<u8>> {
    let bad = || Error::InvalidArgument(format!("bad value {v:?} for PID 0x{p:04X}"));
    if let Some(h) = v.strip_prefix("hex:") {
        return pixelmapper_net::parse_hex(h).ok_or_else(bad);
    }
    let def = pid_def(p)
        .filter(|d| d.device == Device::Node)
        .ok_or_else(|| {
            Error::InvalidArgument(format!(
                "0x{p:04X} is not a node PID of pids.toml (use hex:… or `rdm set`)"
            ))
        })?;
    if def.command_class & 0x02 == 0 {
        return Err(Error::InvalidArgument(format!(
            "{} is not settable",
            def.name
        )));
    }
    let size = def.set_request_pdl.unwrap_or(0);
    let desc = def.desc;
    if desc.is_some_and(|d| d.data_type == 0x02) {
        return Ok(v.as_bytes().to_vec());
    }
    if size == 0 {
        return Ok(Vec::new());
    }
    let n: u64 = match v.to_ascii_lowercase().as_str() {
        "on" | "true" | "yes" => 1,
        "off" | "false" | "no" => 0,
        s => match s.strip_prefix("0x") {
            Some(h) => u64::from_str_radix(h, 16).map_err(|_| bad())?,
            None => s.parse().map_err(|_| bad())?,
        },
    };
    if let Some(d) = desc {
        if d.max > d.min && (n < u64::from(d.min) || n > u64::from(d.max)) {
            return Err(Error::InvalidArgument(format!(
                "{} must be {}..{}",
                def.name, d.min, d.max
            )));
        }
    }
    let bytes = n.to_be_bytes();
    let size = usize::from(size).min(8);
    if bytes[..8 - size].iter().any(|b| *b != 0) {
        return Err(bad());
    }
    Ok(bytes[8 - size..].to_vec())
}

async fn show(ctrl: &Controller, g: &Global, o: &Out, node: &NodeState) -> Result<()> {
    let mut settings = serde_json::Map::new();
    let mut lines = Vec::new();
    if let Some(uid) = node.uid {
        let pa = net::port_for_uid(ctrl, g, node, uid).await?;
        for d in pids::PIDS
            .iter()
            .filter(|d| d.device == Device::Node && d.listed && d.command_class & 0x01 != 0)
            .filter(|d| d.get_request_pdl == Some(0))
        {
            let v: Value = match ctrl.rdm_get(&node.id, pa, uid, d.pid, &[]).await {
                Ok(data) => {
                    decode(d.pid, &data).unwrap_or_else(|| json!(pixelmapper_net::to_hex(&data)))
                }
                Err(e) => json!({"error": e.code(), "message": e.to_string()}),
            };
            lines.push(format!("  {:<28} {}", d.name, v));
            settings.insert(d.name.to_string(), v);
        }
    }
    o.emit(
        json!({"type": "nodeConfig", "node": to_value(node), "settings": settings}),
        || {
            let mut s = node_text(node);
            if !lines.is_empty() {
                s.push_str("\n  settings (RDM):\n");
                s.push_str(&lines.join("\n"));
            }
            s
        },
    );
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn encode_values() {
        assert_eq!(encode_value(pids::PID_PANEL_LOCK, "on").unwrap(), vec![1]);
        assert_eq!(
            encode_value(pids::PID_PORT1_DMX_BREAK, "200").unwrap(),
            vec![0, 200]
        );
        assert!(encode_value(pids::PID_PORT1_DMX_BREAK, "100").is_err());
        assert!(encode_value(pids::PID_NODE_HW_REVISION, "1").is_err());
        assert_eq!(encode_value(0x1234, "hex:0102").unwrap(), vec![1, 2]);
        assert!(encode_value(0x1234, "5").is_err());
    }
}
