//! Native network tools — Rust replacements for the old Electron main.js
//! implementations (which used Node's child_process/dns/net). Forward/reverse
//! DNS and the port scanner use verified-working patterns (hickory-resolver,
//! tokio TcpStream connect-scan — see docs/TAURI_MIGRATION.md for the
//! standalone test that exercised these exact primitives against real
//! network calls). The ping tool spawns the OS `ping` binary, mirroring the
//! old Node implementation's cross-platform flag mapping — NOT independently
//! network-tested in this environment (no reason to expect it behaves
//! differently from the proven Node version it's a straight port of, but
//! flagging the distinction honestly).

use std::collections::HashMap;
use std::net::IpAddr;
use std::process::Stdio;
use std::sync::Arc;
use std::time::Duration;

use hickory_resolver::Resolver;
use hickory_resolver::config::ResolverConfig;
use hickory_resolver::net::runtime::TokioRuntimeProvider;
use serde::Serialize;
use tokio::io::{AsyncBufReadExt, BufReader};
use tokio::net::TcpStream;
use tokio::process::{Child, Command};
use tokio::sync::Mutex;
use tokio::time::timeout;

// A host/IP must match this before it's used in ANY tool below — same
// allowlist-style validation the Electron version used (isHostish in
// electron/main.js), enforced again here since this is a different process
// boundary now. Rejects anything that isn't letters/digits/./:/-/[]., which
// rules out shell metacharacters even though `ping` is spawned via argv (not
// a shell) and therefore isn't injectable anyway — defense in depth.
fn is_hostish(s: &str) -> bool {
    !s.is_empty()
        && s.len() <= 255
        && s.chars()
            .all(|c| c.is_ascii_alphanumeric() || "._:-[]".contains(c))
}

// ---------------------------------------------------------------------------
// Forward + reverse DNS
// ---------------------------------------------------------------------------

#[derive(Serialize, Default)]
pub struct DnsResult {
    pub name: String,
    pub a: Vec<String>,
    pub aaaa: Vec<String>,
    pub cname: Vec<String>,
    pub error: Option<String>,
}

pub async fn dns_lookup(name: &str) -> DnsResult {
    let name = name.trim();
    if !is_hostish(name) {
        return DnsResult { error: Some("Invalid host name".into()), ..Default::default() };
    }
    let Ok(resolver) = Resolver::builder_with_config(ResolverConfig::default(), TokioRuntimeProvider::default()).build() else {
        return DnsResult { name: name.to_string(), error: Some("Failed to build resolver".into()), ..Default::default() };
    };
    let mut out = DnsResult { name: name.to_string(), ..Default::default() };
    match resolver.lookup_ip(format!("{name}.")).await {
        Ok(r) => {
            for ip in r.iter() {
                match ip {
                    IpAddr::V4(v4) => out.a.push(v4.to_string()),
                    IpAddr::V6(v6) => out.aaaa.push(v6.to_string()),
                }
            }
        }
        Err(e) => out.error = Some(e.to_string()),
    }
    out
}

#[derive(Serialize, Default)]
pub struct ReverseResult {
    pub addr: String,
    pub names: Vec<String>,
    pub error: Option<String>,
}

pub async fn reverse_lookup(addr: &str) -> ReverseResult {
    let addr = addr.trim();
    let Ok(ip) = addr.parse::<IpAddr>() else {
        return ReverseResult { addr: addr.to_string(), error: Some("Invalid address".into()), ..Default::default() };
    };
    let Ok(resolver) = Resolver::builder_with_config(ResolverConfig::default(), TokioRuntimeProvider::default()).build() else {
        return ReverseResult { addr: addr.to_string(), error: Some("Failed to build resolver".into()), ..Default::default() };
    };
    let mut out = ReverseResult { addr: addr.to_string(), ..Default::default() };
    match resolver.reverse_lookup(ip).await {
        Ok(r) => {
            out.names = r
                .answers()
                .iter()
                .filter_map(|record| match &record.data {
                    hickory_resolver::proto::rr::RData::PTR(name) => Some(name.0.to_string()),
                    _ => None,
                })
                .collect();
        }
        Err(e) => out.error = Some(e.to_string()),
    }
    out
}

// ---------------------------------------------------------------------------
// Port scanner — bounded TCP connect scan (verified pattern: see
// docs/TAURI_MIGRATION.md for a real 1.1.1.1:443 connect-scan run).
// ---------------------------------------------------------------------------

