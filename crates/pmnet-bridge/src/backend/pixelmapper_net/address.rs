//! `artAddress` → the sequence of ArtAddress packets (contract §6 Semantics, node contract §5).
//!
//! Rules: one Command per packet; names and Port-Addresses travel with the first packet of their BindIndex;
//! fields left out are "no change" (0x7F switches, empty names). Names and the indicator are node-wide and go to
//! the lowest BindIndex. A port's failsafe mode and scene recording go to its BindIndex (the AcFail* commands
//! have no port number: Art-Net 4 nodes keep them per BindIndex, the Pixelmapper Node has one port per
//! BindIndex). Cancel merge and reset-RX-flags are node-wide intents and go to every BindIndex of the node. The
//! packets are library values (`ArtAddress`); `pixelmapper-net` encodes them.

use crate::protocol::{
    ArtAddressArgs, ArtAddressPort, Failsafe, MergeMode, NodeState, OutputProtocol, OutputStyle,
    SetDirection, SetIndicator,
};
use pixelmapper_net::artnet::{address_command as ac, ArtAddress, PortAddress};
use std::collections::{BTreeMap, BTreeSet};

/// Base packet and commands of one BindIndex.
struct BindPlan {
    base: ArtAddress,
    commands: Vec<u8>,
}

/// The ArtAddress packets for `args`, in sending order. Empty when nothing is to be changed.
pub(super) fn plan(args: &ArtAddressArgs, node: &NodeState) -> Vec<ArtAddress> {
    let mut binds: BTreeSet<u8> = node.ports.iter().map(|p| p.port.bind).collect();
    if binds.is_empty() {
        binds.insert(1);
    }
    let root = binds.first().copied().unwrap_or(1);
    let mut plans: BTreeMap<u8, BindPlan> = BTreeMap::new();

    // Ports: addresses into the base packet, port commands (the "port i" variants) in a fixed order.
    for change in &args.ports {
        let p = at(&mut plans, change.port.bind);
        set_addresses(&mut p.base, change);
        p.commands.extend(port_commands(change));
    }
    // Node-wide fields.
    if args.short_name.is_some() || args.long_name.is_some() {
        let p = at(&mut plans, root);
        p.base.short_name = args.short_name.clone().unwrap_or_default();
        p.base.long_name = args.long_name.clone().unwrap_or_default();
    }
    let mut per_bind = Vec::new();
    if args.cancel_merge {
        per_bind.push(ac::AC_CANCEL_MERGE);
    }
    if args.reset_rx_flags {
        per_bind.push(ac::AC_RESET_RX_FLAGS);
    }
    if !per_bind.is_empty() {
        for &bind in &binds {
            at(&mut plans, bind).commands.extend(&per_bind);
        }
    }
    if let Some(i) = args.indicator {
        at(&mut plans, root).commands.push(match i {
            SetIndicator::Normal => ac::AC_LED_NORMAL,
            SetIndicator::Mute => ac::AC_LED_MUTE,
            SetIndicator::Locate => ac::AC_LED_LOCATE,
        });
    }

    let mut packets = Vec::new();
    for (_, p) in plans {
        let has_fields = p.base != ArtAddress::command(p.base.bind_index, ac::AC_NONE);
        let mut commands = p.commands.into_iter();
        match commands.next() {
            Some(first) => packets.push(ArtAddress {
                command: first,
                ..p.base.clone()
            }),
            None if has_fields => packets.push(p.base.clone()),
            None => {}
        }
        packets.extend(commands.map(|c| ArtAddress::command(p.base.bind_index, c)));
    }
    packets
}

/// The plan of `bind`, created on first use.
fn at(plans: &mut BTreeMap<u8, BindPlan>, bind: u8) -> &mut BindPlan {
    plans.entry(bind).or_insert_with(|| BindPlan {
        base: ArtAddress {
            bind_index: bind,
            ..ArtAddress::default()
        },
        commands: Vec::new(),
    })
}

/// Writes NetSwitch/SubSwitch and SwOut/SwIn of the port (programming bit 7 set).
fn set_addresses(base: &mut ArtAddress, change: &ArtAddressPort) {
    let i = usize::from(change.port.index.min(3));
    let mut write = |raw: u16, out: bool| {
        // The server rejected addresses above 32767 already.
        let Some(pa) = PortAddress::new(raw) else {
            return;
        };
        base.net_switch = ArtAddress::program(pa.net());
        base.sub_switch = ArtAddress::program(pa.sub_net());
        let sw = ArtAddress::program(pa.universe());
        if out {
            base.sw_out[i] = sw;
        } else {
            base.sw_in[i] = sw;
        }
    };
    if let Some(a) = change.output_port_address {
        write(a, true);
    }
    if let Some(a) = change.input_port_address {
        write(a, false);
    }
}

