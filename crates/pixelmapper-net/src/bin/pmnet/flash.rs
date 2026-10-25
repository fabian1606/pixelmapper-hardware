//! `flash tube|node|info|make-alf`.

use crate::net;
use crate::out::{pretty, to_value, Out};
use crate::{Global, NodeMethod};
use pixelmapper_net::flash::node::{NodeFlashMethod, NodeFlashOptions};
use pixelmapper_net::flash::tube::{RescueTarget, TubeFlashOptions};
use pixelmapper_net::flash::{FlashJob, FlashPhase, FlashProgress};
use pixelmapper_net::image::{NodeImage, TubeImage};
use pixelmapper_net::{Error, Result};
use serde_json::json;
use std::path::Path;
use std::time::Duration;

/// Options of `flash tube`.
pub struct TubeArgs {
    pub allow_downgrade: bool,
    pub rescue: bool,
    pub finish_wait_ms: Option<u64>,
}

fn read(path: &Path) -> Result<Vec<u8>> {
    std::fs::read(path).map_err(|e| {
        Error::Io(std::sync::Arc::new(std::io::Error::new(
            e.kind(),
            format!("{}: {e}", path.display()),
        )))
    })
}

fn load_tube(path: &Path) -> Result<TubeImage> {
    let bytes = read(path)?;
    let text = String::from_utf8(bytes)
        .map_err(|_| Error::Image(format!("{} is not Intel HEX text", path.display())))?;
    TubeImage::parse_hex(&text)
}

fn is_hex(path: &Path) -> bool {
    path.extension()
        .and_then(|e| e.to_str())
        .is_some_and(|e| e.eq_ignore_ascii_case("hex") || e.eq_ignore_ascii_case("ihex"))
}

fn print_tube_info(o: &Out, path: &Path, img: &TubeImage) {
    let i = img.info();
    o.emit(
        json!({"type": "imageInfo", "kind": "tube", "file": path.display().to_string(), "info": to_value(&i)}),
        || {
            format!(
                "tube image {}\n  version {} (0x{:08X})  manufacturer 0x{:04X}  model 0x{:04X}\n  length {} / {} bytes  crc 0x{:04X}",
                path.display(),
                i.version_label,
                i.version,
                i.manufacturer,
                i.model,
                i.length,
                i.max_length,
                i.crc
            )
        },
    );
}

fn print_node_info(o: &Out, path: &Path, img: &NodeImage) {
    let i = img.info();
    o.emit(
        json!({"type": "imageInfo", "kind": "node", "file": path.display().to_string(), "info": to_value(&i)}),
        || format!("node image {}\n{}", path.display(), pretty(&to_value(&i), 2)),
    );
}

/// `flash info <file>`.
pub fn info(o: &Out, path: &Path) -> Result<()> {
    if is_hex(path) {
        print_tube_info(o, path, &load_tube(path)?);
    } else {
        print_node_info(o, path, &NodeImage::from_file_bytes(read(path)?)?);
    }
    Ok(())
}

/// `flash make-alf <bin> <out>`.
pub fn make_alf(o: &Out, bin: &Path, out: &Path) -> Result<()> {
    let img = NodeImage::from_file_bytes(read(bin)?)?;
    let alf = img.to_alf();
    std::fs::write(out, &alf)?;
    o.emit(
        json!({"type": "alf", "file": out.display().to_string(), "bytes": alf.len(), "info": to_value(&img.info())}),
        || format!("wrote {} ({} bytes)", out.display(), alf.len()),
    );
    Ok(())
}

