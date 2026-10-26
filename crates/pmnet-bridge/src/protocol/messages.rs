//! Commands (app → bridge), their arguments and results, the message envelope and the version rule
//! (`protocol/bridge-ws.md` §1, §2, §6).

use super::error::{CommandError, ErrorCode};
use super::ids::{HexData, NodeId, PortRef, RdmUid};
use super::model::{
    DmxRoute, Failsafe, FlashJob, MergeMode, NodeState, OutputStyle, RdmValue, Tod,
};
use serde::de::DeserializeOwned;
use serde::{Deserialize, Serialize};
use serde_json::{Map, Value};
use std::net::Ipv4Addr;

/// Protocol version implemented by this bridge (contract §1 Versioning).
pub const PROTOCOL_VERSION: &str = "1.0.0";

// ── Version rule ──────────────────────────────────────────────────────────────────────────────────────────────────

/// Semver major of a version string (`"1.2.3"`, `"1.2.3-rc.1"`); `None` when unparsable.
pub fn semver_major(version: &str) -> Option<u64> {
    let mut parts = version.splitn(3, '.');
    let major = parts.next()?;
    let minor = parts.next()?;
    let patch = parts.next()?;
    let digits = |s: &str| !s.is_empty() && s.bytes().all(|b| b.is_ascii_digit());
    let patch_digits: String = patch.chars().take_while(char::is_ascii_digit).collect();
    if !digits(major) || !digits(minor) || patch_digits.is_empty() {
        return None;
    }
    major.parse().ok()
}

// ── Command arguments ─────────────────────────────────────────────────────────────────────────────────────────────

/// `hello` (client handshake).
#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct HelloArgs {
    /// Client name, e.g. `"pixelmapper"` (informational).
    #[serde(default)]
    pub client: String,
    /// Client version (informational).
    #[serde(default)]
    pub client_version: String,
    /// Protocol semver the client implements.
    pub protocol: String,
}

/// `todRequest`.
#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
#[allow(missing_docs)]
pub struct NodePortArgs {
    pub node_id: NodeId,
    pub port: PortRef,
}

/// ArtTodControl command of `todControl`.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum TodAction {
    /// `AtcFlush`: clear the TOD and run a full discovery.
    Flush,
    /// `AtcEnd`: end discovery.
    End,
    /// `AtcIncOn`: background discovery of the port on.
    IncOn,
    /// `AtcIncOff`: background discovery of the port off.
    IncOff,
}

/// `todControl`.
#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
#[allow(missing_docs)]
pub struct TodControlArgs {
    pub node_id: NodeId,
    pub port: PortRef,
    pub action: TodAction,
}

/// `addPollTarget` / `removePollTarget`.
#[derive(Debug, Clone, Deserialize)]
#[allow(missing_docs)]
pub struct PollTargetArgs {
    pub ip: Ipv4Addr,
}

/// `rdmGet` / `rdmSet`: RDM addressing, PID and optional parameter data (hex) or decoded value.
#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
#[allow(missing_docs)]
pub struct RdmArgs {
    pub node_id: NodeId,
    pub port: PortRef,
    pub uid: RdmUid,
    #[serde(default)]
    pub sub_device: u16,
    pub pid: u16,
    #[serde(default)]
    pub data: Option<HexData>,
    #[serde(default)]
    pub value: Option<RdmValue>,
}

/// `rdmDescribe`.
#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
#[allow(missing_docs)]
pub struct RdmDescribeArgs {
    pub node_id: NodeId,
    pub port: PortRef,
    pub uid: RdmUid,
    #[serde(default)]
    pub sub_device: u16,
    #[serde(default)]
    pub refresh: bool,
}

/// Direction a port is switched to (AcDirectionTx/Rx).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
#[allow(missing_docs)]
pub enum SetDirection {
    Output,
    Input,
}

/// Output protocol (AcArtNetSel / AcAcnSel).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
#[allow(missing_docs)]
pub enum OutputProtocol {
    Artnet,
    Sacn,
}

/// Indicator command (AcLedNormal/Mute/Locate).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
#[allow(missing_docs)]
pub enum SetIndicator {
    Normal,
    Mute,
    Locate,
}

