//! Socket setup and network interfaces.

use crate::error::Result;
use serde::Serialize;
use socket2::{Domain, Protocol, Socket, Type};
use std::net::{Ipv4Addr, SocketAddr, SocketAddrV4};

/// One IPv4 network interface.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Interface {
    /// OS name, e.g. `en0`.
    pub name: String,
    /// Address.
    pub ip: Ipv4Addr,
    /// Netmask.
    pub netmask: Ipv4Addr,
    /// Directed broadcast address of the subnet.
    pub broadcast: Ipv4Addr,
    /// Loopback interface.
    pub loopback: bool,
}

/// All IPv4 interfaces of this host.
pub fn interfaces() -> Vec<Interface> {
    let Ok(list) = if_addrs::get_if_addrs() else {
        return Vec::new();
    };
    list.into_iter()
        .filter_map(|i| match &i.addr {
            if_addrs::IfAddr::V4(v4) => {
                let bc = v4
                    .broadcast
                    .unwrap_or_else(|| Ipv4Addr::from(u32::from(v4.ip) | !u32::from(v4.netmask)));
                Some(Interface {
                    name: i.name.clone(),
                    ip: v4.ip,
                    netmask: v4.netmask,
                    broadcast: bc,
                    loopback: v4.ip.is_loopback(),
                })
            }
            _ => None,
        })
        .collect()
}

/// Default ArtPoll/ArtSync destinations (contract §2): the directed broadcast of every non-loopback interface
/// (restricted to the bound interface if `bind_ip` is not unspecified) plus 2.255.255.255. Never
/// 255.255.255.255.
pub fn default_broadcast_targets(bind_ip: Ipv4Addr, port: u16) -> Vec<SocketAddr> {
    let mut out: Vec<SocketAddr> = interfaces()
        .into_iter()
        .filter(|i| !i.loopback && (bind_ip.is_unspecified() || i.ip == bind_ip))
        .map(|i| SocketAddr::V4(SocketAddrV4::new(i.broadcast, port)))
        .collect();
    let two = SocketAddr::V4(SocketAddrV4::new(Ipv4Addr::new(2, 255, 255, 255), port));
    if !out.contains(&two) {
        out.push(two);
    }
    out
}

/// Create the UDP socket: SO_REUSEADDR (+ SO_REUSEPORT if `reuse_port`), broadcast enabled, non-blocking.
pub fn bind_socket(addr: SocketAddr, reuse_port: bool) -> Result<tokio::net::UdpSocket> {
    let s = Socket::new(Domain::IPV4, Type::DGRAM, Some(Protocol::UDP))?;
    s.set_reuse_address(true)?;
    #[cfg(unix)]
    if reuse_port {
        s.set_reuse_port(true)?;
    }
    #[cfg(not(unix))]
    let _ = reuse_port;
    s.set_broadcast(true)?;
    s.set_nonblocking(true)?;
    s.bind(&addr.into())?;
    Ok(tokio::net::UdpSocket::from_std(s.into())?)
}
