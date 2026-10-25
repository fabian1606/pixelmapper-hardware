//! `dmx <pa> <values…>`.

use crate::net;
use crate::out::Out;
use crate::Global;
use clap::Args;
use pixelmapper_net::{Error, Result};
use serde_json::json;
use std::time::Duration;

/// Arguments of `dmx`.
#[derive(Args, Debug)]
pub struct DmxArgs {
    /// Port-Address (N:S:U or number).
    pub pa: String,
    /// Values: `slot=value` (1-based, `a-b=value` for a range) or plain values in order; with --hex one hex string.
    pub values: Vec<String>,
    /// Values are hex bytes.
    #[arg(long)]
    pub hex: bool,
    /// Number of frames.
    #[arg(long, default_value_t = 1)]
    pub repeat: u32,
    /// Frames per second for --repeat.
    #[arg(long, default_value_t = 30.0)]
    pub fps: f64,
    /// Send ArtSync (broadcast) after each frame.
    #[arg(long)]
    pub sync: bool,
}

fn num(s: &str) -> Option<u32> {
    match s.strip_prefix("0x") {
        Some(h) => u32::from_str_radix(h, 16).ok(),
        None => s.parse().ok(),
    }
}

/// Build the slot data from the value arguments.
pub fn parse_values(values: &[String], hex: bool) -> Result<Vec<u8>> {
    if hex {
        let joined: String = values.concat();
        let t = joined.strip_prefix("0x").unwrap_or(&joined);
        return pixelmapper_net::parse_hex(t)
            .ok_or_else(|| Error::InvalidArgument(format!("bad hex data {joined:?}")));
    }
    let mut data: Vec<u8> = Vec::new();
    let bad = |s: &str| Error::InvalidArgument(format!("bad DMX value {s:?}"));
    for v in values {
        let (from, to, value) = match v.split_once('=') {
            Some((slots, val)) => {
                let (a, b) = match slots.split_once('-') {
                    Some((a, b)) => (num(a), num(b)),
                    None => (num(slots), num(slots)),
                };
                (a.ok_or_else(|| bad(v))?, b.ok_or_else(|| bad(v))?, val)
            }
            None => {
                let next = data.len() as u32 + 1;
                (next, next, v.as_str())
            }
        };
        let value = num(value)
            .and_then(|x| u8::try_from(x).ok())
            .ok_or_else(|| bad(v))?;
        if from == 0 || to < from || to > 512 {
            return Err(Error::InvalidArgument(format!(
                "slot range {v:?} outside 1..512"
            )));
        }
        if data.len() < to as usize {
            data.resize(to as usize, 0);
        }
        for s in from..=to {
            data[s as usize - 1] = value;
        }
    }
    Ok(data)
}

/// `dmx`.
pub async fn dmx(g: &Global, o: &Out, a: DmxArgs) -> Result<()> {
    let pa = net::parse_pa(&a.pa)?;
    let data = parse_values(&a.values, a.hex)?;
    if data.len() > 512 {
        return Err(Error::InvalidArgument(format!(
            "{} slots > 512",
            data.len()
        )));
    }
    if !(a.fps > 0.0 && a.fps <= 1000.0) {
        return Err(Error::InvalidArgument("--fps must be 0..1000".into()));
    }
    let ctrl = net::controller(g).await?;
    let node = if g.node.is_some() {
        Some(net::select_node(&ctrl, g).await?)
    } else {
        net::discover_all(&ctrl, g).await?;
        None
    };
    let period = Duration::from_secs_f64(1.0 / a.fps);
    let mut targets = 0;
    let mut interval = tokio::time::interval(period);
    let r: Result<()> = async {
        for _ in 0..a.repeat.max(1) {
            interval.tick().await;
            targets = match &node {
                Some(n) => {
                    ctrl.send_dmx_to(&n.id, pa, &data).await?;
                    1
                }
                None => ctrl.send_dmx(pa, &data).await?,
            };
            if targets == 0 {
                return Err(Error::InvalidArgument(format!(
                    "no node outputs Port-Address {pa} (use --node to send anyway)"
                )));
            }
            if a.sync {
                ctrl.send_sync().await?;
            }
        }
        Ok(())
    }
    .await;
    // give the socket a moment to flush before the runtime ends
    tokio::time::sleep(Duration::from_millis(20)).await;
    ctrl.shutdown();
    r?;
    o.emit(
        json!({
            "type": "dmx",
            "portAddress": pa,
            "slots": data.len(),
            "frames": a.repeat.max(1),
            "nodes": targets,
            "sync": a.sync,
        }),
        || {
            format!(
                "sent {} frame(s) of {} slot(s) to {pa} ({} node(s))",
                a.repeat.max(1),
                data.len(),
                targets
            )
        },
    );
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn values() {
        let v = |a: &[&str]| a.iter().map(|s| s.to_string()).collect::<Vec<_>>();
        assert_eq!(
            parse_values(&v(&["1=255", "3-4=7"]), false).unwrap(),
            vec![255, 0, 7, 7]
        );
        assert_eq!(
            parse_values(&v(&["10", "0x20"]), false).unwrap(),
            vec![10, 32]
        );
        assert_eq!(
            parse_values(&v(&["ff00", "01"]), true).unwrap(),
            vec![255, 0, 1]
        );
        assert!(parse_values(&v(&["0=1"]), false).is_err());
        assert!(parse_values(&v(&["513=1"]), false).is_err());
        assert!(parse_values(&v(&["1=256"]), false).is_err());
    }
}
