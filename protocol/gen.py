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
    for p in d["pids"]:
        L.append(f"/// {p['device']} {p['cc']}: {p.get('doc', '')}")
        L.append(f"pub const PID_{p['name']}: u16 = 0x{p['pid']:04X};")
    L.append("")
    L.append("""/// Which firmware answers a PID.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
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
    return "\n".join(L)


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