#[derive(Serialize, Default)]
pub struct PortScanResult {
    pub host: String,
    pub scanned: (u16, u16),
    pub open: Vec<u16>,
    pub error: Option<String>,
}

pub async fn port_scan(host: &str, start: u16, end: u16) -> PortScanResult {
    let host = host.trim();
    if !is_hostish(host) {
        return PortScanResult { host: host.to_string(), error: Some("Invalid host".into()), ..Default::default() };
    }
    let (mut s, mut e) = (start.max(1), end.max(1));
    if e < s {
        std::mem::swap(&mut s, &mut e);
    }
    if (e as u32 - s as u32) > 2048 {
        e = s.saturating_add(2048); // same cap as the old Electron implementation
    }

    let host_owned = host.to_string();
    let mut set = tokio::task::JoinSet::new();
    let mut open = Vec::new();
    const CONCURRENCY: usize = 200;
    let mut ports: Vec<u16> = (s..=e).collect();

    while !ports.is_empty() || !set.is_empty() {
        while set.len() < CONCURRENCY {
            let Some(port) = ports.pop() else { break };
            let h = host_owned.clone();
            set.spawn(async move {
                let target = format!("{h}:{port}");
                let ok = match timeout(Duration::from_millis(800), TcpStream::connect(&target)).await {
                    Ok(Ok(_)) => true,
                    _ => false,
                };
                (port, ok)
            });
        }
        if let Some(res) = set.join_next().await {
            if let Ok((port, true)) = res {
                open.push(port);
            }
        } else {
            break;
        }
    }
    open.sort_unstable();
    PortScanResult { host: host.to_string(), scanned: (s, e), open, error: None }
}

// ---------------------------------------------------------------------------
// Ping — spawns the OS `ping`, streaming output. Structurally a direct port
// of the Electron main.js implementation (same flag mapping per-OS); the
// event-streaming mechanism (Tauri events vs. Electron IPC) is the only real
// difference. See commands.rs for how this hooks into #[tauri::command].
// ---------------------------------------------------------------------------

pub struct PingOpts {
    pub count: u32,
    pub size: Option<u32>,
    pub timeout_ms: Option<u32>,
    pub ttl: Option<u32>,
    pub interval: Option<f64>,
    pub family: Option<String>, // "v4" | "v6" | None
    pub continuous: bool,
}

pub struct PingRegistry {
    procs: Mutex<HashMap<String, Child>>,
}

impl PingRegistry {
    pub fn new() -> Arc<Self> {
        Arc::new(Self { procs: Mutex::new(HashMap::new()) })
    }
}

fn clampi(v: i64, lo: i64, hi: i64) -> i64 {
    v.max(lo).min(hi)
}

/// Whether `stdbuf` exists on PATH — checked once, lazily, and cached for the
/// process lifetime (a std::process::Command::output() call is blocking, so
/// running it on every `start()` call from an async fn would stall a tokio
/// worker thread each time; OnceLock means that cost is paid at most once).
fn have_stdbuf() -> bool {
    static CACHE: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *CACHE.get_or_init(|| {
        std::process::Command::new("stdbuf").arg("--version").output().is_ok()
    })
}

/// Builds the OS-appropriate ping argv. Mirrors electron/main.js's mapping
/// exactly (Windows -n/-l/-i/-w vs. Unix -c/-s/-t/-i/-W), never via a shell
/// string — argv only, so there's nothing to inject regardless of host input.
fn build_ping_args(host: &str, opts: &PingOpts) -> Vec<String> {
    let is_win = cfg!(target_os = "windows");
    let is_mac = cfg!(target_os = "macos");
    let mut args = Vec::new();

    if let Some(fam) = &opts.family {
        if fam == "v4" {
            args.push("-4".into());
        } else if fam == "v6" {
            args.push("-6".into());
        }
    }

    let count = clampi(opts.count as i64, 1, 10000) as u32;
    if is_win {
        if opts.continuous {
            args.push("-t".into());
        } else {
            args.push("-n".into());
            args.push(count.to_string());
        }
        if let Some(sz) = opts.size {
            args.push("-l".into());
            args.push(clampi(sz as i64, 0, 65500).to_string());
        }
        if let Some(ttl) = opts.ttl {
            args.push("-i".into());
            args.push(clampi(ttl as i64, 1, 255).to_string());
        }
        if let Some(t) = opts.timeout_ms {
            args.push("-w".into());
            args.push(clampi(t as i64, 100, 60000).to_string());
        }
    } else {
        if !opts.continuous {
            args.push("-c".into());
            args.push(count.to_string());
        }
        if let Some(sz) = opts.size {
            args.push("-s".into());
            args.push(clampi(sz as i64, 0, 65500).to_string());
        }
        if let Some(ttl) = opts.ttl {
            args.push("-t".into());
            args.push(clampi(ttl as i64, 1, 255).to_string());
        }
        if let Some(iv) = opts.interval {
            args.push("-i".into());
            args.push(format!("{:.1}", iv.clamp(0.2, 60.0)));
        }
        if let Some(t) = opts.timeout_ms {
            let secs = ((t as f64) / 1000.0).ceil() as i64;
            args.push(if is_mac { "-t" } else { "-W" }.into());
            args.push(secs.to_string());
        }
    }
    args.push(host.to_string());
    args
}

