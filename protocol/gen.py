#!/usr/bin/env python3
"""Generate PID definitions from protocol/pids.toml (Python >= 3.11, stdlib only).

  python3 protocol/gen.py            write the C header and the Rust module
  python3 protocol/gen.py --check    validate pids.toml, verify the generated files are up to date and that
                                     tube/firmware/include/rdm_boot.h + config.h match pids.toml (exit 1 on error)

Outputs:
  node/firmware/components/pm_protocol/include/pm_pids.h
  crates/pixelmapper-net/src/pids_generated.rs
"""
from __future__ import annotations

import argparse
import pathlib
import re
import sys
import tomllib

ROOT = pathlib.Path(__file__).resolve().parent.parent
TOML = ROOT / "protocol" / "pids.toml"
C_OUT = ROOT / "node/firmware/components/pm_protocol/include/pm_pids.h"
RS_OUT = ROOT / "crates/pixelmapper-net/src/pids_generated.rs"
TUBE_PID_HEADER = ROOT / "tube/firmware/include/rdm_boot.h"
TUBE_CONFIG = ROOT / "tube/firmware/include/config.h"

# PARAMETER_DESCRIPTION codes (rdm-digest.md 6.2, OLA RDMEnums.h)
DATA_TYPES = {"not_defined": 0x00, "bit_field": 0x01, "ascii": 0x02, "unsigned_byte": 0x03, "signed_byte": 0x04,
              "unsigned_word": 0x05, "signed_word": 0x06, "unsigned_dword": 0x07, "signed_dword": 0x08}
UNITS = {"none": 0x00, "centigrade": 0x01, "volts_dc": 0x02, "volts_ac_peak": 0x03, "volts_ac_rms": 0x04,
         "ampere_dc": 0x05, "ampere_ac_peak": 0x06, "ampere_ac_rms": 0x07, "hertz": 0x08, "ohm": 0x09,
         "watt": 0x0A, "kilogram": 0x0B, "meters": 0x0C, "second": 0x15, "degree": 0x16, "byte": 0x1C,
         "decibel": 0x1D, "percent": 0x21, "rpm": 0x23, "byte_per_second": 0x24}
PREFIXES = {"none": 0x00, "deci": 0x01, "centi": 0x02, "milli": 0x03, "micro": 0x04, "nano": 0x05,
            "deca": 0x11, "hecto": 0x12, "kilo": 0x13, "mega": 0x14}
CCS = {"get": 0x01, "set": 0x02, "get_set": 0x03}
STATUS_TYPES = {"advisory": 0x02, "warning": 0x03, "error": 0x04}
FIELD_SIZES = {"u8": 1, "i8": 1, "bool": 1, "u16": 2, "i16": 2, "u32": 4, "i32": 4, "uid": 6, "ipv4": 4}
DEVICES = {"tube_app", "tube_boot", "node"}


class GenError(Exception):
    pass


# --------------------------------------------------------------------------------------------- load + validate
def field_len(f: dict, maximum: bool = True) -> int:
    t = f["type"]
    if t in FIELD_SIZES:
        return FIELD_SIZES[t]
    if t in ("ascii", "bytes"):
        if "len" in f:
            return f["len"]
        if "max_len" in f:
            return f["max_len"] if maximum else 0
    raise GenError(f"field {f.get('name')}: unknown type/length {t}")


def pdl(fields: list[dict]) -> int:
    return sum(field_len(f) for f in fields)


def expand(entries: list[dict]) -> list[dict]:
    out = []
    for e in entries:
        n = e.get("repeat", 0)
        if not n:
            e = dict(e)
            e["base_name"] = e["name"]
            out.append(e)
            continue
        if "{n}" not in e["name"]:
            raise GenError(f"{e['name']}: repeat needs {{n}} in the name")
        for i in range(1, n + 1):
            x = {k: v for k, v in e.items()}
            x["name"] = e["name"].replace("{n}", str(i))
            x["base_name"] = e["name"].replace("{n}", "")
            x["pid"] = e["pid"] + (i - 1) * e.get("stride", 1)
            x["index"] = i
            if "pd" in e:
                x["pd"] = dict(e["pd"])
                x["pd"]["description"] = e["pd"]["description"].replace("{n}", str(i))
            out.append(x)
    return out


