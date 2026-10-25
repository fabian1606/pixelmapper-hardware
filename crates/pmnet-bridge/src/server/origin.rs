//! `Origin` allow-list of the WebSocket upgrade (contract §1 Connection). Patterns are `scheme://host`,
//! `scheme://host:port` or `scheme://host:*` (any port, including none). Scheme and host compare
//! case-insensitively; paths are not allowed.

use std::fmt;

/// Origins every bridge accepts (contract §1).
pub const DEFAULT_ORIGINS: &[&str] = &[
    "http://localhost:*",
    "http://127.0.0.1:*",
    "tauri://localhost",
    "http://tauri.localhost",
    "https://tauri.localhost",
];

/// Hosted Pixelmapper origins, accepted by default as well.
///
/// TODO: fill in once the production domains are fixed (contract §10 Open); until then pass them with
/// `--allow-origin`.
pub const HOSTED_ORIGINS: &[&str] = &[];

/// Port part of a pattern.
#[derive(Debug, Clone, PartialEq, Eq)]
enum PortPattern {
    /// `:*` – any port, or none.
    Any,
    /// No port in the pattern – the origin must not have one either.
    Absent,
    /// Exactly this port.
    Exact(u16),
}

/// One allowed origin.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct OriginPattern {
    scheme: String,
    host: String,
    port: PortPattern,
}

/// A pattern that could not be parsed.
#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
#[error(
    "invalid origin pattern {0:?} (expected scheme://host, scheme://host:port or scheme://host:*)"
)]
pub struct OriginPatternError(pub String);

/// Splits `scheme://host[:port]` into lowercase scheme, lowercase host and the raw port text.
fn split_origin(s: &str) -> Option<(String, String, Option<&str>)> {
    let (scheme, rest) = s.split_once("://")?;
    if scheme.is_empty() || rest.is_empty() || rest.contains('/') {
        return None;
    }
    // IPv6 literal: [::1]:port
    let (host, port) = if let Some(after) = rest.strip_prefix('[') {
        let (h, tail) = after.split_once(']')?;
        let port = match tail {
            "" => None,
            t => Some(t.strip_prefix(':')?),
        };
        (format!("[{}]", h.to_ascii_lowercase()), port)
    } else {
        match rest.rsplit_once(':') {
            Some((h, p)) => (h.to_ascii_lowercase(), Some(p)),
            None => (rest.to_ascii_lowercase(), None),
        }
    };
    if host.is_empty() || host == "[]" {
        return None;
    }
    Some((scheme.to_ascii_lowercase(), host, port))
}

impl OriginPattern {
    /// Parses a pattern.
    pub fn parse(s: &str) -> Result<Self, OriginPatternError> {
        let err = || OriginPatternError(s.to_owned());
        let (scheme, host, port) = split_origin(s.trim()).ok_or_else(err)?;
        let port = match port {
            None => PortPattern::Absent,
            Some("*") => PortPattern::Any,
            Some(p) => PortPattern::Exact(p.parse().map_err(|_| err())?),
        };
        Ok(Self { scheme, host, port })
    }

    /// True when the `Origin` header value matches this pattern.
    pub fn matches(&self, origin: &str) -> bool {
        let Some((scheme, host, port)) = split_origin(origin.trim()) else {
            return false;
        };
        if scheme != self.scheme || host != self.host {
            return false;
        }
        match (&self.port, port) {
            (PortPattern::Any, None) => true,
            (PortPattern::Any, Some(p)) => p.parse::<u16>().is_ok(),
            (PortPattern::Absent, None) => true,
            (PortPattern::Exact(want), Some(p)) => p.parse::<u16>().ok() == Some(*want),
            _ => false,
        }
    }
}

impl fmt::Display for OriginPattern {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}://{}", self.scheme, self.host)?;
        match self.port {
            PortPattern::Any => f.write_str(":*"),
            PortPattern::Absent => Ok(()),
            PortPattern::Exact(p) => write!(f, ":{p}"),
        }
    }
}

/// The complete allow-list.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct OriginPolicy {
    patterns: Vec<OriginPattern>,
}

impl OriginPolicy {
    /// Defaults (`DEFAULT_ORIGINS` + `HOSTED_ORIGINS`) plus `extra` patterns.
    pub fn with_defaults<S: AsRef<str>>(extra: &[S]) -> Result<Self, OriginPatternError> {
        let defaults = DEFAULT_ORIGINS
            .iter()
            .chain(HOSTED_ORIGINS)
            .map(|s| OriginPattern::parse(s));
        let extra = extra.iter().map(|s| OriginPattern::parse(s.as_ref()));
        Ok(Self {
            patterns: defaults.chain(extra).collect::<Result<_, _>>()?,
        })
    }

    /// Exactly the given patterns (no defaults).
    pub fn only(patterns: Vec<OriginPattern>) -> Self {
        Self { patterns }
    }

    /// The patterns, for logging.
    pub fn patterns(&self) -> &[OriginPattern] {
        &self.patterns
    }

    /// Decision for an upgrade request: no `Origin` header (CLI tools, tests) is allowed; otherwise it must match.
    pub fn allows(&self, origin: Option<&str>) -> bool {
        match origin {
            None => true,
            Some(o) => self.patterns.iter().any(|p| p.matches(o)),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn defaults() {
        let p = OriginPolicy::with_defaults::<&str>(&[]).unwrap();
        assert!(p.allows(None));
        assert!(p.allows(Some("http://localhost:3000")));
        assert!(p.allows(Some("http://localhost")));
        assert!(p.allows(Some("HTTP://LocalHost:3000")));
        assert!(p.allows(Some("http://127.0.0.1:5173")));
        assert!(p.allows(Some("tauri://localhost")));
        assert!(p.allows(Some("https://tauri.localhost")));
        assert!(!p.allows(Some("https://localhost:3000")));
        assert!(!p.allows(Some("http://localhost.evil.com")));
        assert!(!p.allows(Some("http://evil.com")));
        assert!(!p.allows(Some("null")));
        assert!(!p.allows(Some("tauri://localhost:1")));
        assert!(!p.allows(Some("http://localhost:x")));
        assert!(!p.allows(Some("")));
    }

    #[test]
    fn extra_patterns() {
        let p = OriginPolicy::with_defaults(&[
            "https://app.example.org",
            "http://[::1]:*",
            "http://x.test:8080",
        ])
        .unwrap();
        assert!(p.allows(Some("https://app.example.org")));
        assert!(!p.allows(Some("https://app.example.org:444")));
        assert!(p.allows(Some("http://[::1]:3000")));
        assert!(p.allows(Some("http://x.test:8080")));
        assert!(!p.allows(Some("http://x.test:8081")));
        assert!(OriginPolicy::with_defaults(&["app.example.org"]).is_err());
        assert!(OriginPolicy::with_defaults(&["https://app.example.org/path"]).is_err());
        assert_eq!(
            OriginPattern::parse("HTTP://LocalHost:*")
                .unwrap()
                .to_string(),
            "http://localhost:*"
        );
    }
}