/// Port commands for port `index` of its BindIndex: direction first, then merge, RDM, style, protocol, clear,
/// then the BindIndex's failsafe mode and scene recording.
fn port_commands(change: &ArtAddressPort) -> Vec<u8> {
    let i = change.port.index.min(3);
    let mut out = Vec::new();
    if let Some(d) = change.direction {
        out.push(
            match d {
                SetDirection::Output => ac::AC_DIRECTION_TX0,
                SetDirection::Input => ac::AC_DIRECTION_RX0,
            } + i,
        );
    }
    if let Some(m) = change.merge {
        out.push(
            match m {
                MergeMode::Htp => ac::AC_MERGE_HTP0,
                MergeMode::Ltp => ac::AC_MERGE_LTP0,
            } + i,
        );
    }
    if let Some(r) = change.rdm {
        out.push(
            if r {
                ac::AC_RDM_ENABLE0
            } else {
                ac::AC_RDM_DISABLE0
            } + i,
        );
    }
    if let Some(s) = change.output_style {
        out.push(
            match s {
                OutputStyle::Delta => ac::AC_STYLE_DELTA0,
                OutputStyle::Continuous => ac::AC_STYLE_CONST0,
            } + i,
        );
    }
    if let Some(p) = change.protocol {
        out.push(
            match p {
                OutputProtocol::Artnet => ac::AC_ARTNET_SEL0,
                OutputProtocol::Sacn => ac::AC_ACN_SEL0,
            } + i,
        );
    }
    if change.clear_output {
        out.push(ac::AC_CLEAR_OP0 + i);
    }
    if let Some(f) = change.failsafe {
        out.push(match f {
            Failsafe::Hold => ac::AC_FAIL_HOLD,
            Failsafe::Zero => ac::AC_FAIL_ZERO,
            Failsafe::Full => ac::AC_FAIL_FULL,
            Failsafe::Scene => ac::AC_FAIL_SCENE,
        });
    }
    if change.record_failsafe_scene {
        out.push(ac::AC_FAIL_RECORD);
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::backend::mock::sample_node;
    use crate::protocol::PortRef;
    use std::net::Ipv4Addr;

    fn args() -> ArtAddressArgs {
        ArtAddressArgs {
            node_id: "7FF0:00000001".into(),
            short_name: None,
            long_name: None,
            ports: Vec::new(),
            indicator: None,
            cancel_merge: false,
            reset_rx_flags: false,
        }
    }

    fn port(bind: u8) -> ArtAddressPort {
        ArtAddressPort {
            port: PortRef::new(bind, 0),
            output_port_address: None,
            input_port_address: None,
            direction: None,
            merge: None,
            rdm: None,
            output_style: None,
            protocol: None,
            clear_output: false,
            failsafe: None,
            record_failsafe_scene: false,
        }
    }

    fn node() -> NodeState {
        sample_node("7FF0:00000001", Ipv4Addr::LOCALHOST, 4)
    }

    #[test]
    fn nothing_to_send() {
        assert!(plan(&args(), &node()).is_empty());
    }

    #[test]
    fn names_go_to_the_root_bind() {
        let a = ArtAddressArgs {
            short_name: Some("Stage left".into()),
            ..args()
        };
        let p = plan(&a, &node());
        assert_eq!(p.len(), 1);
        assert_eq!(p[0].bind_index, 1);
        assert_eq!(p[0].short_name, "Stage left");
        assert_eq!(p[0].long_name, "", "empty = no change");
        assert_eq!(p[0].command, ac::AC_NONE);
        assert_eq!(p[0].net_switch, ArtAddress::NO_CHANGE);
    }

    #[test]
    fn port_address_and_commands() {
        let a = ArtAddressArgs {
            ports: vec![ArtAddressPort {
                output_port_address: Some(0x0123),
                direction: Some(SetDirection::Output),
                merge: Some(MergeMode::Ltp),
                rdm: Some(false),
                ..port(3)
            }],
            ..args()
        };
        let p = plan(&a, &node());
        assert_eq!(p.len(), 3);
        assert_eq!(p[0].bind_index, 3);
        assert_eq!(p[0].net_switch, 0x81);
        assert_eq!(p[0].sub_switch, 0x82);
        assert_eq!(p[0].sw_out[0], 0x83);
        assert_eq!(p[0].sw_in[0], ArtAddress::NO_CHANGE);
        assert_eq!(p[0].port_address_out(0).raw(), 0x0123);
        assert_eq!(p[0].command, ac::AC_DIRECTION_TX0);
        assert_eq!(p[1], ArtAddress::command(3, ac::AC_MERGE_LTP0));
        assert_eq!(p[2], ArtAddress::command(3, ac::AC_RDM_DISABLE0));
    }

    #[test]
    fn node_wide_and_per_port_commands() {
        let a = ArtAddressArgs {
            cancel_merge: true,
            indicator: Some(SetIndicator::Locate),
            ports: vec![ArtAddressPort {
                failsafe: Some(Failsafe::Zero),
                record_failsafe_scene: true,
                ..port(3)
            }],
            ..args()
        };
        let p = plan(&a, &node());
        let cmds: Vec<(u8, u8)> = p.iter().map(|x| (x.bind_index, x.command)).collect();
        assert_eq!(
            cmds,
            vec![
                (1, ac::AC_CANCEL_MERGE),
                (1, ac::AC_LED_LOCATE),
                (2, ac::AC_CANCEL_MERGE),
                (3, ac::AC_FAIL_ZERO),
                (3, ac::AC_FAIL_RECORD),
                (3, ac::AC_CANCEL_MERGE),
                (4, ac::AC_CANCEL_MERGE)
            ]
        );
    }

    #[test]
    fn second_port_of_a_bind_uses_its_index() {
        let mut n = node();
        let mut extra = n.ports[0].clone();
        extra.port = PortRef::new(1, 2);
        n.ports.insert(1, extra);
        let a = ArtAddressArgs {
            ports: vec![ArtAddressPort {
                input_port_address: Some(5),
                output_style: Some(OutputStyle::Delta),
                ..ArtAddressPort {
                    port: PortRef::new(1, 2),
                    ..port(1)
                }
            }],
            ..args()
        };
        let p = plan(&a, &n);
        assert_eq!(p.len(), 1);
        assert_eq!(p[0].sw_in[2], 0x85);
        assert_eq!(p[0].command, ac::AC_STYLE_DELTA0 + 2);
    }
}