/// Spawns `ping` and streams stdout/stderr lines to `on_line`, calling
/// `on_done` when the process exits. `id` is generated by the CALLER (not
/// here) specifically so it can be cloned into `on_line`/`on_done` before
/// this function is invoked — those closures need the id at the moment
/// they're constructed, not after spawning, since they run on later ticks of
/// the async runtime.
pub async fn start<F, D>(
    registry: &Arc<PingRegistry>,
    id: String,
    host: &str,
    opts: PingOpts,
    mut on_line: F,
    on_done: D,
) -> Result<(), String>
where
    F: FnMut(String, &'static str) + Send + 'static,
    D: FnOnce(Option<i32>) + Send + 'static,
{
    let host = host.trim();
    if !is_hostish(host) {
        return Err("Invalid host".into());
    }
    let args = build_ping_args(host, &opts);
    // On Linux, `ping`'s stdout is fully block-buffered (not line-buffered)
    // once it's a pipe rather than a TTY — discovered by testing this exact
    // spawn+stream path directly: no output arrived until the process ended,
    // which would silently delay "live" streaming until ping exits. `stdbuf
    // -oL` forces line buffering; it's a coreutils tool present on essentially
    // every Linux distro, a no-op to skip if missing, and irrelevant on
    // Windows/macOS (their ping binaries don't exhibit this, so it's Linux-only).
    let mut cmd = if cfg!(target_os = "linux") && have_stdbuf() {
        let mut c = Command::new("stdbuf");
        c.arg("-oL").arg("ping").args(&args);
        c
    } else {
        let mut c = Command::new("ping");
        c.args(&args);
        c
    };
    cmd.stdout(Stdio::piped()).stderr(Stdio::piped());
    #[cfg(windows)]
    {
        const CREATE_NO_WINDOW: u32 = 0x0800_0000;
        cmd.creation_flags(CREATE_NO_WINDOW);
    }
    let mut child = cmd.spawn().map_err(|e| e.to_string())?;

    let stdout = child.stdout.take();
    let stderr = child.stderr.take();
    registry.procs.lock().await.insert(id.clone(), child);

    let registry2 = registry.clone();
    let id2 = id.clone();
    tokio::spawn(async move {
        if let Some(out) = stdout {
            let mut lines = BufReader::new(out).lines();
            while let Ok(Some(line)) = lines.next_line().await {
                on_line(line, "out");
            }
        }
        if let Some(err) = stderr {
            let mut lines = BufReader::new(err).lines();
            while let Ok(Some(line)) = lines.next_line().await {
                on_line(line, "err");
            }
        }
        let code = {
            let mut procs = registry2.procs.lock().await;
            if let Some(mut child) = procs.remove(&id2) {
                child.wait().await.ok().and_then(|s| s.code())
            } else {
                None
            }
        };
        on_done(code);
    });

    Ok(())
}

pub async fn stop(registry: &Arc<PingRegistry>, id: &str) {
    if let Some(mut child) = registry.procs.lock().await.remove(id) {
        let _ = child.kill().await;
    }
}

/// A short, process-unique-enough id for tagging one ping run — not a real
/// UUID (no external crate needed for something this low-stakes), just
/// nanosecond time as hex. Public so callers can generate the id BEFORE
/// calling `start`, letting them capture it into their own callbacks.
pub fn new_id() -> String {
    use std::time::{SystemTime, UNIX_EPOCH};
    let n = SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_nanos();
    format!("{:x}", n)
}