/// Print progress lines until the job ends; returns the job result.
async fn follow(
    o: &Out,
    job: FlashJob,
    rescue: bool,
) -> Result<pixelmapper_net::flash::FlashReport> {
    let mut rx = job.progress();
    let mut last: Option<(FlashPhase, u32, String)> = None;
    let mut prompted = false;
    let mut print = |p: &FlashProgress| {
        let key = (p.phase, p.done, p.message.clone());
        if last.as_ref() == Some(&key) {
            return;
        }
        if rescue && p.phase == FlashPhase::Catching && !prompted {
            prompted = true;
            o.emit(
                json!({"type": "prompt", "message": "power-cycle the tube now"}),
                String::new,
            );
            if !o.json {
                eprintln!("\n>>> POWER-CYCLE THE TUBE NOW (unplug and reconnect its power) <<<\n");
            }
        }
        // in text mode only print phase changes and every 10 % of a long phase
        let show = o.json
            || last.as_ref().is_none_or(|(ph, d, _)| {
                *ph != p.phase
                    || p.total == 0
                    || (p.done * 10 / p.total.max(1)) != (d * 10 / p.total.max(1))
                    || p.done == p.total
            });
        last = Some(key);
        if show {
            o.emit(json!({"type": "progress", "progress": to_value(p)}), || {
                if p.total > 0 {
                    format!("{:?} {}/{} {}", p.phase, p.done, p.total, p.message)
                } else {
                    format!("{:?} {}", p.phase, p.message)
                }
            });
        }
    };
    loop {
        let p = rx.borrow_and_update().clone();
        print(&p);
        if matches!(
            p.phase,
            FlashPhase::Done | FlashPhase::Failed | FlashPhase::Cancelled
        ) {
            break;
        }
        if rx.changed().await.is_err() {
            break;
        }
    }
    job.wait().await
}

/// `flash tube <uid> <hex>`.
pub async fn tube(g: &Global, o: &Out, uid: &str, hex: &Path, a: TubeArgs) -> Result<()> {
    let uid = net::parse_uid(uid)?;
    let img = load_tube(hex)?;
    print_tube_info(o, hex, &img);
    let ctrl = net::controller(g).await?;
    let node = net::select_node(&ctrl, g).await?;
    let pa = net::port_for_uid(&ctrl, g, &node, uid).await?;
    let mut opts = TubeFlashOptions {
        allow_downgrade: a.allow_downgrade,
        ..TubeFlashOptions::default()
    };
    if let Some(ms) = a.finish_wait_ms {
        opts.finish_wait = Duration::from_millis(ms);
    }
    if a.rescue {
        let port = node.port(pa).ok_or_else(|| Error::UnknownPort {
            node: node.id.to_string(),
            port_address: pa.to_string(),
        })?;
        let node_uid = node
            .uid
            .ok_or_else(|| Error::InvalidArgument("node reports no RDM UID".into()))?;
        opts.rescue = Some(RescueTarget {
            node_uid,
            port: port.port_number,
        });
    }
    o.note(&format!("flashing {uid} via {} port {pa}", node.id));
    let job = ctrl.flash_tube_with(&node.id, pa, uid, img, opts)?;
    let r = follow(o, job, a.rescue).await;
    ctrl.shutdown();
    let rep = r?;
    o.emit(
        json!({"type": "flashReport", "report": to_value(&rep)}),
        || {
            format!(
                "done: {uid} now runs {} ({} unit(s) written)",
                rep.version_label, rep.units_written
            )
        },
    );
    Ok(())
}

/// `flash node <image>`.
pub async fn node(
    g: &Global,
    o: &Out,
    path: &Path,
    method: NodeMethod,
    allow_downgrade: bool,
) -> Result<()> {
    let img = NodeImage::from_file_bytes(read(path)?)?;
    print_node_info(o, path, &img);
    let ctrl = net::controller(g).await?;
    let node = net::select_node(&ctrl, g).await?;
    let opts = NodeFlashOptions {
        method: match method {
            NodeMethod::Artnet => NodeFlashMethod::ArtFirmwareMaster,
            NodeMethod::Rdm => NodeFlashMethod::Rdm,
        },
        allow_downgrade,
        ..NodeFlashOptions::default()
    };
    let job = ctrl.flash_node(&node.id, img, opts)?;
    let r = follow(o, job, false).await;
    ctrl.shutdown();
    let rep = r?;
    o.emit(
        json!({"type": "flashReport", "report": to_value(&rep)}),
        || format!("done: node {} now runs {}", node.id, rep.version_label),
    );
    Ok(())
}