/// Per-port changes of `artAddress`. `None`/`false` = no change.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
#[allow(missing_docs)]
pub struct ArtAddressPort {
    pub port: PortRef,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub output_port_address: Option<u16>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub input_port_address: Option<u16>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub direction: Option<SetDirection>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub merge: Option<MergeMode>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub rdm: Option<bool>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub output_style: Option<OutputStyle>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub protocol: Option<OutputProtocol>,
    #[serde(default)]
    pub clear_output: bool,
    /// AcFailHold/Zero/Full/Scene to the port's BindIndex.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub failsafe: Option<Failsafe>,
    /// AcFailRecord to the port's BindIndex.
    #[serde(default)]
    pub record_failsafe_scene: bool,
}

/// `artAddress` (all optional except `nodeId`; contract §6 Semantics).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
#[allow(missing_docs)]
pub struct ArtAddressArgs {
    pub node_id: NodeId,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub short_name: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub long_name: Option<String>,
    #[serde(default)]
    pub ports: Vec<ArtAddressPort>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub indicator: Option<SetIndicator>,
    #[serde(default)]
    pub cancel_merge: bool,
    #[serde(default)]
    pub reset_rx_flags: bool,
}

/// `ipProg`.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
#[allow(missing_docs)]
pub struct IpProgArgs {
    pub node_id: NodeId,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub dhcp: Option<bool>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub ip: Option<Ipv4Addr>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub mask: Option<Ipv4Addr>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub gateway: Option<Ipv4Addr>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub reset_to_default: Option<bool>,
}

/// `artInput`.
#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
#[allow(missing_docs)]
pub struct ArtInputArgs {
    pub node_id: NodeId,
    pub bind: u8,
    pub disabled: Vec<bool>,
}

/// `imageInfo`.
#[derive(Debug, Clone, Deserialize)]
#[allow(missing_docs)]
pub struct ImageInfoArgs {
    /// Intel HEX file content.
    pub hex: String,
}

/// `flash`.
#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
#[allow(missing_docs)]
pub struct FlashArgs {
    pub node_id: NodeId,
    pub port: PortRef,
    pub uid: RdmUid,
    #[serde(default)]
    pub sub_device: u16,
    pub hex: String,
    #[serde(default)]
    pub allow_downgrade: bool,
}

/// `flashCancel`.
#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
#[allow(missing_docs)]
pub struct JobArgs {
    pub job_id: String,
}

/// `dmxRoutes`.
#[derive(Debug, Clone, Deserialize)]
#[allow(missing_docs)]
pub struct DmxRoutesArgs {
    pub routes: Vec<DmxRoute>,
}

// ── Command ───────────────────────────────────────────────────────────────────────────────────────────────────────

/// A parsed command (without `id`).
#[derive(Debug, Clone)]
#[allow(missing_docs)]
pub enum Command {
    Hello(HelloArgs),
    Nodes,
    Poll,
    AddPollTarget(PollTargetArgs),
    RemovePollTarget(PollTargetArgs),
    TodRequest(NodePortArgs),
    TodControl(TodControlArgs),
    RdmGet(RdmArgs),
    RdmSet(RdmArgs),
    RdmDescribe(RdmDescribeArgs),
    ArtAddress(ArtAddressArgs),
    IpProg(IpProgArgs),
    ArtInput(ArtInputArgs),
    ImageInfo(ImageInfoArgs),
    Flash(FlashArgs),
    FlashCancel(JobArgs),
    FlashJobs,
    DmxRoutes(DmxRoutesArgs),
}

impl Command {
    /// Parses the arguments of command `kind` from the whole message object. `Ok(None)` = unknown command.
    /// Unknown argument fields are ignored (contract §1 Versioning).
    pub fn parse(kind: &str, body: Value) -> Result<Option<Command>, CommandError> {
        let cmd = match kind {
            "hello" => Command::Hello(args(body)?),
            "nodes" => Command::Nodes,
            "poll" => Command::Poll,
            "addPollTarget" => Command::AddPollTarget(args(body)?),
            "removePollTarget" => Command::RemovePollTarget(args(body)?),
            "todRequest" => Command::TodRequest(args(body)?),
            "todControl" => Command::TodControl(args(body)?),
            "rdmGet" => Command::RdmGet(args(body)?),
            "rdmSet" => Command::RdmSet(args(body)?),
            "rdmDescribe" => Command::RdmDescribe(args(body)?),
            "artAddress" => Command::ArtAddress(args(body)?),
            "ipProg" => Command::IpProg(args(body)?),
            "artInput" => Command::ArtInput(args(body)?),
            "imageInfo" => Command::ImageInfo(args(body)?),
            "flash" => Command::Flash(args(body)?),
            "flashCancel" => Command::FlashCancel(args(body)?),
            "flashJobs" => Command::FlashJobs,
            "dmxRoutes" => Command::DmxRoutes(args(body)?),
            _ => return Ok(None),
        };
        Ok(Some(cmd))
    }
}

