// Guaranteed, link-independent Winsock initialization.
//
// WHY THIS FILE EXISTS: on Windows, WSAStartup() must run before ANY other
// Winsock call (getaddrinfo, socket, select, ...). It used to live only as a
// namespace-scope static object inside transport.cpp. That is NOT reliable:
// when netpulse_core is linked as a static library, the linker only pulls in
// .obj files needed to resolve a symbol some other .obj references. A binary
// that never calls into transport.cpp (e.g. a unit test that only exercises
// Session::resolve(), which calls getaddrinfo() directly) will silently never
// link transport.obj — so WSAStartup() never runs — and the first Winsock
// call crashes. This bit us in exactly that way: netpulse_tests.exe crashed
// instantly under the debugger because it calls getaddrinfo() via
// Session::resolve() but never references Prober/list_interfaces.
//
// Fix: make initialization a function, called explicitly at the top of every
// place that touches a socket API (Session::resolve, Prober::Prober). A
// function-local static runs exactly once, on first call, regardless of which
// .obj files the linker decided to include.
#pragma once

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <timeapi.h>
#  pragma comment(lib, "Ws2_32.lib")
#  pragma comment(lib, "Winmm.lib")
// BUG FIX: process_is_elevated()/capture_driver_present() below use HANDLE,
// TOKEN_ELEVATION, OpenProcessToken, LoadLibraryExW, and several other core
// Win32 APIs, but until this line NOTHING in this file ever included the
// header those live in (<windows.h>). This "worked" only by accident,
// whenever some OTHER header — pulled in by whatever .cpp happened to
// include this file — already dragged in <windows.h> first. netpulse_ffi.cpp
// is exactly the file that does NOT reliably get that accident: a real build
// hit a catastrophic winsock.h-vs-winsock2.h conflict there (see build.rs's
// own matching comment on WIN32_LEAN_AND_MEAN for the full mechanism). Two
// independent fixes now cover this: build.rs defines WIN32_LEAN_AND_MEAN
// process-wide before any of our files even start compiling, AND this file
// includes <windows.h> itself, right here, immediately after winsock2.h/
// ws2tcpip.h — which is what makes <windows.h>'s own internal guard skip
// re-including the legacy <winsock.h> regardless of the WIN32_LEAN_AND_MEAN
// compiler flag's presence, AND makes this header genuinely self-sufficient
// rather than dependent on luck from whatever else a caller happens to
// include. `#ifndef WIN32_LEAN_AND_MEAN` mirrors the NOMINMAX pattern just
// above so this stays correct even if only one of the two fixes is present.
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

#ifndef _WIN32
#include <unistd.h>
#endif

#include <atomic>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <string>
// For debug_log()'s timestamp prefix — explicit, not assumed transitively
// available from some other header, the exact class of mistake already
// found and fixed twice this project (mstcpip.h, netinet/tcp.h).
#include <chrono>
#include <ctime>

