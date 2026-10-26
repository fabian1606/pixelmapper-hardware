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

impl Interface {
    /// Hardware (MAC) address of this interface, `None` if it has none (loopback, tunnels) or the platform is
    /// not supported (Linux, macOS/BSD are). Looked up on each call, see [`interface_macs`].
    pub fn mac(&self) -> Option<[u8; 6]> {
        interface_macs()
            .into_iter()
            .find(|(n, _)| *n == self.name)
            .map(|(_, m)| m)
    }
}

/// MAC addresses of this host's interfaces as `(name, mac)`, sorted by name; interfaces without a 6-byte
/// non-zero hardware address are left out.
pub fn interface_macs() -> Vec<(String, [u8; 6])> {
    let mut out = os_macs();
    out.retain(|(_, m)| *m != [0; 6]);
    out.sort();
    out.dedup();
    out
}

#[cfg(any(
    target_os = "linux",
    target_os = "android",
    target_os = "macos",
    target_os = "ios",
    target_os = "freebsd",
    target_os = "openbsd",
    target_os = "netbsd"
))]
fn os_macs() -> Vec<(String, [u8; 6])> {
    let mut out = Vec::new();
    let mut head: *mut libc::ifaddrs = std::ptr::null_mut();
    // SAFETY: getifaddrs fills `head` with a list that we free with freeifaddrs below; every pointer is checked
    // for null before it is read, and the sockaddr is only reinterpreted for its own address family.
    unsafe {
        if libc::getifaddrs(&mut head) != 0 {
            return out;
        }
        let mut cur = head;
        while !cur.is_null() {
            let ifa = &*cur;
            cur = ifa.ifa_next;
            if ifa.ifa_addr.is_null() || ifa.ifa_name.is_null() {
                continue;
            }
            let name = std::ffi::CStr::from_ptr(ifa.ifa_name)
                .to_string_lossy()
                .into_owned();
            if let Some(mac) = link_mac(ifa.ifa_addr) {
                out.push((name, mac));
            }
        }
        libc::freeifaddrs(head);
    }
    out
}

#[cfg(any(target_os = "linux", target_os = "android"))]
unsafe fn link_mac(sa: *const libc::sockaddr) -> Option<[u8; 6]> {
    if i32::from((*sa).sa_family) != libc::AF_PACKET {
        return None;
    }
    let ll = &*(sa as *const libc::sockaddr_ll);
    if ll.sll_halen != 6 {
        return None;
    }
    let mut m = [0u8; 6];
    m.copy_from_slice(&ll.sll_addr[..6]);
    Some(m)
}

#[cfg(not(any(target_os = "linux", target_os = "android")))]
#[cfg(any(
    target_os = "macos",
    target_os = "ios",
    target_os = "freebsd",
    target_os = "openbsd",
    target_os = "netbsd"
))]
unsafe fn link_mac(sa: *const libc::sockaddr) -> Option<[u8; 6]> {
    if i32::from((*sa).sa_family) != libc::AF_LINK {
        return None;
    }
    let dl = &*(sa as *const libc::sockaddr_dl);
    if dl.sdl_alen != 6 {
        return None;
    }
    // sdl_data holds the name (sdl_nlen bytes) followed by the address; it may extend past the declared array.
    let base = std::ptr::addr_of!(dl.sdl_data) as *const u8;
    let mut m = [0u8; 6];
    std::ptr::copy_nonoverlapping(base.add(usize::from(dl.sdl_nlen)), m.as_mut_ptr(), 6);
    Some(m)
}

#[cfg(not(any(
    target_os = "linux",
    target_os = "android",
    target_os = "macos",
    target_os = "ios",
    target_os = "freebsd",
    target_os = "openbsd",
    target_os = "netbsd"
)))]
fn os_macs() -> Vec<(String, [u8; 6])> {
    Vec::new()
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn macs_of_interfaces() {
        let macs = interface_macs();
        assert!(macs.iter().all(|(_, m)| *m != [0; 6]));
        // every non-loopback IPv4 interface of a normal host (CI, laptop) has a MAC on Linux/macOS
        #[cfg(any(target_os = "linux", target_os = "macos"))]
        {
            let eth: Vec<Interface> = interfaces()
                .into_iter()
                .filter(|i| {
                    !i.loopback && !i.name.starts_with("utun") && !i.name.starts_with("tun")
                })
                .collect();
            if let Some(i) = eth.first() {
                assert!(i.mac().is_some(), "no MAC for {} in {macs:?}", i.name);
            }
            let lo = interfaces().into_iter().find(|i| i.loopback);
            if let Some(lo) = lo {
                assert_eq!(lo.mac(), None);
            }
        }
    }
}