/// Deserializes command arguments; failures become `badRequest` with `details.field` = path of the bad argument.
fn args<T: DeserializeOwned>(body: Value) -> Result<T, CommandError> {
    serde_path_to_error::deserialize::<_, T>(body).map_err(|e| {
        let path = e.path().to_string();
        let message = e.inner().to_string();
        let field = if path == "." {
            missing_field(&message).unwrap_or_default()
        } else {
            path
        };
        CommandError::bad_request(field, message)
    })
}

/// Extracts `x` from serde's "missing field `x`" message.
fn missing_field(message: &str) -> Option<String> {
    let rest = message.strip_prefix("missing field `")?;
    Some(rest.split('`').next()?.to_owned())
}

// ── Envelope ──────────────────────────────────────────────────────────────────────────────────────────────────────

/// A text message split into `id`, `type` and the whole object (the arguments).
#[derive(Debug, Clone)]
pub struct Envelope {
    /// Correlation id chosen by the app.
    pub id: u32,
    /// Command type.
    pub kind: String,
    /// The complete message object.
    pub body: Value,
}

/// A message that could not be turned into an envelope; `id` is set when it was readable.
#[derive(Debug, Clone)]
pub struct EnvelopeError {
    /// Id of the message, if readable.
    pub id: Option<u32>,
    /// Error to send back.
    pub error: CommandError,
}

/// Parses a text message into an envelope (contract §2): a JSON object with a uint32 `id` and a string `type`.
pub fn parse_envelope(text: &str) -> Result<Envelope, EnvelopeError> {
    let fail = |id, error| EnvelopeError { id, error };
    let body: Value = serde_json::from_str(text).map_err(|e| {
        fail(
            None,
            CommandError::bad_request("", format!("malformed JSON: {e}")),
        )
    })?;
    let Some(obj) = body.as_object() else {
        return Err(fail(
            None,
            CommandError::bad_request("", "message must be a JSON object"),
        ));
    };
    let id = match obj.get("id") {
        Some(v) => match v.as_u64().and_then(|n| u32::try_from(n).ok()) {
            Some(id) => id,
            None => {
                return Err(fail(
                    None,
                    CommandError::bad_request("id", "id must be a uint32"),
                ))
            }
        },
        None => return Err(fail(None, CommandError::bad_request("id", "missing id"))),
    };
    let Some(kind) = obj.get("type").and_then(Value::as_str).map(str::to_owned) else {
        return Err(fail(
            Some(id),
            CommandError::bad_request("type", "missing or non-string type"),
        ));
    };
    Ok(Envelope { id, kind, body })
}

// ── Results ───────────────────────────────────────────────────────────────────────────────────────────────────────

/// `{}` result.
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct Empty {}

/// `nodes` result.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[allow(missing_docs)]
pub struct NodesResult {
    pub nodes: Vec<NodeState>,
    pub tods: Vec<Tod>,
}

/// `rdmGet` / `rdmSet` result.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[allow(missing_docs)]
pub struct RdmResult {
    pub data: HexData,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub value: Option<RdmValue>,
    /// `Some(true)` for requests to broadcast UIDs.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub broadcast: Option<bool>,
}

/// `artAddress` / `artInput` result.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[allow(missing_docs)]
pub struct NodeResult {
    pub node: NodeState,
}

/// `ipProg` result (ArtIpProgReply fields).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[allow(missing_docs)]
pub struct IpProgReply {
    pub ip: Ipv4Addr,
    pub mask: Ipv4Addr,
    pub gateway: Ipv4Addr,
    pub dhcp: bool,
}

/// `flash` result.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[allow(missing_docs)]
pub struct FlashStartResult {
    pub job: FlashJob,
}