def load() -> dict:
    d = tomllib.loads(TOML.read_text())
    if d.get("schema") != 1:
        raise GenError("pids.toml: schema must be 1")
    pids = expand(d["pid"])
    seen: dict[int, str] = {}
    names: set[str] = set()
    for p in pids:
        name, num = p["name"], p["pid"]
        if not re.fullmatch(r"[A-Z0-9_]+", name):
            raise GenError(f"{name}: name must be UPPER_SNAKE")
        if num in seen:
            raise GenError(f"PID 0x{num:04X} used by {seen[num]} and {name}")
        seen[num] = name
        if name in names:
            raise GenError(f"duplicate name {name}")
        names.add(name)
        if not 0x8000 <= num <= 0xFFDF:
            raise GenError(f"{name}: 0x{num:04X} outside the manufacturer range 0x8000-0xFFDF")
        if p["device"] not in DEVICES:
            raise GenError(f"{name}: unknown device {p['device']}")
        if p["device"] == "node" and not 0x8100 <= num <= 0x81FF:
            raise GenError(f"{name}: node PIDs live in 0x8100-0x81FF")
        if p["device"] != "node" and not 0x8000 <= num <= 0x80FF:
            raise GenError(f"{name}: tube PIDs live in 0x8000-0x80FF")
        cc = p["cc"]
        if cc not in CCS:
            raise GenError(f"{name}: cc must be get/set/get_set")
        for k in ("get_request", "get_response"):
            if ("get" in cc) != (k in p):
                raise GenError(f"{name}: {k} must exist exactly when cc includes get")
        for k in ("set_request", "set_response"):
            if ("set" in cc) != (k in p):
                raise GenError(f"{name}: {k} must exist exactly when cc includes set")
        for k in ("get_request", "get_response", "set_request", "set_response"):
            if k in p and pdl(p[k]) > 231:
                raise GenError(f"{name}: {k} PDL {pdl(p[k])} > 231")
        if p["listed"]:
            pd = p.get("pd")
            if not pd:
                raise GenError(f"{name}: listed PIDs need [pid.pd] (PARAMETER_DESCRIPTION)")
            desc = pd["description"]
            if len(desc) > 32 or not all(0x20 <= ord(c) < 0x7F for c in desc):
                raise GenError(f"{name}: description must be <= 32 printable ASCII chars: {desc!r}")
            for key, table in (("data_type", DATA_TYPES), ("unit", UNITS), ("prefix", PREFIXES)):
                if pd[key] not in table:
                    raise GenError(f"{name}: unknown {key} {pd[key]!r}")
            value_fields = p["get_response"] if "get" in cc else p["set_request"]
            if pd["pdl_size"] != pdl(value_fields):
                raise GenError(f"{name}: pd.pdl_size {pd['pdl_size']} != PDL of the value {pdl(value_fields)}")
            if pd["data_type"] != "not_defined" and pd["data_type"] != "ascii":
                if len(value_fields) != 1:
                    raise GenError(f"{name}: numeric data_type needs exactly one value field")
                if not pd["min"] <= pd["default"] <= pd["max"]:
                    raise GenError(f"{name}: default outside min..max")
    for p in pids:
        for k in ("get_request", "get_response", "set_request", "set_response"):
            for f in p.get(k, []):
                e = f.get("enum")
                if e is None or e in d["enums"]:
                    continue
                if not (e.endswith("_*") and any(c.startswith(e[:-1]) for c in d["tube_constants"])):
                    raise GenError(f"{p['name']}.{f['name']}: enum {e!r} is neither an [enums.*] table "
                                   f"nor a [tube_constants] prefix PREFIX_*")
    for s in d.get("status", []):
        if not 0x8000 <= s["id"] <= 0xFFDF or s["type"] not in STATUS_TYPES or len(s["description"]) > 32:
            raise GenError(f"status {s['name']}: bad id/type/description")
    for grp in ("required", "listed"):
        for s in d["node"]["standard_pids"][grp]:
            if s["pid"] >= 0x8000:
                raise GenError(f"standard pid {s['name']} in manufacturer range")
    d["pids"] = pids
    return d


