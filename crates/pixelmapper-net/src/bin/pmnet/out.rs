//! Output: human text or one JSON object per line (`--json`).

use pixelmapper_net::Error;
use serde_json::{json, Value};

/// Output mode.
pub struct Out {
    pub json: bool,
}

impl Out {
    pub fn new(json: bool) -> Self {
        Self { json }
    }

    /// Print a result: JSON line `obj` with `--json`, else the text produced by `text`.
    pub fn emit(&self, obj: Value, text: impl FnOnce() -> String) {
        if self.json {
            println!("{obj}");
        } else {
            let t = text();
            if !t.is_empty() {
                println!("{t}");
            }
        }
    }

    /// Human-only remark on stderr (suppressed with `--json`).
    pub fn note(&self, msg: &str) {
        if !self.json {
            eprintln!("{msg}");
        }
    }
}

/// Print an error on stderr.
pub fn error(json: bool, e: &Error) {
    if json {
        eprintln!(
            "{}",
            json!({"type": "error", "code": e.code(), "message": e.to_string()})
        );
    } else {
        eprintln!("pmnet: error [{}]: {e}", e.code());
    }
}

/// Serialize anything serializable (never fails for our types; `null` otherwise).
pub fn to_value<T: serde::Serialize>(v: &T) -> Value {
    serde_json::to_value(v).unwrap_or(Value::Null)
}

/// Render a JSON value as indented `key: value` lines.
pub fn pretty(v: &Value, indent: usize) -> String {
    let pad = " ".repeat(indent);
    match v {
        Value::Object(m) => m
            .iter()
            .map(|(k, v)| match v {
                Value::Object(_) => format!("{pad}{k}:\n{}", pretty(v, indent + 2)),
                Value::Array(a) if a.iter().any(|x| x.is_object()) => {
                    format!("{pad}{k}:\n{}", pretty(v, indent + 2))
                }
                _ => format!("{pad}{k}: {}", scalar(v)),
            })
            .collect::<Vec<_>>()
            .join("\n"),
        Value::Array(a) => a
            .iter()
            .enumerate()
            .map(|(i, x)| match x {
                Value::Object(_) => format!("{pad}[{i}]\n{}", pretty(x, indent + 2)),
                _ => format!("{pad}- {}", scalar(x)),
            })
            .collect::<Vec<_>>()
            .join("\n"),
        _ => format!("{pad}{}", scalar(v)),
    }
}

fn scalar(v: &Value) -> String {
    match v {
        Value::String(s) => s.clone(),
        Value::Array(a) => a.iter().map(scalar).collect::<Vec<_>>().join(", "),
        other => other.to_string(),
    }
}