/// `flashJobs` result.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[allow(missing_docs)]
pub struct FlashJobsResult {
    pub jobs: Vec<FlashJob>,
}

// ── Reply serialization ───────────────────────────────────────────────────────────────────────────────────────────

/// Serializes `{ "type": "result", "id", "result" }`. A result that cannot be serialized (a bug) becomes `internal`.
pub fn result_json<T: Serialize>(id: u32, result: &T) -> String {
    #[derive(Serialize)]
    struct ResultMsg<'a, T> {
        #[serde(rename = "type")]
        kind: &'static str,
        id: u32,
        result: &'a T,
    }
    serde_json::to_string(&ResultMsg {
        kind: "result",
        id,
        result,
    })
    .unwrap_or_else(|e| {
        error_json(
            Some(id),
            &CommandError::new(ErrorCode::Internal, format!("result not serializable: {e}")),
        )
    })
}

/// Serializes `{ "type": "error", "id", "code", "message", "details"? }`.
pub fn error_json(id: Option<u32>, error: &CommandError) -> String {
    #[derive(Serialize)]
    struct ErrorMsg<'a> {
        #[serde(rename = "type")]
        kind: &'static str,
        id: Option<u32>,
        code: ErrorCode,
        message: &'a str,
        #[serde(skip_serializing_if = "Option::is_none")]
        details: Option<&'a Map<String, Value>>,
    }
    let msg = ErrorMsg {
        kind: "error",
        id,
        code: error.code,
        message: &error.message,
        details: error.details.as_ref(),
    };
    // Only strings, numbers and JSON values: serialization cannot fail.
    serde_json::to_string(&msg).unwrap_or_default()
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn semver() {
        assert_eq!(semver_major("1.0.0"), Some(1));
        assert_eq!(semver_major("12.3.4-rc.1"), Some(12));
        assert_eq!(semver_major("1.0"), None);
        assert_eq!(semver_major("x.0.0"), None);
        assert_eq!(semver_major(""), None);
    }

    #[test]
    fn envelope_errors() {
        let e = parse_envelope("{").unwrap_err();
        assert_eq!((e.id, e.error.code), (None, ErrorCode::BadRequest));
        let e = parse_envelope("[1]").unwrap_err();
        assert_eq!(e.id, None);
        let e = parse_envelope(r#"{"type":"nodes"}"#).unwrap_err();
        assert_eq!(e.error.details.unwrap()["field"], "id");
        let e = parse_envelope(r#"{"type":"nodes","id":-1}"#).unwrap_err();
        assert_eq!(e.id, None);
        let e = parse_envelope(r#"{"id":7}"#).unwrap_err();
        assert_eq!(e.id, Some(7));
        let ok = parse_envelope(r#"{"type":"nodes","id":7}"#).unwrap();
        assert_eq!((ok.id, ok.kind.as_str()), (7, "nodes"));
    }

    #[test]
    fn command_parse_paths() {
        assert!(Command::parse("nope", json!({})).unwrap().is_none());
        let err = Command::parse(
            "todRequest",
            json!({"type":"todRequest","id":1,"port":{"bind":1,"index":0}}),
        )
        .unwrap_err();
        assert_eq!(err.details.unwrap()["field"], "nodeId");
        let err = Command::parse(
            "todRequest",
            json!({"nodeId":"n","port":{"bind":1,"index":"x"}}),
        )
        .unwrap_err();
        assert_eq!(err.details.unwrap()["field"], "port.index");
        let err = Command::parse(
            "rdmGet",
            json!({"nodeId":"n","port":{"bind":1,"index":0},"uid":"zz","pid":96}),
        )
        .unwrap_err();
        assert_eq!(err.details.unwrap()["field"], "uid");
        // Unknown fields are ignored.
        let ok = Command::parse("poll", json!({"type":"poll","id":3,"extra":true})).unwrap();
        assert!(matches!(ok, Some(Command::Poll)));
    }

    #[test]
    fn reply_shapes() {
        assert_eq!(
            result_json(4, &Empty {}),
            r#"{"type":"result","id":4,"result":{}}"#
        );
        let err = CommandError::bad_request("uid", "bad");
        assert_eq!(
            error_json(None, &err),
            r#"{"type":"error","id":null,"code":"badRequest","message":"bad","details":{"field":"uid"}}"#
        );
    }
}