# --------------------------------------------------------------------------------------------- C
def c_pid_name(p: dict) -> str:
    return f"PM_PID_{p['name']}"   # c_name is only the tube header name (rdm_boot.h), checked by --check


def gen_c(d: dict) -> str:
    L = ["/* @generated by protocol/gen.py from protocol/pids.toml - do not edit. */",
         "/* Manufacturer-specific RDM definitions shared by node firmware, tubes and pixelmapper-net. */",
         "#pragma once", "", "#include <stdint.h>", ""]
    L.append("/* ---- constants ---- */")
    for k, v in d["constants"].items():
        L.append(f"#define PM_{k} 0x{v['value']:X}u /* {v['doc']} */")
    L.append("")
    L.append("/* ---- tube bootloader constants (same values as tube/firmware/include/rdm_boot.h) ---- */")
    for k, v in d["tube_constants"].items():
        L.append(f"#define PM_{k} 0x{v:X}u")
    L.append("")
    L.append("/* ---- enumerations ---- */")
    for en, vals in d["enums"].items():
        up = en.upper()
        L.append("typedef enum {")
        for k, v in vals.items():
            L.append(f"    PM_{up}_{k} = {v},")
        L.append(f"}} pm_{en}_t;")
        L.append("")
    L.append("/* ---- manufacturer status message IDs (STATUS_MESSAGES) ---- */")
    for s in d.get("status", []):
        L.append(f"#define PM_STATUS_{s['name']} 0x{s['id']:04X}u /* {STATUS_TYPES[s['type']]:#04x} {s['type']}: \"{s['description']}\" */")
    L.append("#define PM_STATUS_ID_DESC_LIST(X) \\")
    for s in d.get("status", []):
        L.append(f"    X(0x{s['id']:04X}u, \"{s['description']}\") \\")
    L.append("")
    L.append("/* X(id, status_type, name, description) - name = pids.toml [[status]] name (e.g. for logs/UI) */")
    L.append("#define PM_STATUS_LIST(X) \\")
    for s in d.get("status", []):
        L.append(f"    X(0x{s['id']:04X}u, 0x{STATUS_TYPES[s['type']]:02X}u, \"{s['name']}\", \"{s['description']}\") \\")
    L.append("")
    L.append(f"#define PM_STATUS_COUNT {len(d.get('status', []))}u")
    L.append("")
    L.append("/* ---- manufacturer PIDs ---- */")
    bases: dict[str, list[dict]] = {}
    for p in d["pids"]:
        L.append(f"#define {c_pid_name(p)} 0x{p['pid']:04X}u /* {p['device']} {p['cc']} */")
        if "index" in p:
            bases.setdefault(p["base_name"], []).append(p)
    L.append("")
    L.append("/* port-indexed PIDs: PM_PID_<NAME>(port) with port 1..N */")
    for base, ps in bases.items():
        stride = ps[1]["pid"] - ps[0]["pid"] if len(ps) > 1 else 1
        name = base.replace("__", "_")
        L.append(f"#define PM_PID_{name}(port) ((uint16_t)(0x{ps[0]['pid']:04X}u + ((port) - 1u) * {stride}u))")
        L.append(f"#define PM_PID_{name}_COUNT {len(ps)}u")
    L.append("")
    L.append("/* ---- PDL sizes (maximum for variable-length data) ---- */")
    for p in d["pids"]:
        if "index" in p and p["index"] != 1:
            continue
        nm = p["base_name"].replace("__", "_")
        for k in ("get_request", "get_response", "set_request", "set_response"):
            if k in p:
                L.append(f"#define PM_PDL_{nm}_{k.upper()} {pdl(p[k])}u")
    L.append("")
    L.append("/* ---- PARAMETER_DESCRIPTION of the listed node PIDs ----")
    L.append(" * X(pid, pdl_size, data_type, command_class, unit, prefix, min, max, default, description) */")
    L.append("#define PM_NODE_PID_DESC_LIST(X) \\")
    for p in d["pids"]:
        if p["device"] != "node" or not p["listed"]:
            continue
        pd = p["pd"]
        L.append(f"    X(0x{p['pid']:04X}u, {pd['pdl_size']}u, 0x{DATA_TYPES[pd['data_type']]:02X}u, "
                 f"0x{CCS[p['cc']]:02X}u, 0x{UNITS[pd['unit']]:02X}u, 0x{PREFIXES[pd['prefix']]:02X}u, "
                 f"{pd['min']}, {pd['max']}, {pd['default']}, \"{pd['description']}\") \\")
    L.append("")
    std = d["node"]["standard_pids"]
    L.append("/* ---- node SUPPORTED_PARAMETERS: standard PIDs (required ones are not reported) ---- */")
    L.append("#define PM_NODE_STD_PIDS_REQUIRED \\")
    L.append("    " + ", ".join(f"0x{s['pid']:04X}u /* {s['name']} */" for s in std["required"]))
    L.append("#define PM_NODE_STD_PIDS_LISTED \\")
    L.append("    " + ", \\\n    ".join(f"0x{s['pid']:04X}u /* {s['name']} */" for s in std["listed"]))
    L.append("")
    return "\n".join(L)


