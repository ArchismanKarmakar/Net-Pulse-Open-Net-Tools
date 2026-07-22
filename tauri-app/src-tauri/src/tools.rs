//! Native network tools — Rust replacements for the old Electron main.js
//! implementations (which used Node's child_process/dns/net). Forward/reverse
//! DNS and the port scanner use verified-working patterns (hickory-resolver,
//! tokio TcpStream connect-scan — see docs/TAURI_MIGRATION.md for the
//! standalone test that exercised these exact primitives against real
//! network calls). Ping now lives in the C++ core (PingRun, ping_run.hpp) —
//! it used to spawn and text-parse the OS `ping` binary here, but that was
//! the one tool in this app that didn't share the main engine's pooled-
//! socket/scalable architecture; see ffi.rs's ping_start/ping_stop/ping_poll
//! and commands.rs's ping_start/ping_stop for where it lives now.

use std::net::IpAddr;
use std::time::Duration;

use hickory_resolver::Resolver;
use hickory_resolver::config::ResolverConfig;
use hickory_resolver::net::runtime::TokioRuntimeProvider;
use serde::Serialize;
use tokio::net::TcpStream;
use tokio::time::timeout;

// A host/IP must match this before it's used in ANY tool below — same
// allowlist-style validation the Electron version used (isHostish in
// electron/main.js), enforced again here since this is a different process
// boundary now. Rejects anything that isn't letters/digits/./:/-/[]., which
// rules out shell metacharacters — defense in depth even though nothing
// left in this file spawns a subprocess anymore.
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
    pub mx: Vec<String>,
    pub txt: Vec<String>,
    pub ns: Vec<String>,
    pub error: Option<String>,
}

// ResolverConfig::default() hardcodes Google's public DNS (8.8.8.8/8.8.4.4) —
// it does NOT read the system's actual configured resolver. On a network
// that only permits the system-configured DNS server (the same class of
// restrictive firewall/router setup that was found blocking ICMP on some
// test machines), direct queries to those hardcoded external servers can be
// silently blocked while the OS's own DNS (used by nslookup/browsers/etc.)
// works fine. Resolver::builder_tokio() reads the real OS DNS config
// directly; only fall back to the hardcoded default if that genuinely fails
// (e.g. no resolv.conf-equivalent readable at all), so a lookup is still
// attempted rather than failing outright.
macro_rules! build_system_resolver {
    () => {
        match Resolver::builder_tokio() {
            Ok(b) => b.build(),
            Err(_) => Resolver::builder_with_config(ResolverConfig::default(), TokioRuntimeProvider::default()).build(),
        }
    };
}

pub async fn dns_lookup(name: &str) -> DnsResult {
    use hickory_resolver::proto::rr::{RData, RecordType};

    let name = name.trim();
    if !is_hostish(name) {
        return DnsResult { error: Some("Invalid host name".into()), ..Default::default() };
    }
    let Ok(resolver) = build_system_resolver!() else {
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

    // MX/TXT/NS are best-effort: a missing record type shouldn't fail the
    // whole lookup, so each is queried independently and errors are dropped.
    if let Ok(r) = resolver.lookup(format!("{name}."), RecordType::MX).await {
        out.mx = r
            .answers()
            .iter()
            .filter_map(|rec| match &rec.data {
                RData::MX(mx) => Some(format!("{} {}", mx.preference, mx.exchange)),
                _ => None,
            })
            .collect();
    }
    if let Ok(r) = resolver.lookup(format!("{name}."), RecordType::TXT).await {
        out.txt = r
            .answers()
            .iter()
            .filter_map(|rec| match &rec.data {
                RData::TXT(txt) => Some(
                    txt.txt_data
                        .iter()
                        .map(|b| String::from_utf8_lossy(b).into_owned())
                        .collect::<Vec<_>>()
                        .join(""),
                ),
                _ => None,
            })
            .collect();
    }
    if let Ok(r) = resolver.lookup(format!("{name}."), RecordType::NS).await {
        out.ns = r
            .answers()
            .iter()
            .filter_map(|rec| match &rec.data {
                RData::NS(ns) => Some(ns.0.to_string()),
                _ => None,
            })
            .collect();
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
    let Ok(resolver) = build_system_resolver!() else {
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