namespace netpulse {

inline void ensure_winsock_ready() {
#ifdef _WIN32
    struct WsaInit {
        WsaInit() {
            WSADATA d;
            WSAStartup(MAKEWORD(2, 2), &d);
            timeBeginPeriod(1); // 1ms timer resolution for accurate RTTs
        }
        ~WsaInit() {
            timeEndPeriod(1);
            WSACleanup();
        }
    };
    static WsaInit g_wsa; // constructed on first call, from whichever TU calls first
#endif
}


// Whether this process is running with elevated privileges (Administrator on
// Windows, root/euid 0 elsewhere). Raw-socket behaviour differs sharply with
// this, and UDP-style traceroute in particular is documented by other tools
// (PingPlotter's manual, for one) as REQUIRING administrative rights on
// Windows — so when UDP mode receives nothing, "are we actually elevated?" is
// the first question worth answering with a fact instead of a guess.
inline bool process_is_elevated() {
#ifdef _WIN32
    HANDLE tok = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION el{};
    DWORD sz = sizeof(el);
    bool ok = ::GetTokenInformation(tok, TokenElevation, &el, sizeof(el), &sz) != 0;
    ::CloseHandle(tok);
    return ok && el.TokenIsElevated != 0;
#else
    return ::geteuid() == 0;
#endif
}


// Whether a packet-capture driver (Npcap / WinPcap) is installed. Only
// meaningful on Windows; other platforms receive the ICMP errors that hop
// discovery needs directly on a raw socket, so nothing extra is required and
// this reports true. Detected by asking the loader for the capture DLLs
// rather than by poking the registry or the service list: if wpcap.dll can be
// loaded, a capture driver is genuinely usable by this process right now,
// which is the actual question — a registry key can be left behind by an
// uninstall, and a service entry can exist while the driver fails to start.
inline bool capture_driver_present() {
#ifdef _WIN32
    // Npcap installs to System32\\Npcap by default and is NOT on the default
    // search path, so a bare LoadLibrary("wpcap.dll") can miss a perfectly
    // good install. Try the explicit Npcap directory first, then the plain
    // name (covers WinPcap and Npcap installed in WinPcap-compatible mode).
    HMODULE h = ::LoadLibraryExW(L"wpcap.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!h) {
        wchar_t sysdir[MAX_PATH]{};
        if (::GetSystemDirectoryW(sysdir, MAX_PATH)) {
            std::wstring p2 = std::wstring(sysdir) + L"\\Npcap\\wpcap.dll";
            h = ::LoadLibraryW(p2.c_str());
        }
    }
    if (!h) h = ::LoadLibraryW(L"wpcap.dll");
    if (h) { ::FreeLibrary(h); return true; }
    return false;
#else
    return true; // not applicable — see this function's doc comment
#endif
}


// Diagnostic logging sink.
//
// WHY A FILE AND NOT JUST stderr: the diagnostics were originally gated on
// NETPULSE_DEBUG=1 and written to stderr, which is unusable in the exact
// situation they matter most. Elevating on Windows starts a BRAND NEW
// process via ShellExecute/UAC — it does not inherit the environment of the
// terminal you set the variable in, and it gets its own console window that
// closes with it. So "run as admin AND set NETPULSE_DEBUG" is, in practice,
// not something a user can straightforwardly do at all. Writing to a file in
// the app data directory sidesteps the whole problem: no environment
// variable, no console, works identically however the app was launched, and
// the file can simply be sent along with a bug report.
//
// Still opt-in (nothing is written unless enabled) so normal runs pay no I/O
// cost and nothing accumulates on disk unasked. NETPULSE_DEBUG=1 continues to
// work and additionally enables it, so existing habits are unaffected.
inline std::string& debug_log_path() { static std::string p; return p; }
inline std::atomic<bool>& debug_log_enabled() { static std::atomic<bool> b{false}; return b; }
inline std::mutex& debug_log_mutex() { static std::mutex m; return m; }

inline bool debug_logging_on() {
    static const bool env_on = !!std::getenv("NETPULSE_DEBUG");
    return env_on || debug_log_enabled().load();
}

// BUG FIX: this used to fopen(path, "a") + fputs + fclose on EVERY SINGLE
// CALL — including from dispatch_incoming() (session.cpp), the shared RX
// dispatcher's hot path that runs for every incoming ICMP packet across
// EVERY active session. With several targets running for hours (reported
// live: multiple protocols to two destinations, left open "for long"), that
// is tens of thousands of raw file-open syscalls. On Windows in particular,
// each fopen/fclose is a real-time-AV scan hook opportunity — genuinely
// plausible as the actual cause of a reported crash/hang after long uptime,
// independent of whatever the log file was originally opened to diagnose.
// There was also no size cap at all: left enabled indefinitely, the file
// grows without bound.
//
// Fixed by keeping ONE persistent handle open for the process's lifetime
// (opened lazily on first log line, flushed after each write so a crash
// doesn't lose the tail of the log — the whole point of this feature — but
// never closed/reopened per call), and capping total output at kMaxLogBytes
// so a long session degrades to "logging stops" rather than "disk fills
// up". A stopped-due-to-cap state is logged once, not silently.
constexpr long long kMaxDebugLogBytes = 20LL * 1024 * 1024; // 20MB
struct DebugLogFile {
    std::FILE* f = nullptr;
    long long written = 0;
    bool cap_notice_shown = false;
    ~DebugLogFile() { if (f) std::fclose(f); }
};
inline DebugLogFile& debug_log_file() { static DebugLogFile d; return d; }

// Closes the persistent handle (if open) and resets the size-cap/notice
// state. Called when logging is turned OFF, so the file isn't held open
// indefinitely afterward — a stale open handle would block the user from
// moving, deleting, or attaching the log file (the entire point of turning
// logging on in the first place) even after they've explicitly stopped it.
// Safe to call when already closed. The handle reopens lazily and cleanly
// the next time logging is turned back on.
inline void debug_log_reset() {
    std::lock_guard<std::mutex> lk(debug_log_mutex());
    DebugLogFile& d = debug_log_file();
    if (d.f) { std::fclose(d.f); d.f = nullptr; }
    d.written = 0;
    d.cap_notice_shown = false;
}


// Appends one already-formatted line to both stderr (useful when a console
// IS attached) and the log file (the reliable path). Cheap no-op when off.
//
// BUG FIX: a real log file showed messages like "SIO_RCVALL enabled ΓÇö raw
// socket..." — that garbled "ΓÇö" is the exact, well-known signature of a
// UTF-8-encoded em-dash (—, bytes E2 80 94) being DECODED as CP1252/ANSI,
// which is what many Windows text viewers (Notepad among them) fall back to
// for a file with no BOM and no other UTF-8 signal. The bytes actually
// written were always correct UTF-8; nothing here was ever corrupting the
// dash itself. Fixed by writing a UTF-8 BOM (EF BB BF) as the very first
// bytes of a newly-created log file — every mainstream text editor treats a
// leading BOM as an unambiguous "this file is UTF-8" signal and decodes the
// rest of the file correctly, including every non-ASCII character already
// used throughout this codebase's log messages, not just this one line.
//
// BUG FIX/FEATURE: also now prefixes every line with a wall-clock
// timestamp. Every debug_log() call site benefits automatically from doing
// this here once, rather than needing each of the 15+ call sites across the
// codebase to remember to include one themselves.
inline void debug_log(const std::string& line) {
    if (!debug_logging_on()) return;
    // %H:%M:%S with millisecond precision — enough to see genuine ordering
    // and timing between rapid-fire lines (e.g. a "sent" immediately
    // followed by a "routed" for the same probe), without the visual noise
    // of a full date on every single line.
    char ts[32];
    {
        using namespace std::chrono;
        auto now = system_clock::now();
        auto now_t = system_clock::to_time_t(now);
        auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
        std::tm tm_buf{};
#ifdef _WIN32
        localtime_s(&tm_buf, &now_t);
#else
        localtime_r(&now_t, &tm_buf);
#endif
        std::snprintf(ts, sizeof(ts), "[%02d:%02d:%02d.%03d] ",
                      tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, static_cast<int>(ms.count()));
    }
    const std::string stamped = std::string(ts) + line;
    std::fputs(stamped.c_str(), stderr);
    const std::string& path = debug_log_path();
    if (path.empty()) return;
    std::lock_guard<std::mutex> lk(debug_log_mutex());
    DebugLogFile& d = debug_log_file();
    if (d.written >= kMaxDebugLogBytes) {
        if (!d.cap_notice_shown) {
            d.cap_notice_shown = true;
            const char* notice = "[netpulse] diagnostic log reached its size cap (20MB) -- further lines are dropped. Restart logging (Tools menu) to continue.\n";
            std::fputs(notice, stderr);
            if (d.f) { std::fputs(notice, d.f); std::fflush(d.f); }
        }
        return;
    }
    if (!d.f) {
        d.f = std::fopen(path.c_str(), "a");
        if (d.f) {
            // Only meaningful right after creating/opening for the first
            // time in this process — writing it on every open would corrupt
            // the file with a BOM in the middle every time logging is
            // toggled off and back on. ftell() distinguishes "brand new/
            // empty file" from "appending to one that already has content
            // (and, from an earlier session, may already have its own BOM)".
            if (std::ftell(d.f) == 0) {
                static const unsigned char utf8_bom[] = {0xEF, 0xBB, 0xBF};
                std::fwrite(utf8_bom, 1, sizeof(utf8_bom), d.f);
            }
        }
    }
    if (d.f) {
        std::fputs(stamped.c_str(), d.f);
        std::fflush(d.f); // not fclose — see this function's own doc comment for why keeping the handle open matters
        d.written += static_cast<long long>(line.size());
    }
}

} // namespace netpulse