# --------------------------------------------------------------------------------------------- Rust
def gen_rs(d: dict) -> str:
    L = ["// @generated by protocol/gen.py from protocol/pids.toml - do not edit",
         "//! Manufacturer-specific RDM definitions (pids.toml).", ""]
    for k, v in d["constants"].items():
        ty = "u32" if v["value"] > 0xFFFF else "u16"
        L.append(f"/// {v['doc']}")
        L.append(f"pub const {k}: {ty} = 0x{v['value']:X};")
    L.append("")
    L.append("/// Tube bootloader constants (rdm_boot.h).")
    L.append("pub mod tube {")
    for k, v in d["tube_constants"].items():
        L.append(f"    pub const {k}: u32 = 0x{v:X};")
    L.append("}")
    L.append("")
    for en, vals in d["enums"].items():
        L.append(f"/// Enumeration `{en}`.")
        L.append(f"pub mod {en} {{")
        for k, v in vals.items():
            L.append(f"    pub const {k}: u8 = {v};")
        L.append("}")
        L.append("")
    L.append("/// Manufacturer status message IDs.")
    L.append("pub mod status {")
    for s in d.get("status", []):
        L.append(f"    /// {s['type']}: \"{s['description']}\"")
        L.append(f"    pub const {s['name']}: u16 = 0x{s['id']:04X};")
    L.append("}")
    L.append("")
    L.append("""/// One manufacturer status message (pids.toml `[[status]]`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct StatusDef {
    pub id: u16,
    pub name: &'static str,
    /// E1.20 status type (0x02 advisory, 0x03 warning, 0x04 error).
    pub status_type: u8,
    /// STATUS_ID_DESCRIPTION text (may contain %d for data1/data2).
    pub description: &'static str,
}

/// All manufacturer status messages.
pub static STATUS: &[StatusDef] = &[""")
    for s in d.get("status", []):
        L.append(f"    StatusDef {{ id: 0x{s['id']:04X}, name: \"{s['name']}\", "
                 f"status_type: 0x{STATUS_TYPES[s['type']]:02X}, description: {rs_str(s['description'])} }},")
    L.append("];")
    L.append("")
    L.append("""/// Manufacturer status message definition by id.
pub fn status_def(id: u16) -> Option<&'static StatusDef> {
    STATUS.iter().find(|s| s.id == id)
}

/// Name of a manufacturer status message id (e.g. `"BOOT_CATCH_CAUGHT"`).
pub fn status_name(id: u16) -> Option<&'static str> {
    status_def(id).map(|s| s.name)
}

/// STATUS_ID_DESCRIPTION text of a manufacturer status message id.
pub fn status_description(id: u16) -> Option<&'static str> {
    status_def(id).map(|s| s.description)
}
""")
    for p in d["pids"]:
        L.append(f"/// {p['device']} {p['cc']}: {p.get('doc', '')}")
        L.append(f"pub const PID_{p['name']}: u16 = 0x{p['pid']:04X};")
    L.append("")
    L.append("""/// Which firmware answers a PID.
#[derive(Debug, Clone, Copy, PartialEq, Eq, serde::Serialize)]
#[serde(rename_all = "snake_case")]
pub enum Device {
    TubeApp,
    TubeBoot,
    Node,
}

/// PARAMETER_DESCRIPTION values (E1.20 codes).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ParamDesc {
    pub pdl_size: u8,
    pub data_type: u8,
    pub unit: u8,
    pub prefix: u8,
    pub min: u32,
    pub max: u32,
    pub default: u32,
    pub description: &'static str,
}

/// One manufacturer PID.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PidDef {
    pub pid: u16,
    pub name: &'static str,
    pub device: Device,
    /// E1.20 command class bits: 0x01 GET, 0x02 SET, 0x03 GET_SET.
    pub command_class: u8,
    /// Listed in SUPPORTED_PARAMETERS.
    pub listed: bool,
    pub get_request_pdl: Option<u8>,
    pub get_response_pdl: Option<u8>,
    pub set_request_pdl: Option<u8>,
    pub set_response_pdl: Option<u8>,
    pub desc: Option<ParamDesc>,
}

/// All manufacturer PIDs of pids.toml.
pub static PIDS: &[PidDef] = &[""")
    dev = {"tube_app": "Device::TubeApp", "tube_boot": "Device::TubeBoot", "node": "Device::Node"}
    for p in d["pids"]:
        def o(k):
            return f"Some({pdl(p[k])})" if k in p else "None"
        if p["listed"]:
            pd = p["pd"]
            desc = (f"Some(ParamDesc {{ pdl_size: {pd['pdl_size']}, data_type: 0x{DATA_TYPES[pd['data_type']]:02X}, "
                    f"unit: 0x{UNITS[pd['unit']]:02X}, prefix: 0x{PREFIXES[pd['prefix']]:02X}, min: {pd['min']}, "
                    f"max: {pd['max']}, default: {pd['default']}, description: {rs_str(pd['description'])} }})")
        else:
            desc = "None"
        L.append(f"    PidDef {{ pid: 0x{p['pid']:04X}, name: \"{p['name']}\", device: {dev[p['device']]}, "
                 f"command_class: 0x{CCS[p['cc']]:02X}, listed: {str(p['listed']).lower()}, "
                 f"get_request_pdl: {o('get_request')}, get_response_pdl: {o('get_response')}, "
                 f"set_request_pdl: {o('set_request')}, set_response_pdl: {o('set_response')}, desc: {desc} }},")
    L.append("];")
    L.append("")
    std = d["node"]["standard_pids"]
    L.append("/// Standard PIDs the node answers but does not report (E1.20 10.4.1).")
    L.append("pub const NODE_STD_PIDS_REQUIRED: &[u16] = &[" + ", ".join(f"0x{s['pid']:04X}" for s in std["required"]) + "];")
    L.append("/// Standard PIDs the node reports in SUPPORTED_PARAMETERS.")
    L.append("pub const NODE_STD_PIDS_LISTED: &[u16] = &[" + ", ".join(f"0x{s['pid']:04X}" for s in std["listed"]) + "];")
    L.append("")
    gen_rs_meta(d, L)
    return "\n".join(L)


RS_FIELD_TYPES = {"u8": "U8", "u16": "U16", "u32": "U32", "i8": "I8", "i16": "I16", "i32": "I32", "bool": "Bool",
                  "uid": "Uid", "ipv4": "Ipv4", "ascii": "Ascii", "bytes": "Bytes"}


def rs_opt(v, fmt=str) -> str:
    return "None" if v is None else f"Some({fmt(v)})"


def rs_field(f: dict, enums: dict) -> str:
    e = f.get("enum")
    return (f"PidField {{ name: \"{f['name']}\", ty: FieldType::{RS_FIELD_TYPES[f['type']]}, "
            f"len: {rs_opt(f.get('len'))}, max_len: {rs_opt(f.get('max_len'))}, "
            f"min: {rs_opt(f.get('min'))}, max: {rs_opt(f.get('max'))}, value: {rs_opt(f.get('value'), rs_str)}, "
            f"enum_name: {rs_opt(e if e in enums else None, rs_str)}, enum_ref: {rs_opt(e, rs_str)}, "
            f"doc: {rs_opt(f.get('doc'), rs_str)} }}")


def gen_rs_meta(d: dict, L: list[str]) -> None:
    """Field layouts, value kinds and PARAMETER_DESCRIPTION per expanded PID, plus the enum tables (additive)."""
    L.append("""// ---- per-PID metadata: field layouts + PARAMETER_DESCRIPTION (PID_META) and enum tables (ENUMS) ----

/// Type of one parameter data field (pids.toml `type`). Multi-byte values are big-endian.
#[derive(Debug, Clone, Copy, PartialEq, Eq, serde::Serialize)]
#[serde(rename_all = "lowercase")]
pub enum FieldType {
    U8,
    U16,
    U32,
    I8,
    I16,
    I32,
    /// One byte, 0 = false, 1 = true.
    Bool,
    /// RDM UID, 6 bytes.
    Uid,
    /// IPv4 address, 4 bytes.
    Ipv4,
    /// ASCII text, `len` (fixed) or up to `max_len` bytes.
    Ascii,
    /// Raw bytes, `len` (fixed) or up to `max_len` bytes.
    Bytes,
}

impl FieldType {
    /// Size in bytes of the fixed-size types (`None` for `Ascii`/`Bytes`).
    pub const fn fixed_size(self) -> Option<u16> {
        match self {
            FieldType::U8 | FieldType::I8 | FieldType::Bool => Some(1),
            FieldType::U16 | FieldType::I16 => Some(2),
            FieldType::U32 | FieldType::I32 | FieldType::Ipv4 => Some(4),
            FieldType::Uid => Some(6),
            FieldType::Ascii | FieldType::Bytes => None,
        }
    }
}

/// One field of a PDL layout (pids.toml `get_request`/`get_response`/`set_request`/`set_response` entry).
#[derive(Debug, Clone, Copy, PartialEq, Eq, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct PidField {
    pub name: &'static str,
    #[serde(rename = "type")]
    pub ty: FieldType,
    /// Fixed length of an `Ascii`/`Bytes` field.
    pub len: Option<u16>,
    /// Maximum length of a variable `Ascii`/`Bytes` field (must be the last field).
    pub max_len: Option<u16>,
    /// Valid range of the field, if pids.toml restricts it.
    pub min: Option<i64>,
    pub max: Option<i64>,
    /// Fixed content the field must have (e.g. `"BOOT"`).
    pub value: Option<&'static str>,
    /// Name of the `[enums.*]` table (see [`ENUMS`], [`enum_def`]) the value belongs to.
    pub enum_name: Option<&'static str>,
    /// Raw pids.toml `enum` reference: an `[enums.*]` name or a constant prefix such as `"BOOT_ERR_*"`.
    pub enum_ref: Option<&'static str>,
    pub doc: Option<&'static str>,
}

impl PidField {
    /// Maximum size in bytes.
    pub const fn max_size(&self) -> u16 {
        match (self.ty.fixed_size(), self.len, self.max_len) {
            (Some(n), _, _) => n,
            (None, Some(n), _) => n,
            (None, None, Some(n)) => n,
            (None, None, None) => 0,
        }
    }

    /// Minimum size in bytes (0 for variable-length fields).
    pub const fn min_size(&self) -> u16 {
        match (self.ty.fixed_size(), self.len) {
            (Some(n), _) => n,
            (None, Some(n)) => n,
            (None, None) => 0,
        }
    }
}

/// Everything pids.toml says about one (expanded) manufacturer PID: command classes, PDL layouts and
/// PARAMETER_DESCRIPTION values (the `[pid.pd]` fields are `None` for PIDs without one).
#[derive(Debug, Clone, Copy, PartialEq, Eq, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct PidMeta {
    pub pid: u16,
    /// Expanded name (`"PORT2_DMX_BREAK"`).
    pub name: &'static str,
    /// Name with `{n}` removed (`"PORT_DMX_BREAK"`), equal to `name` for non-repeated PIDs.
    pub base_name: &'static str,
    /// Port 1..N of a `repeat`-expanded PID.
    pub port: Option<u8>,
    pub device: Device,
    /// E1.20 command class bits: 0x01 GET, 0x02 SET, 0x03 GET_SET.
    pub command_class: u8,
    pub get: bool,
    pub set: bool,
    /// Listed in SUPPORTED_PARAMETERS (answers PARAMETER_DESCRIPTION).
    pub listed: bool,
    pub doc: &'static str,
    /// PDL layouts, `None` when the command class does not include GET resp. SET.
    pub get_request: Option<&'static [PidField]>,
    pub get_response: Option<&'static [PidField]>,
    pub set_request: Option<&'static [PidField]>,
    pub set_response: Option<&'static [PidField]>,
    /// PARAMETER_DESCRIPTION (E1.20 codes).
    pub pdl_size: Option<u8>,
    pub data_type: Option<u8>,
    pub unit: Option<u8>,
    pub prefix: Option<u8>,
    pub min: Option<i64>,
    pub max: Option<i64>,
    pub default: Option<i64>,
    pub description: Option<&'static str>,
}

impl PidMeta {
    /// Layout of the parameter value: GET response, or SET request for SET-only PIDs.
    pub fn value_fields(&self) -> &'static [PidField] {
        match (self.get_response, self.set_request) {
            (Some(f), _) | (None, Some(f)) => f,
            (None, None) => &[],
        }
    }

    /// The single value field of a single-value PID.
    pub fn value_field(&self) -> Option<&'static PidField> {
        match self.value_fields() {
            [f] => Some(f),
            _ => None,
        }
    }

    /// Single-value PID whose value is one bool field.
    pub fn is_bool(&self) -> bool {
        self.value_field().is_some_and(|f| f.ty == FieldType::Bool)
    }
}

/// One value of an enum table.
#[derive(Debug, Clone, Copy, PartialEq, Eq, serde::Serialize)]
pub struct EnumValue {
    pub name: &'static str,
    pub value: u8,
}

/// One pids.toml `[enums.*]` table.
#[derive(Debug, Clone, Copy, PartialEq, Eq, serde::Serialize)]
pub struct EnumDef {
    pub name: &'static str,
    pub values: &'static [EnumValue],
}

impl EnumDef {
    /// Name of a value.
    pub fn value_name(&self, value: u8) -> Option<&'static str> {
        self.values.iter().find(|v| v.value == value).map(|v| v.name)
    }

    /// Value of a name.
    pub fn value_of(&self, name: &str) -> Option<u8> {
        self.values.iter().find(|v| v.name == name).map(|v| v.value)
    }
}

/// All enum tables of pids.toml.
pub static ENUMS: &[EnumDef] = &[""")
    for en, vals in d["enums"].items():
        vs = ", ".join(f"EnumValue {{ name: \"{k}\", value: {v} }}" for k, v in vals.items())
        L.append(f"    EnumDef {{ name: \"{en}\", values: &[{vs}] }},")
    L.append("];")
    L.append("")
    L.append("""/// Enum table by name (e.g. `"fw_state"`).
pub fn enum_def(name: &str) -> Option<&'static EnumDef> {
    ENUMS.iter().find(|e| e.name == name)
}

/// Metadata of all manufacturer PIDs (same order as [`PIDS`]).
pub static PID_META: &[PidMeta] = &[""")
    dev = {"tube_app": "Device::TubeApp", "tube_boot": "Device::TubeBoot", "node": "Device::Node"}
    for p in d["pids"]:
        cc = p["cc"]
        def layout(k):
            if k not in p:
                return "None"
            return "Some(&[" + ", ".join(rs_field(f, d["enums"]) for f in p[k]) + "])"
        pd = p.get("pd") if p["listed"] else None
        def g(k, table=None):
            if pd is None:
                return "None"
            v = pd[k]
            return f"Some({table[v]})" if table else f"Some({v})"
        L.append(f"    PidMeta {{ pid: 0x{p['pid']:04X}, name: \"{p['name']}\", base_name: \"{p['base_name']}\", "
                 f"port: {rs_opt(p.get('index'))}, device: {dev[p['device']]}, command_class: 0x{CCS[cc]:02X}, "
                 f"get: {str('get' in cc).lower()}, set: {str('set' in cc).lower()}, "
                 f"listed: {str(p['listed']).lower()}, doc: {rs_str(p.get('doc', ''))},")
        L.append(f"        get_request: {layout('get_request')},")
        L.append(f"        get_response: {layout('get_response')},")
        L.append(f"        set_request: {layout('set_request')},")
        L.append(f"        set_response: {layout('set_response')},")
        L.append(f"        pdl_size: {g('pdl_size')}, data_type: {g('data_type', DATA_TYPES)}, unit: {g('unit', UNITS)}, "
                 f"prefix: {g('prefix', PREFIXES)}, min: {g('min')}, max: {g('max')}, default: {g('default')}, "
                 f"description: {rs_opt(pd['description'] if pd else None, rs_str)} }},")
    L.append("];")
    L.append("")
    L.append("""/// Metadata of a manufacturer PID.
pub fn pid_meta(pid: u16) -> Option<&'static PidMeta> {
    PID_META.iter().find(|m| m.pid == pid)
}
""")


def rs_str(s: str) -> str:
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


# --------------------------------------------------------------------------------------------- tube check
def read_defines(path: pathlib.Path) -> dict[str, int]:
    """Same parser as tube/tools/flash_rdm.py."""
    return {m.group(1): int(m.group(2), 0) for m in
            re.finditer(r"^#define\s+(\w+)\s+(0[xX][0-9A-Fa-f]+|\d+)\b", path.read_text(), re.M)}


def check_tube(d: dict) -> list[str]:
    errs = []
    h = read_defines(TUBE_PID_HEADER)
    expected: dict[str, int] = {}
    for p in d["pids"]:
        if p["device"].startswith("tube"):
            if "c_name" not in p:
                errs.append(f"tube PID {p['name']} needs c_name (its #define in rdm_boot.h)")
                continue
            expected[p["c_name"]] = p["pid"]
    expected.update(d["tube_constants"])
    for k, v in expected.items():
        if k not in h:
            errs.append(f"rdm_boot.h: missing #define {k} (pids.toml: 0x{v:X})")
        elif h[k] != v:
            errs.append(f"rdm_boot.h: {k} = 0x{h[k]:X}, pids.toml says 0x{v:X}")
    for k in h:
        if k not in expected:
            errs.append(f"rdm_boot.h: #define {k} is not in pids.toml (add it to [tube_constants] or a [[pid]])")
    cfg = read_defines(TUBE_CONFIG)
    for k, ref in (("RDM_MANUFACTURER_ID", "ESTA_MANUFACTURER"), ("RDM_DEVICE_MODEL_ID", "MODEL_TUBE")):
        if cfg.get(k) != d["constants"][ref]["value"]:
            errs.append(f"config.h: {k} = {cfg.get(k)} != pids.toml {ref}")
    return errs


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="validate only, do not write")
    a = ap.parse_args()
    try:
        d = load()
    except (GenError, KeyError, tomllib.TOMLDecodeError) as e:
        print(f"pids.toml: {e}", file=sys.stderr)
        return 1
    outputs = {C_OUT: gen_c(d), RS_OUT: gen_rs(d)}
    if a.check:
        errs = check_tube(d)
        for path, text in outputs.items():
            if not path.exists() or path.read_text() != text:
                errs.append(f"{path.relative_to(ROOT)} is out of date - run python3 protocol/gen.py")
        for e in errs:
            print("FAIL:", e, file=sys.stderr)
        if errs:
            return 1
        print(f"ok: {len(d['pids'])} PIDs, rdm_boot.h + config.h consistent, generated files up to date")
        return 0
    for path, text in outputs.items():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        print("wrote", path.relative_to(ROOT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
