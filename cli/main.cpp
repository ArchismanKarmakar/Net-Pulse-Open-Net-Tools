// netpulse-cli (npulse) — terminal front-end over the exact same
// netpulse_core engine the Tauri desktop app uses (Session, PingRun,
// list_interfaces). No probing logic lives here — every subcommand is
// argument parsing plus terminal rendering around an already-existing,
// already-tested engine call, same division of responsibility as
// tauri-app/src-tauri/native/netpulse_ffi.cpp has for the Rust bridge.
// See cli/CLI.md (repo root) for the full command reference this is meant
// to match, and ARCHITECTURE.md's CLI section for how this fits into the
// build/release pipeline (a separate executable target, bundled as a Tauri
// externalBin sidecar into each OS's installer — see
// .github/workflows/tauri-release.yml's "Build CLI sidecar" step).
//
// Multiple familiar entry points, one binary: argv[0] is inspected first
// (so a copy/symlink of this binary named `ping`, `tracert`, `mtr`, or
// `ifconfig`/`ipconfig` behaves like that command directly, no subcommand
// needed), falling back to `npulse SUBCOMMAND ...` when invoked under its
// own name. See CLI.md's "Standard commands" section for the full mapping
// and — importantly — why the release packaging does NOT install `ping`/
// `tracert` as literal PATH-shadowing names by default (it would silently
// replace the OS's own tools for every other program on the system that
// shells out to them): only prefixed aliases (`npulse-ping`, etc.) and the
// `npulse <subcommand>` form are installed to PATH standardly; a person can
// still manually name/symlink a copy `ping` themselves if they specifically
// want that, and this binary will honor it correctly either way.
#include "netpulse/session.hpp"
#include "netpulse/transport.hpp"
#include "netpulse/ping_run.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h> // GetStdHandle/SetConsoleMode — see term_init()'s doc comment
#  include <io.h>       // _isatty/_fileno
#else
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <sys/socket.h>
#  include <sys/wait.h> // waitpid/WIFEXITED/WEXITSTATUS — launch_shell_console()'s POSIX branch
#  include <unistd.h>
#  include <fcntl.h>
#  include <cerrno>
#  ifdef __APPLE__
#    include <mach-o/dyld.h> // _NSGetExecutablePath — own_executable_dir()'s macOS branch
#  endif
#  include <sys/stat.h> // chmod — setup_alias_dir()'s POSIX copy fallback
#  include <termios.h> // tcflush — term_restore()'s POSIX branch
#endif

using namespace netpulse;

// Shared plumbing

static std::atomic<bool> g_stop{false};
static void on_sigint(int) { g_stop = true; }

static std::string basename_of(const std::string& path) {
    size_t p = path.find_last_of("/\\");
    std::string b = (p == std::string::npos) ? path : path.substr(p + 1);
    // Strip a Windows .exe suffix so argv[0]-based dispatch works
    // identically whether this binary is named `mtr` or `mtr.exe`.
    if (b.size() > 4 && b.compare(b.size() - 4, 4, ".exe") == 0) b.resize(b.size() - 4);
    return b;
}


// ---------------------------------------------------------------------------
// Terminal rendering: ANSI enablement, in-place (flicker-free) redraw, and
// color.
//
// BUG FIX, reported against a real Windows run: the live views (tracert-mtr,
// ifconfig -w) were scrolling a fresh copy of the whole table every redraw
// instead of updating in place — CLI.md previously claimed "no
// SetConsoleMode/ENABLE_VIRTUAL_TERMINAL_PROCESSING call was needed" on the
// theory that modern Windows Terminal/PowerShell 7 enable VT processing by
// default. That claim was wrong (or at least not universally true — the
// actual report was against classic Windows PowerShell 5.1): a Windows
// console only interprets ANSI/VT escape sequences from a process's own
// output once THAT PROCESS explicitly opts in via
// SetConsoleMode(..., ENABLE_VIRTUAL_TERMINAL_PROCESSING) on its own stdout
// handle — this is Microsoft's own documented mechanism, not automatic.
// Without it, the \033[2J\033[H bytes this file was already emitting are
// just inert — no clear, no cursor move — so each redraw's `printf` calls
// simply appended below the previous frame's, which is exactly the
// scrolling behavior reported. term_init() below performs that opt-in;
// g_use_ansi records whether it (and therefore every ANSI feature below —
// redraw and color both) is actually safe to use, checked once at startup.
static bool g_use_ansi = false;
#ifdef _WIN32
// Saved so term_restore() (registered via atexit(), below) can put the
// console back exactly as this process found it — see term_init()'s and
// term_restore()'s own doc comments for why this matters.
static DWORD g_original_console_mode = 0;
static bool g_console_mode_saved = false;
#endif

// BUG FIX, reported live: running tracert-mtr directly from an existing
// PowerShell session (as opposed to through the npulse console, which
// hosts plain cmd.exe — reported as unaffected) left the PROMPT itself
// corrupted afterward — fragments of unrelated text ("ee", "l") appearing
// to type themselves, spurious "More?" continuation prompts, arrow-key
// presses inserting garbage instead of navigating. This class of symptom
// is a well-known Windows-console pitfall, not specific to this program:
// a child process that changes console state (ENABLE_VIRTUAL_TERMINAL_
// PROCESSING, in this case) without restoring it before exiting can leave
// the PARENT shell's console in a state it doesn't expect, because the
// console mode is a property of the shared console object both processes
// are attached to, not a private copy — and any escape-sequence response
// bytes still sitting unread in the console's input buffer when this
// process exits (terminals can and do send responses to certain queries)
// get picked up by whatever reads input NEXT, which is exactly the
// parent shell's own line editor (PSReadLine, in PowerShell's case).
// term_restore(), registered once via atexit() so it runs on every exit
// path without needing a matching call at each `return` in this file,
// addresses both: restores the original console mode, and discards any
// input the console received but this process never read.
static void term_restore() {
#ifdef _WIN32
    if (g_console_mode_saved) {
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        if (hOut != INVALID_HANDLE_VALUE && hOut != nullptr) SetConsoleMode(hOut, g_original_console_mode);
    }
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    if (hIn != INVALID_HANDLE_VALUE && hIn != nullptr) FlushConsoleInputBuffer(hIn);
#else
    // Same reasoning as the Windows branch's second half: discard any
    // input the terminal sent that this process never read, so it can't
    // resurface as garbage in the next program (the shell) to read stdin.
    // POSIX terminals don't have an equivalent "restore console mode"
    // concern the way Windows does here — this file never puts the
    // terminal into a raw/non-canonical mode (termios) on POSIX at all,
    // only the ANSI *output* sequences that a normal canonical-mode
    // terminal already interprets on its own.
    if (isatty(fileno(stdin))) tcflush(fileno(stdin), TCIFLUSH);
#endif
    if (g_use_ansi) { std::printf("\033[0m"); std::fflush(stdout); } // reset any color left applied
}

static void term_init() {
    std::atexit(term_restore); // see term_restore()'s doc comment for why this must run on every exit path
#ifdef _WIN32
    // Defense in depth against the SAME class of bug reported live (raw
    // UTF-8 bytes misread as several separate legacy-codepage characters —
    // "Γÿà" instead of "★"): the offending characters this file used to
    // print have been replaced outright with plain ASCII (see truncate()'s
    // and render_hop_table()'s doc comments), which is the real, guaranteed
    // fix — this call is a backstop for anything else that could still
    // legitimately contain non-ASCII bytes, such as a reverse-DNS hostname
    // for an internationalized domain, so THAT renders correctly too rather
    // than relying on every possible source of text staying ASCII forever.
    SetConsoleOutputCP(CP_UTF8);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (hOut != INVALID_HANDLE_VALUE && hOut != nullptr && GetConsoleMode(hOut, &mode)) {
        g_original_console_mode = mode; // see term_restore()'s doc comment
        g_console_mode_saved = true;
        // Only claim ANSI is usable if the opt-in actually succeeded AND
        // stdout is a real console (not redirected to a file/pipe — writing
        // escape codes into a report or piped `-r`/`--json` output would
        // corrupt it for whatever reads it next).
        g_use_ansi = SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) && _isatty(_fileno(stdout));
    }
#else
    g_use_ansi = isatty(fileno(stdout));
#endif
}

// BUG FIX, reported live: after a live view (tracert-mtr's continuous
// table) was interrupted with Ctrl-C, the PowerShell session it ran in
// became corrupted — keystrokes appearing as garbage ("ee", "l"), stray
// "More?" continuation prompts, commands failing with bogus
// "not recognized" errors. Root cause: this file only ever used cursor-
// repositioning WITHIN the terminal's normal scrollback (\033[H to move
// the cursor, \033[J to erase) — never the ALTERNATE SCREEN BUFFER real
// full-screen terminal programs (`vim`, `htop`, `less`, `mtr` itself) use
// for exactly this kind of continuous redraw. Repeatedly repositioning the
// cursor inside the SAME buffer a line-editor (PowerShell's PSReadLine,
// or any shell's own line editing) is also tracking its own prompt
// position within desynchronizes that line editor's internal bookkeeping
// from where the cursor actually is — and once desynced, ordinary typed
// input can render or get interpreted incorrectly, exactly matching what
// was reported. This is a standard, well-known failure mode for exactly
// this class of bug, with a standard, well-known fix: enter the alternate
// screen buffer (`\033[?1049h`) ONCE before a live-redraw session starts,
// do all per-frame cursor repositioning entirely inside that separate
// buffer (which no line editor is tracking, because it isn't the buffer
// the shell's own prompt lives in), and leave it (`\033[?1049l`) ONCE when
// the session ends — which atomically restores the terminal to exactly
// the scrollback and cursor position it had before, the same guarantee
// `vim`/`less`/`htop` already give when you quit them. Critically, this
// must happen even when the session ends via Ctrl-C, not just a clean
// exit — enforced here by ALWAYS calling leave_live_view() before
// returning from cmd_tracert_mtr()/cmd_ifconfig()'s watch loop, on every
// exit path, not just the normal one.
static bool g_alt_screen_active = false;
static void enter_live_view() {
    if (g_use_ansi && !g_alt_screen_active) {
        std::printf("\033[?1049h");
        std::fflush(stdout);
        g_alt_screen_active = true;
    }
}
static void leave_live_view() {
    if (g_alt_screen_active) {
        std::printf("\033[?1049l");
        std::fflush(stdout);
        g_alt_screen_active = false;
    }
}

// Cursor home WITHOUT a full-screen clear (unlike this file's previous
// \033[2J\033[H every frame) — full-clear-then-redraw is what produces
// visible flicker on every refresh; moving the cursor back to the top and
// overwriting in place, the way real `mtr`/`htop`/SolarWinds Traceroute NG
// do, does not. Pair with print_line() (clears any leftover tail from a
// longer previous line) and end_frame() (clears any leftover trailing
// lines from a previous frame that had more rows than this one).
//
// g_frame_live tracks whether the CURRENT render is a live, redrawn-in-
// place one (only ever true between a begin_frame() and its matching
// end_frame()) — separate from g_use_ansi (whether the terminal can do
// ANSI at all). print_line()'s per-line \033[K only exists to erase
// leftovers from a PREVIOUS redraw of the same on-screen position; a
// one-shot `--report`/`--json` render (never wrapped in begin_frame(),
// even when run attached to a real terminal rather than piped) has no
// such leftover to erase and must stay plain text with no redraw-control
// codes in it at all, matching CLI.md's "no ANSI redraw, safe to pipe"
// promise regardless of whether output happens to be a real terminal in
// that particular run. Color (col_*() below) is unaffected by this — it's
// gated on g_use_ansi alone and already fully suppressed automatically
// whenever output isn't a real terminal (term_init()'s own isatty check).
static bool g_frame_live = false;
static void begin_frame() { if (g_use_ansi) { std::printf("\033[H"); g_frame_live = true; } }
// Erase from the cursor to the end of the screen — call once, after the
// last line of a frame, so a frame with FEWER rows than the previous one
// (e.g. hop count changed) doesn't leave stale rows dangling below it.
static void end_frame() {
    if (g_frame_live) std::printf("\033[J");
    std::fflush(stdout);
    g_frame_live = false;
}
// Print one already-formatted line, clearing to end-of-line first (eats
// leftover characters if this line is shorter than what was there before)
// — the per-line counterpart to end_frame()'s per-frame version.
static void print_line(const std::string& s) {
    std::fputs(s.c_str(), stdout);
    if (g_frame_live) std::fputs("\033[K", stdout);
    std::fputc('\n', stdout);
}

static const char* col_reset()  { return g_use_ansi ? "\033[0m" : ""; }
static const char* col_bold()   { return g_use_ansi ? "\033[1m" : ""; }
static const char* col_dim()    { return g_use_ansi ? "\033[2m" : ""; }
static const char* col_red()    { return g_use_ansi ? "\033[31m" : ""; }
static const char* col_yellow() { return g_use_ansi ? "\033[33m" : ""; }
static const char* col_green()  { return g_use_ansi ? "\033[32m" : ""; }
static const char* col_cyan()   { return g_use_ansi ? "\033[36m" : ""; }
// Loss-percentage color, matching the GUI's own red/yellow/green severity
// bands for the same field (App.jsx's loss-coloring thresholds).
static const char* loss_color(double loss_pct) {
    if (loss_pct >= 20.0) return col_red();
    if (loss_pct > 0.0) return col_yellow();
    return col_green();
}

// "VS Developer Command Prompt" console: `npulse`/`netpulse` with no
// arguments — typed bare into an already-open terminal, OR the file run
// directly (double-clicked, a Start Menu/desktop/file-manager shortcut) —
// spawns the user's own real shell, with npulse's own directory prepended
// to PATH for that session, and waits for it. This is genuinely just the
// user's shell the whole time (cmd.exe on Windows, $SHELL on Linux/macOS),
// not a custom REPL — the same relationship VS Developer Command Prompt
// has to plain cmd.exe (a shortcut that runs it with an environment
// script applied first). Asked for uniformly across every OS and both
// invocation styles — an earlier version of this only handled a freshly-
// allocated Windows console specifically (the double-click case, gated on
// GetConsoleProcessList()) and left an already-open shell's bare `npulse`
// showing help text instead; this replaces that with the same behavior
// either way, on every platform.
//
// Only ever triggered for the canonical `npulse`/`netpulse` invocation
// with literally zero arguments — see this section's call site in main():
// running under an ALIAS name (`ping`, `tracert`, `mtr`, `ifconfig`, ...)
// with no further arguments still shows that alias's own usage/behavior
// as before (someone who typed `ping` explicitly asked for `ping`, not
// npulse's general console), and `npulse help`/`npulse <anything>` is
// unaffected since it has an argument.
static std::string own_executable_path(const char* argv0) {
#ifdef _WIN32
    char buf[MAX_PATH] = {0};
    if (GetModuleFileNameA(nullptr, buf, MAX_PATH) > 0) return std::string(buf);
#elif defined(__linux__)
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) { buf[n] = '\0'; return std::string(buf); }
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) return std::string(buf);
#endif
    // Best-effort fallback for any platform/case the branches above didn't
    // resolve: if argv[0] itself contains a path separator (`./npulse`,
    // `/usr/local/bin/npulse`, `bin\npulse.exe`), trust it directly rather
    // than giving up — PATH already normally has the right directory
    // anyway when the binary was found via a plain PATH lookup (typing
    // `npulse` bare only works at all if that's already true), so this
    // fallback only matters for the explicit-path invocation style.
    return argv0 ? std::string(argv0) : std::string();
}

static std::string own_executable_dir(const char* argv0) {
    std::string p = own_executable_path(argv0);
    size_t slash = p.find_last_of("\\/");
    return slash != std::string::npos ? p.substr(0, slash) : std::string();
}

static void prepend_dir_to_path(const std::string& dir) {
    if (dir.empty()) return;
#ifdef _WIN32
    char existing[32768] = {0};
    DWORD n = GetEnvironmentVariableA("PATH", existing, sizeof(existing));
    std::string newPath = dir;
    if (n > 0) { newPath += ";"; newPath += existing; }
    SetEnvironmentVariableA("PATH", newPath.c_str());
#else
    const char* existing = std::getenv("PATH");
    std::string newPath = dir + (existing ? std::string(":") + existing : std::string());
    setenv("PATH", newPath.c_str(), 1);
#endif
}

// Every standard-command name this CLI dispatches on via argv[0] (main()'s
// alias handling) EXCEPT the canonical `npulse`/`netpulse` name itself —
// see setup_alias_dir()'s doc comment for why a real file under each of
// these names has to actually exist somewhere for that dispatch logic to
// ever run at all.
static const char* const kAliasNames[] = {
    "ping", "tracert", "traceroute", "mtr", "tracert-mtr", "ifconfig", "ipconfig", "nslookup"
};
static constexpr size_t kAliasCount = sizeof(kAliasNames) / sizeof(kAliasNames[0]);

// BUG FIX, reported live: inside the npulse console (launch_shell_console()
// below), typing `tracert-mtr 1.1.1.1` — exactly what the console's own
// banner invites — failed with "'tracert-mtr' is not recognized as an
// internal or external command". Root cause: argv[0]-based dispatch (see
// main()) only ever activates for a file that ACTUALLY EXISTS somewhere on
// PATH under that name — nothing anywhere previously created such a file;
// the dispatch logic existed, but nothing backed it. This creates one, for
// every name in kAliasNames, in a session-scoped temporary directory
// prepended to PATH ahead of everything else, so `ping`/`tracert`/`mtr`/
// `tracert-mtr`/`ifconfig`/etc. all genuinely work as typed, matching what
// the banner already promised.
//
// This is DIFFERENT from — and does not change — the standing decision
// documented in CLI.md not to install these names on PATH *permanently*
// (that would silently change the SYSTEM's `ping`/`tracert` for every
// other program on the machine). A session-scoped temp directory that
// only exists and only takes PATH precedence for the lifetime of THIS one
// console the user explicitly opened for exactly this purpose is a
// fundamentally different, much safer case — the same reasoning
// `conda activate`/`nvm use`/a Python virtualenv already rely on to
// temporarily shadow commands within one activated session without
// touching the system permanently.
//
// Hard links, not copies: zero extra disk space (the same file, one more
// directory entry pointing at it) and — unlike a symlink — need no
// elevated privilege or Developer Mode on Windows, and work identically on
// every filesystem this needs to support. Falls back to a plain file copy
// only if the hard link fails (most commonly: the temp directory and the
// executable are on different volumes, where a hard link can never work
// regardless of permissions) — still correct, just a few extra megabytes
// for the session's lifetime instead of zero.
//
// Returns the created directory (to prepend to PATH and clean up
// afterward via cleanup_alias_dir()), or empty if none could be created at
// all (in which case the console still works — `npulse <subcommand>` is
// always available regardless — just without the standalone alias names).
static std::string setup_alias_dir(const std::string& exe_path) {
    if (exe_path.empty()) return {};
#ifdef _WIN32
    char tempPath[MAX_PATH] = {0};
    if (GetTempPathA(MAX_PATH, tempPath) == 0) return {};
    std::string dir = std::string(tempPath) + "npulse-console-" + std::to_string(GetCurrentProcessId());
    if (!CreateDirectoryA(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return {};
    for (size_t i = 0; i < kAliasCount; ++i) {
        std::string linkPath = dir + "\\" + kAliasNames[i] + ".exe";
        if (!CreateHardLinkA(linkPath.c_str(), exe_path.c_str(), nullptr)) {
            CopyFileA(exe_path.c_str(), linkPath.c_str(), FALSE); // best-effort fallback — see doc comment above
        }
    }
    return dir;
#else
    std::string tmpl = "/tmp/npulse-console-XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    if (mkdtemp(buf.data()) == nullptr) return {};
    std::string dir(buf.data());
    for (size_t i = 0; i < kAliasCount; ++i) {
        std::string linkPath = dir + "/" + kAliasNames[i];
        if (link(exe_path.c_str(), linkPath.c_str()) != 0) {
            // Best-effort fallback — see doc comment above.
            std::ifstream src(exe_path, std::ios::binary);
            std::ofstream dst(linkPath, std::ios::binary);
            dst << src.rdbuf();
            src.close(); dst.close();
            chmod(linkPath.c_str(), 0755);
        }
    }
    return dir;
#endif
}

static void cleanup_alias_dir(const std::string& dir) {
    if (dir.empty()) return;
#ifdef _WIN32
    for (size_t i = 0; i < kAliasCount; ++i) DeleteFileA((dir + "\\" + kAliasNames[i] + ".exe").c_str());
    RemoveDirectoryA(dir.c_str());
#else
    for (size_t i = 0; i < kAliasCount; ++i) unlink((dir + "/" + kAliasNames[i]).c_str());
    rmdir(dir.c_str());
#endif
}

// Visible indication of being "inside" npulse — asked for explicitly,
// motivated by the exact confusion a corrupted prompt (see term_restore()'s
// doc comment above) can cause: without something persistent showing it,
// there's no way to tell at a glance whether you're in a plain shell or
// one npulse set up. The window/tab title is the most universal mechanism
// available on every terminal this ships for — `SetConsoleTitleA` on
// Windows, the widely-supported OSC 0 escape sequence
// (`\033]0;TITLE\007`) on every POSIX terminal emulator — and doesn't
// fight with however the person has their own shell prompt customized
// (a bespoke PS1/oh-my-zsh/starship setup), unlike trying to prepend text
// to their actual prompt string would.
static void set_terminal_title(const std::string& title) {
#ifdef _WIN32
    SetConsoleTitleA(title.c_str());
#else
    if (g_use_ansi) { std::printf("\033]0;%s\007", title.c_str()); std::fflush(stdout); }
#endif
}

// Windows only, deliberately: used to restore the title after a direct
// (not console-hosted) live view ends, e.g. `npulse tracert-mtr host` run
// straight from an existing shell. No POSIX equivalent is attempted — a
// POSIX terminal has no portable "query current title" API, but this is a
// low-stakes gap there: most interactive shells with any title-setting of
// their own (a PROMPT_COMMAND/precmd hook, oh-my-zsh, starship, ...)
// naturally overwrite whatever this left the moment they draw their next
// prompt anyway, so a title that briefly lingers after exit on a shell
// with none of that is a minor, common, and widely-tolerated cosmetic
// quirk (the same experience running `htop`/`vim` leaves on many
// terminals), not a functional problem the way the console-mode/input-
// buffer issues term_restore() addresses are.
#ifdef _WIN32
static std::string get_terminal_title() {
    char buf[1024] = {0};
    GetConsoleTitleA(buf, sizeof(buf));
    return std::string(buf);
}
#endif

static void print_console_banner(const char* shell_desc) {
    std::printf("%s=== npulse console ===%s\n", col_bold(), col_reset());
    std::printf("This is your normal shell (%s), with npulse's own folder added\n", shell_desc);
    std::printf("to PATH for this session. Try 'npulse help', or use 'ping', 'tracert',\n");
    std::printf("'traceroute', 'mtr', 'tracert-mtr', 'ifconfig', 'ipconfig', 'nslookup'\n");
    std::printf("directly, alongside any normal command for your OS.\n");
    std::printf("Type 'exit' to close this session.\n\n");
    std::fflush(stdout);
}

// Asked for explicitly: some standing, visible sign that a shell session
// is "inside" the npulse console beyond the one-time banner above (which
// scrolls away and is easy to lose track of, especially in a long
// session). Modifies the SPAWNED shell's own prompt to prefix
// "[npulse] " onto whatever it already was.
//
// BUG FIX, found while testing this properly rather than assuming a plain
// `setenv("PS1", ...)` would work: it doesn't, for the single most common
// case (an interactive bash or zsh session with a normal ~/.bashrc/
// ~/.zshrc). An INTERACTIVE shell always re-runs its own startup file on
// launch (bash: ~/.bashrc for a non-login shell; zsh: $ZDOTDIR/.zshrc,
// $ZDOTDIR defaulting to $HOME) — and that startup file is exactly where
// a real prompt customization lives, so it unconditionally overwrites
// whatever PS1 was inherited from the environment before the shell ever
// shows a prompt at all. Confirmed directly: spawned a genuinely
// interactive bash (a real pty on both stdin AND stdout — piped input
// alone isn't enough, since bash then correctly detects itself as
// non-interactive and skips this whole codepath, which is what made an
// earlier, more naive test of this look like it might be working when it
// wasn't really being exercised at all) and watched the prompt come back
// as bash's own configured default, with $PS1 holding that default too —
// the inherited value was never used.
//
// The real fix layers the prefix AFTER the user's own startup file runs,
// using each shell's own supported mechanism for exactly this:
//   bash: `--rcfile FILE -i` — FILE sources the user's real ~/.bashrc
//         first, then appends the prefix, so it's the last thing that
//         runs and can't be clobbered by anything before it.
//   zsh:  `ZDOTDIR` — zsh looks for its startup files in $ZDOTDIR instead
//         of $HOME when set; pointed at a temp directory whose .zshrc
//         does the same source-then-append.
//   anything else (plain sh/dash, an unrecognized $SHELL): falls back to
//         the plain `setenv(PS1)` — a real, known limitation (documented
//         in CLI.md) rather than a fully solved case, since minimal
//         shells don't have a standard "run this after your normal
//         startup" mechanism to hook the same way, but harmless where it
//         doesn't take effect. fish specifically is not attempted at all:
//         it has no PS1 concept, its prompt is a `fish_prompt` shell
//         function, which would need a temp function definition instead
//         of an environment variable — a real gap, out of scope here.
//
// Returns the extra argv entries to pass to execl() (empty for the
// fallback case) and the temp path created (if any), so the caller can
// clean it up after the session ends — mirrors setup_alias_dir()'s own
// create-now/clean-up-after-exit lifecycle for the same reason.
struct PromptPlan {
    std::vector<std::string> extra_args;
    std::string temp_path; // file (bash) or directory (zsh) to remove afterward; empty if nothing to clean up
    bool temp_is_dir = false;
};

#ifndef _WIN32
static std::string write_temp_file_posix(const std::string& tmpl, const std::string& content) {
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    int fd = mkstemp(buf.data());
    if (fd < 0) return {};
    ssize_t written = write(fd, content.data(), content.size());
    close(fd);
    if (written < 0 || static_cast<size_t>(written) != content.size()) { unlink(buf.data()); return {}; }
    return std::string(buf.data());
}

static PromptPlan prepare_prompt_plan(const std::string& shell) {
    PromptPlan plan;
    std::string shellName = basename_of(shell);
    if (shellName == "bash") {
        std::string rc =
            "[ -f ~/.bashrc ] && source ~/.bashrc\n"
            "PS1=\"[npulse] $PS1\"\n"
            "export PS1\n";
        std::string path = write_temp_file_posix("/tmp/npulse-bashrc-XXXXXX", rc);
        if (!path.empty()) {
            plan.extra_args = {"--rcfile", path, "-i"};
            plan.temp_path = path;
        }
    } else if (shellName == "zsh") {
        std::string origZdotdir = std::getenv("ZDOTDIR") ? std::getenv("ZDOTDIR")
                                 : (std::getenv("HOME") ? std::getenv("HOME") : "");
        std::string tmpl = "/tmp/npulse-zdotdir-XXXXXX";
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        if (mkdtemp(buf.data()) != nullptr) {
            std::string dir(buf.data());
            std::string rc =
                "ZDOTDIR=\"" + origZdotdir + "\"\n"
                "[ -f \"$ZDOTDIR/.zshrc\" ] && source \"$ZDOTDIR/.zshrc\"\n"
                "PROMPT=\"[npulse] $PROMPT\"\n"
                "PS1=\"[npulse] $PS1\"\n"
                "export PROMPT PS1\n";
            std::ofstream f(dir + "/.zshrc");
            f << rc;
            f.close();
            setenv("ZDOTDIR", dir.c_str(), 1);
            plan.temp_path = dir;
            plan.temp_is_dir = true;
        }
    } else {
        // Fallback for anything else (plain sh/dash, an unrecognized
        // $SHELL) — see this function's doc comment above for why this
        // doesn't reliably work the same way, kept only as a harmless
        // best-effort rather than doing nothing at all.
        const char* existing = std::getenv("PS1");
        std::string base = (existing && *existing) ? existing : "\\$ ";
        setenv("PS1", ("[npulse] " + base).c_str(), 1);
    }
    setenv("NPULSE_CONSOLE", "1", 1); // for any prompt framework/rc file that wants to check this itself
    return plan;
}

static void cleanup_prompt_plan(const PromptPlan& plan) {
    if (plan.temp_path.empty()) return;
    if (plan.temp_is_dir) {
        unlink((plan.temp_path + "/.zshrc").c_str());
        rmdir(plan.temp_path.c_str());
    } else {
        unlink(plan.temp_path.c_str());
    }
}
#endif

static void set_console_prompt_indicator() {
#ifdef _WIN32
    char existing[1024] = {0};
    DWORD n = GetEnvironmentVariableA("PROMPT", existing, sizeof(existing));
    std::string base = (n > 0) ? existing : "$P$G"; // cmd.exe's own documented default when PROMPT is unset
    SetEnvironmentVariableA("PROMPT", (std::string("[npulse] ") + base).c_str());
    // cmd.exe has no startup-file mechanism analogous to bash's ~/.bashrc
    // that runs unconditionally on launch (only an opt-in AutoRun registry
    // key most systems don't have configured), so — unlike the POSIX side
    // — a plain environment variable is genuinely sufficient here; nothing
    // downstream clobbers it before the first prompt is shown.
#endif
}

#ifdef _WIN32
static int launch_shell_console(const char* argv0) {
    std::string exePath = own_executable_path(argv0);
    std::string aliasDir = setup_alias_dir(exePath);
    prepend_dir_to_path(aliasDir);
    prepend_dir_to_path(own_executable_dir(argv0));
    set_console_prompt_indicator();
    set_terminal_title("npulse console");
    print_console_banner("cmd.exe");

    char comspec[MAX_PATH] = {0};
    const char* shell = (GetEnvironmentVariableA("COMSPEC", comspec, sizeof(comspec)) > 0) ? comspec : "cmd.exe";

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    // CreateProcessA's second argument (the mutable command line) must be
    // a writable buffer, not a string literal/temporary — it can rewrite
    // it in place (e.g. to split argv[0]).
    std::string cmdline = std::string("\"") + shell + "\"";
    std::vector<char> cmdlineBuf(cmdline.begin(), cmdline.end());
    cmdlineBuf.push_back('\0');
    int result = 1;
    if (!CreateProcessA(nullptr, cmdlineBuf.data(), nullptr, nullptr, /*bInheritHandles=*/TRUE,
                         0, nullptr, nullptr, &si, &pi)) {
        std::fprintf(stderr, "npulse: could not launch a shell (tried \"%s\")\n", shell);
    } else {
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD exitCode = 0;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        result = static_cast<int>(exitCode);
    }
    cleanup_alias_dir(aliasDir);
    return result;
}
#else
static int launch_shell_console(const char* argv0) {
    std::string exePath = own_executable_path(argv0);
    std::string aliasDir = setup_alias_dir(exePath);
    prepend_dir_to_path(aliasDir);
    prepend_dir_to_path(own_executable_dir(argv0));
    const char* shell = std::getenv("SHELL");
    if (!shell || !*shell) shell = "/bin/sh";
    PromptPlan promptPlan = prepare_prompt_plan(shell);
    set_terminal_title("npulse console");
    print_console_banner(shell);

    pid_t pid = fork();
    if (pid < 0) {
        std::fprintf(stderr, "npulse: could not fork a shell\n");
        cleanup_alias_dir(aliasDir);
        cleanup_prompt_plan(promptPlan);
        return 1;
    }
    if (pid == 0) {
        // Child: become the shell outright (exec, not run-and-wait) — same
        // process, same terminal, same job-control/signal behavior a
        // normal interactive shell in that terminal would have, rather
        // than a subprocess of a subprocess. execv, not execl, since
        // promptPlan.extra_args (bash's `--rcfile FILE -i`, say) is a
        // variable-length list execl's fixed arity can't express.
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(shell));
        for (auto& a : promptPlan.extra_args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execv(shell, argv.data());
        std::fprintf(stderr, "npulse: could not run shell \"%s\"\n", shell); // only reached if execv fails
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    cleanup_alias_dir(aliasDir); // parent only — the child became the shell and never returns here
    cleanup_prompt_plan(promptPlan);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
#endif


static char* fmt(char* buf, size_t n, const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    std::vsnprintf(buf, n, format, ap);
    va_end(ap);
    return buf;
}

// std::string-returning convenience wrapper around fmt() for inline use in
// expressions (argument lists, concatenation) where a caller-supplied
// buffer would be awkward — used by cmd_ping/cmd_tracert's summary lines.
static std::string fmt_str(const char* format, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, format);
    std::vsnprintf(buf, sizeof(buf), format, ap);
    va_end(ap);
    return std::string(buf);
}

// ---------------------------------------------------------------------------
static double cli_now_secs() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

static std::string take_next(const std::vector<std::string>& args, size_t& i) {
    if (i + 1 < args.size()) return args[++i];
    return {};
}

// ---------------------------------------------------------------------------
// `ping` — thin terminal front-end over PingRun (ping_run.hpp), the SAME
// engine the desktop app's own Ping tool uses. Flag meanings are this CLI's
// own, chosen for cross-platform consistency rather than bug-for-bug
// matching any one OS's native `ping` (whose flags already disagree with
// each other — Windows' `-t` means "ping until stopped", Linux/macOS's `-t`
// means "set IP TTL" — so exact compatibility with all three isn't
// achievable at once; see CLI.md's "Standard commands" section for the
// deliberate choice made here).
static void print_ping_usage() {
    std::fprintf(stderr,
        "usage: npulse ping HOST [-4|-6] [-c COUNT] [-i INTERVAL] [-W TIMEOUT]\n"
        "                        [-s SIZE] [-P icmp|udp|tcp] [-p PORT] [-I INTERFACE]\n"
        "                        [--continuous]\n");
}

static int cmd_ping(std::vector<std::string> args) {
    PingConfig cfg;
    cfg.count = 4;
    bool continuous = false;
    std::string target;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "-4") cfg.family = PingFamilyPref::V4;
        else if (a == "-6") cfg.family = PingFamilyPref::V6;
        else if (a == "-c" || a == "--count") cfg.count = std::atoi(take_next(args, i).c_str());
        else if (a == "-i" || a == "--interval") cfg.interval_secs = std::atof(take_next(args, i).c_str());
        else if (a == "-W" || a == "--timeout") cfg.timeout_secs = std::atof(take_next(args, i).c_str());
        else if (a == "-s" || a == "--size") cfg.payload_size = static_cast<size_t>(std::atoi(take_next(args, i).c_str()));
        else if (a == "-I" || a == "--interface") cfg.source_addr = take_next(args, i);
        else if (a == "--ttl") cfg.ttl = static_cast<uint8_t>(std::atoi(take_next(args, i).c_str()));
        else if (a == "-P" || a == "--protocol") {
            std::string p = take_next(args, i);
            if (p == "udp") cfg.protocol = PingProtocol::Udp;
            else if (p == "tcp") cfg.protocol = PingProtocol::Tcp;
            else cfg.protocol = PingProtocol::Icmp;
        } else if (a == "-p" || a == "--port") {
            auto port = static_cast<uint16_t>(std::atoi(take_next(args, i).c_str()));
            cfg.tcp_dest_port = port; cfg.udp_dest_port = port;
        } else if (a == "--continuous") { continuous = true; }
        else if (a == "-h" || a == "--help") { print_ping_usage(); return 0; }
        else if (!a.empty() && a[0] != '-') { target = a; }
    }
    if (target.empty()) { print_ping_usage(); return 1; }
    cfg.target = target;
    cfg.continuous = continuous;

    std::string proto_tag;
    if (cfg.protocol == PingProtocol::Tcp) proto_tag = fmt_str("TCP:%u", cfg.tcp_dest_port);
    else if (cfg.protocol == PingProtocol::Udp) proto_tag = fmt_str("UDP:%u", cfg.udp_dest_port);
    std::printf("%sPING%s %s%s%s  (payload %zu bytes%s)\n", col_bold(), col_reset(), target.c_str(),
        proto_tag.empty() ? "" : "  ", proto_tag.c_str(), cfg.payload_size,
        cfg.count > 0 && !continuous ? fmt_str(", %d probes", cfg.count).c_str() : "");

    PingRun run(1, cfg);
    int sent = 0, recv = 0;
    double rtt_min = 1e18, rtt_max = 0.0, rtt_sum = 0.0, rtt_sumsq = 0.0;
    run.run(&g_stop,
        [&](const PingLine& line) {
            ++sent;
            if (line.ok && line.rtt_ms) {
                ++recv;
                double r = *line.rtt_ms;
                rtt_min = std::min(rtt_min, r);
                rtt_max = std::max(rtt_max, r);
                rtt_sum += r; rtt_sumsq += r * r;
                // Color-tiered by RTT, matching the GUI's own latency
                // severity bands: fast/typical/slow read at a glance the
                // same way the loss-percentage coloring elsewhere does.
                const char* rtt_c = r < 50.0 ? col_green() : (r < 150.0 ? col_yellow() : col_red());
                std::printf("%sReply%s from %-15s  seq=%-4d  time=%s%.2f ms%s\n", col_green(), col_reset(),
                    line.from_ip.empty() ? target.c_str() : line.from_ip.c_str(), line.seq, rtt_c, r, col_reset());
            } else {
                std::printf("%sRequest%s seq=%-4d  %s\n", col_red(), col_reset(), line.seq,
                    line.note.empty() ? "timed out" : line.note.c_str());
            }
        },
        [] {});

    double loss_pct = sent ? (100.0 * static_cast<double>(sent - recv) / sent) : 0.0;
    const char* loss_c = loss_color(loss_pct);
    std::printf("\n%s--- %s ping statistics ---%s\n", col_bold(), target.c_str(), col_reset());
    std::printf("%d transmitted, %d received, %s%.1f%% loss%s\n", sent, recv, loss_c, loss_pct, col_reset());
    if (recv > 0) {
        double avg = rtt_sum / recv;
        double variance = (rtt_sumsq / recv) - (avg * avg);
        double mdev = variance > 0 ? std::sqrt(variance) : 0.0;
        std::printf("rtt min/avg/max/mdev = %.3f/%.3f/%.3f/%.3f ms\n", rtt_min, avg, rtt_max, mdev);
    }
    return recv > 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// `tracert` (also: `traceroute`) — classic ONE-SHOT progressive traceroute,
// matching Windows `tracert`/Linux `traceroute`'s output shape: each hop is
// printed exactly once, in order, as soon as it settles, and the run ends
// once the destination answers or --max-hops is exhausted. For the
// continuously-refreshing MTR-style live table instead, see `tracert-mtr`
// below — deliberately a SEPARATE command rather than a flag on this one,
// since the two have genuinely different output models (print-once-and-
// exit vs. live-monitor-indefinitely), matching the GUI's own single
// Path/MTR view being the tracert-mtr equivalent — the GUI has no
// classic-traceroute-style one-shot mode at all, this CLI adds one.
//
// `Session` probes concurrently across hops by design (see ARCHITECTURE.md
// §3) rather than strictly one-hop-at-a-time the way historic traceroute
// implementations do, so "wait for real replies, then print" is adapted
// as: a hop is considered SETTLED once it has either resolved (an address)
// or has gone 5 probes with zero replies (printed as "* * *", same as
// real traceroute's own silent-hop row) — not literal "send exactly 3
// probes and print their individual times" the way `traceroute -q 3` does,
// since the underlying engine's per-hop cadence isn't request/response
// paced that way. Documented here and in CLI.md rather than presented as
// an exact behavioral match.
static void print_tracert_usage() {
    std::fprintf(stderr,
        "usage: npulse tracert HOST [-4|-6] [-P icmp|udp|tcp|http] [-p PORT]\n"
        "                           [-m MAXHOPS] [-i INTERVAL] [-T TRACE-INTERVAL]\n"
        "                           [-W TIMEOUT] [-s PAYLOAD] [-I INTERFACE] [--unprivileged]\n"
        "(also available as: npulse traceroute)\n");
}

// Shared by `tracert` and `tracert-mtr` — both take the full option set the
// GUI's Add Target form exposes (family, protocol, port, max hops, probe
// interval, TRACE interval — Settings::trace_interval, the GUI's "Route
// re-discovery interval" field — timeout, payload size, source interface,
// and the raw/unprivileged toggle), differing only in output mode.
struct TraceOpts {
    Settings st;
    std::string target;
    int cycles = 0;
    bool report_mode = false;
    bool show_help = false;
};

static bool parse_trace_opts(const std::vector<std::string>& args, TraceOpts& o) {
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "-4") o.st.family = FamilyPref::V4;
        else if (a == "-6") o.st.family = FamilyPref::V6;
        else if (a == "-P" || a == "--protocol") {
            std::string p = take_next(args, i);
            if (p == "udp") o.st.protocol = Protocol::Udp;
            else if (p == "tcp") o.st.protocol = Protocol::Tcp;
            else if (p == "http") o.st.protocol = Protocol::Http;
            else o.st.protocol = Protocol::Icmp;
        } else if (a == "-p" || a == "--port") { o.st.dest_port = static_cast<uint16_t>(std::atoi(take_next(args, i).c_str())); }
        else if (a == "-m" || a == "--max-hops") { o.st.max_hops = static_cast<uint8_t>(std::atoi(take_next(args, i).c_str())); }
        else if (a == "-i" || a == "--interval") { o.st.probe_interval = std::atof(take_next(args, i).c_str()); }
        // Settings::trace_interval — the GUI's "Trace" field ("Route
        // re-discovery interval, seconds"), distinct from -i's per-hop
        // probe interval: how often the engine re-checks for a ROUTE
        // change rather than how often it re-measures an already-
        // discovered hop.
        else if (a == "-T" || a == "--trace-interval") { o.st.trace_interval = std::atof(take_next(args, i).c_str()); }
        else if (a == "-W" || a == "--timeout") { o.st.timeout = std::atof(take_next(args, i).c_str()); }
        else if (a == "-s" || a == "--payload") { o.st.payload_size = static_cast<size_t>(std::atoi(take_next(args, i).c_str())); }
        // Settings::source_addr — the GUI's "iface" dropdown (bind to a
        // specific local interface/source address instead of the OS's
        // default egress choice).
        else if (a == "-I" || a == "--interface") { o.st.source_addr = take_next(args, i); }
        // Settings::privileged — the inverse of the GUI's "Raw" checkbox
        // (raw/privileged ICMP is the GUI's default, same as here).
        else if (a == "--unprivileged") { o.st.privileged = false; }
        else if (a == "-c" || a == "--count") { o.cycles = std::atoi(take_next(args, i).c_str()); }
        else if (a == "-r" || a == "--report" || a == "--json") { o.report_mode = true; }
        else if (a == "-h" || a == "--help") { o.show_help = true; }
        else if (!a.empty() && a[0] != '-') { o.target = a; }
    }
    return !o.target.empty();
}

static int cmd_tracert(std::vector<std::string> args) {
    TraceOpts o;
    bool have_target = parse_trace_opts(args, o);
    if (o.show_help || !have_target) { print_tracert_usage(); return o.show_help ? 0 : 1; }

    std::string proto_tag;
    if (o.st.protocol == Protocol::Udp) proto_tag = "UDP";
    else if (o.st.protocol == Protocol::Tcp) proto_tag = "TCP";
    else if (o.st.protocol == Protocol::Http) proto_tag = "HTTP";
    std::printf("%sTracing route%s to %s%s%s over a maximum of %u hops:\n\n", col_bold(), col_reset(),
        o.target.c_str(), proto_tag.empty() ? "" : "  ", proto_tag.c_str(), o.st.max_hops);

    Session sess(1, o.target, o.st);
    std::atomic<bool> paused{false};
    uint8_t next_to_print = 1;
    bool reached_dest = false;
    sess.run(&g_stop, &paused, [&](const Snapshot& snap) {
        if (g_stop.load()) return;
        if (snap.error) {
            std::printf("%s%s%s\n", col_yellow(), snap.error->c_str(), col_reset());
            g_stop = true;
            return;
        }
        for (;;) {
            auto it = std::find_if(snap.hops.begin(), snap.hops.end(),
                [&](const HopStat& h) { return h.hop == next_to_print; });
            if (it == snap.hops.end()) break; // this hop hasn't started probing yet
            const HopStat& h = *it;
            bool resolved = h.address.has_value();
            bool gave_up = !resolved && h.sent >= 5; // see this section's own doc comment above
            if (!resolved && !gave_up) break; // still waiting on this hop — don't print yet

            if (resolved) {
                std::string shown = (h.hostname && !h.hostname->empty()) ? *h.hostname : *h.address;
                if (h.hostname && !h.hostname->empty()) shown += "  [" + *h.address + "]";
                if (h.is_dest) shown += "  [DEST]";
                char rtt[32] = "*";
                const char* rtt_c = col_dim();
                if (h.cur) {
                    std::snprintf(rtt, sizeof(rtt), "%.2f ms", *h.cur);
                    rtt_c = *h.cur < 50.0 ? col_green() : (*h.cur < 150.0 ? col_yellow() : col_red());
                }
                std::printf("%3u  %-46s  %s%s%s\n", h.hop, shown.c_str(), rtt_c, rtt, col_reset());
            } else {
                std::printf("%s%3u  * * *  Request timed out.%s\n", col_red(), h.hop, col_reset());
            }
            if (h.is_dest && resolved) { reached_dest = true; g_stop = true; }
            ++next_to_print;
            if (next_to_print > o.st.max_hops) { g_stop = true; break; }
        }
    });
    if (reached_dest) std::printf("\n%sTrace complete.%s\n", col_green(), col_reset());
    else std::printf("\n%sTrace incomplete: reached max hops (%u) without confirming the destination.%s\n",
        col_yellow(), o.st.max_hops, col_reset());
    return reached_dest ? 0 : 1;
}

// ---------------------------------------------------------------------------
// `tracert-mtr` (also: `mtr`) — the live, continuously-refreshing per-hop
// table, matching the GUI's Path/MTR view exactly (same Session, same
// HopStat fields). See `tracert` above for the one-shot alternative.
static void print_tracert_mtr_usage() {
    std::fprintf(stderr,
        "usage: npulse tracert-mtr HOST [-4|-6] [-P icmp|udp|tcp|http] [-p PORT]\n"
        "                                [-m MAXHOPS] [-i INTERVAL] [-T TRACE-INTERVAL]\n"
        "                                [-W TIMEOUT] [-s PAYLOAD] [-I INTERFACE]\n"
        "                                [--unprivileged] [-c CYCLES] [-r|--report]\n"
        "(also available as: npulse mtr)\n");
}

static std::string truncate(const std::string& s, size_t max_len) {
    if (s.size() <= max_len) return s;
    if (max_len <= 3) return s.substr(0, max_len);
    // BUG FIX: was "…" (UTF-8 E2 80 A6). A Windows console not explicitly
    // in UTF-8 mode misreads multi-byte UTF-8 as several separate
    // legacy-codepage characters — reported live as "Γÿà" where "★" should
    // have been (same root cause, different character) — which both
    // gives garbage output AND throws off this exact column's width
    // calculation (the padding math below counts bytes, and a multi-byte
    // sequence has more bytes than the single display column it's meant
    // to occupy). Plain ASCII "..." is guaranteed correct on every
    // console/codepage with no dependency on the terminal's Unicode
    // support at all.
    return s.substr(0, max_len - 3) + "...";
}

// Width of the Host column in the tracert-mtr live table. Widened from an
// earlier 38 — see this section's own BUG FIX comment below for why: a
// realistic reverse-DNS IPv6 hostname plus its address routinely exceeds
// 38 characters on its own, before any destination marker, making
// truncation (and the risk of cutting off something that matters) the
// COMMON case rather than a rare edge case. Also used for the header row
// and the separator-rule width, so both track this if it changes again.
static constexpr size_t kHostColumnWidth = 46;

static void render_hop_table(const Snapshot& snap, bool live) {
    if (live) begin_frame();
    char buf[320];
    const std::string sep(96 + (kHostColumnWidth - 38), '-');

    std::string title = std::string(col_bold()) + "npulse tracert-mtr" + col_reset() + "  " + snap.target;
    if (snap.dest_ip && *snap.dest_ip != snap.target) title += "  (" + *snap.dest_ip + ")";
    title += "  " + std::string(col_dim()) + (snap.family && *snap.family == Family::V6 ? "IPv6" : "IPv4") + col_reset();
    print_line(title);
    print_line(sep);

    if (snap.error) {
        print_line(std::string(col_yellow()) + "  " + *snap.error + col_reset());
        end_frame();
        return;
    }
    // BUG FIX: was "\xE2\x9A\xA0" ("⚠") — same UTF-8/codepage hazard as
    // truncate()'s ellipsis above.
    if (snap.loop_warning) print_line(std::string(col_yellow()) + "  !  " + *snap.loop_warning + col_reset());

    print_line(fmt(buf, sizeof(buf), (std::string("%s%-4s %-") + std::to_string(kHostColumnWidth) + "s %7s %6s %6s %8s %8s %8s %8s%s").c_str(),
        col_bold(), "Hop", "Host", "Loss%", "Sent", "Recv", "Last", "Avg", "Best", "Worst", col_reset()));
    print_line(sep);

    for (const auto& h : snap.hops) {
        // BUG FIX, reported live: a hop that had JUST been wiped by the
        // guarded-wipe/Frankenstein-route guard (see ARCHITECTURE.md §6 —
        // it clears a hop's address when it suspects a stale route, then
        // rediscovers it) showed as bare "*" while STILL displaying real,
        // healthy loss/RTT numbers next to it — HopStat's rolling stats
        // window doesn't instantly zero out at the same moment the address
        // is cleared (the guarded wipe specifically only fires on a hop
        // that WAS looking healthy — see kGuardedWipeMaxRecentLossPct's own
        // doc comment, session.cpp — so genuinely-recent good samples are
        // still inside the averaging window right after). "* with good
        // numbers" reads as a broken/inconsistent row; distinguishing WHY
        // there's no address (never yet resolved vs. just cleared and
        // being re-verified) makes the same real data self-explanatory
        // instead.
        std::string shown;
        if (h.address) {
            shown = *h.address;
            if (h.hostname && !h.hostname->empty()) shown = *h.hostname + " (" + shown + ")";
        } else if (h.recv > 0) {
            shown = "(re-resolving)";
        } else {
            shown = "*";
        }
        // BUG FIX: the destination marker used to be appended BEFORE
        // truncating — was "  \xE2\x98\x85" ("★"), a separate UTF-8/
        // codepage hazard already fixed elsewhere (now "[DEST]") — but
        // appending it first meant a long hostname+address (routine for a
        // resolved IPv6 reverse-DNS name, and reported live as exactly
        // this: an already-truncated-looking row where it was impossible
        // to tell whether [DEST] had been cut off) could truncate the
        // marker PARTIALLY OR ENTIRELY away — hiding the one piece of
        // information (this IS the destination) that matters most, on
        // exactly the row most likely to have a long enough hostname to
        // trigger it. Truncate the host/address text first, to a budget
        // that reserves room for the suffix, then append the suffix after
        // — guaranteeing it's always fully visible regardless of hostname
        // length.
        const std::string suffix = h.is_dest ? " [DEST]" : "";
        size_t budget = kHostColumnWidth > suffix.size() ? kHostColumnWidth - suffix.size() : 0;
        shown = truncate(shown, budget) + suffix;

        char cur[16] = "-", avg[16] = "-", mn[16] = "-", mx[16] = "-";
        if (h.cur) std::snprintf(cur, sizeof(cur), "%.2f", *h.cur);
        if (h.avg) std::snprintf(avg, sizeof(avg), "%.2f", *h.avg);
        if (h.min) std::snprintf(mn, sizeof(mn), "%.2f", *h.min);
        if (h.max) std::snprintf(mx, sizeof(mx), "%.2f", *h.max);

        print_line(fmt(buf, sizeof(buf), (std::string("%-4u %-") + std::to_string(kHostColumnWidth) + "s %s%6.1f%%%s %6zu %6zu %8s %8s %8s %8s").c_str(),
            h.hop, shown.c_str(), loss_color(h.loss), h.loss, col_reset(), h.sent, h.recv, cur, avg, mn, mx));
    }
    print_line(sep);
    end_frame();
}

// Runs `sess` until either a real SIGINT (g_stop) or `want_restart` becomes
// true, whichever comes first. Exists so cmd_tracert_mtr's Auto-family
// self-heal (below) can end a Session's run() to restart it internally
// WITHOUT that internal decision being confused with, or swallowing, a
// real user Ctrl-C: sess.run() only ever accepts one stop pointer, so this
// combines the two into one without letting either one shadow the other —
// the caller can tell them apart afterward by checking g_stop specifically.
static void run_session_until(Session& sess, std::atomic<bool>& want_restart,
                               const std::function<void(const Snapshot&)>& on_update) {
    std::atomic<bool> combined{false};
    std::atomic<bool> poller_stop{false};
    std::thread poller([&] {
        while (!poller_stop.load()) {
            if (g_stop.load() || want_restart.load()) { combined = true; return; }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
    std::atomic<bool> paused{false};
    sess.run(&combined, &paused, on_update);
    poller_stop = true;
    poller.join();
}

static int cmd_tracert_mtr(std::vector<std::string> args) {
    TraceOpts o;
    bool have_target = parse_trace_opts(args, o);
    if (o.show_help || !have_target) { print_tracert_mtr_usage(); return o.show_help ? 0 : 1; }
    if (o.report_mode && o.cycles == 0) o.cycles = 10; // see CLI.md: report/json default to 10 cycles when no -c given

    // Auto-refresh when Family is Auto — asked for explicitly as the
    // alternative to Force Recheck (which needs an interactive keypress
    // this simpler CLI live-view doesn't implement a key-reader for).
    // Session::resolve() (session.cpp) — which decides family availability
    // via has_local_v4/has_local_v6 — runs exactly ONCE, at the very top of
    // Session::run(), never re-evaluated mid-loop. So a trace started
    // before, say, a real IPv6 route came up would show "No local IPv6
    // egress available" and simply stay stuck showing that stale message
    // forever, with no way to retry short of restarting the whole command
    // by hand. This self-heals that specifically for Auto (a pinned -4/-6
    // has no ambiguity to retry — the person asked for that exact family
    // and an unavailable one should keep saying so, not silently swap
    // families on them): if the session reports an error continuously for
    // more than 15s, tear it down and construct a fresh one, which re-runs
    // resolve() and its family-availability check from scratch.
    bool auto_family = (o.st.family == FamilyPref::Auto);
    Snapshot last;
    bool got_any = false;
    // Enter the alternate screen buffer ONCE for the whole live session —
    // never for --report/--json, which never redraws at all and belongs
    // in normal scrollback like any other one-shot command output. See
    // enter_live_view()'s doc comment above for why this specifically
    // fixes the reported terminal corruption.
#ifdef _WIN32
    std::string savedTitle = o.report_mode ? std::string() : get_terminal_title();
#endif
    if (!o.report_mode) { enter_live_view(); set_terminal_title("npulse tracert-mtr " + o.target); }
    while (!g_stop.load()) {
        Session sess(1, o.target, o.st);
        int updates = 0;
        double error_since = 0.0;
        std::atomic<bool> want_restart{false};
        run_session_until(sess, want_restart, [&](const Snapshot& snap) {
            last = snap; got_any = true;
            if (!snap.hops.empty()) ++updates;
            if (!o.report_mode) render_hop_table(snap, true);
            if (o.cycles > 0 && updates >= o.cycles) { g_stop = true; return; }
            if (auto_family && snap.error) {
                if (error_since == 0.0) error_since = cli_now_secs();
                else if (cli_now_secs() - error_since > 15.0) want_restart = true;
            } else {
                error_since = 0.0;
            }
        });
        if (g_stop.load()) break; // real Ctrl-C, or -c cycles reached — either way, done
        if (!o.report_mode) print_line(std::string(col_yellow()) + "(Auto family: no local egress yet - retrying family detection...)" + col_reset());
        std::this_thread::sleep_for(std::chrono::seconds(3));
    }
    // Leave the alternate screen buffer before printing anything else —
    // MUST run regardless of how the loop above ended (normal completion,
    // -c cycles reached, or a real Ctrl-C caught by g_stop) so the
    // terminal is never left corrupted, matching what vim/less/htop
    // guarantee on every exit path, not just a clean one.
    leave_live_view();
#ifdef _WIN32
    if (!savedTitle.empty()) set_terminal_title(savedTitle); // see get_terminal_title()'s doc comment — Windows-only restore
#endif
    if (o.report_mode && got_any) render_hop_table(last, false);
    return 0;
}

// ---------------------------------------------------------------------------
// `ifconfig` / `ipconfig` / `interfaces` — full adapter listing, same data
// and same `usable` classification (is_cacheable_ip) as the desktop app's
// Interfaces page and its no-IPv4/no-IPv6 critical alert.
//
// `-w`/`--watch [SECS]` — asked for explicitly as an "auto-refresh"
// counterpart for interfaces specifically (distinct from tracert-mtr's
// Auto-family self-heal above, which is about a single in-progress trace,
// not about watching the adapter list itself change over time — e.g.
// noticing a cable get unplugged/replugged, or a VPN adapter come up).
static void print_ifconfig_usage() {
    std::fprintf(stderr, "usage: npulse ifconfig [-w [SECS]]   (default watch interval: 5s)\n");
}

static void render_interfaces(bool live) {
    if (live) begin_frame();
    char buf[320];
    const std::string sep(72, '-');
    print_line(std::string(col_bold()) + "npulse ifconfig" + col_reset());
    print_line(sep);
    // KIND column (FEATURE: "add more details" — see NetInterface::kind's
    // doc comment, transport.hpp) mirrors the desktop app's Interfaces tab
    // and source-Interface dropdown, so all three surfaces agree on which
    // adapter is Wi-Fi vs Ethernet vs a VPN/hypervisor virtual adapter.
    print_line(fmt(buf, sizeof(buf), "%s%-14s %-24s %-4s %-6s %-8s %-8s %-6s %s%s",
        col_bold(), "ADAPTER", "ADDRESS", "FAM", "STATE", "TYPE", "KIND", "MTU", "EGRESS", col_reset()));
    print_line(sep);
    for (const auto& ni : list_interfaces(/*include_all=*/true)) {
        bool usable = is_cacheable_ip(ni.address);
        const char* state_color = ni.up ? col_green() : col_dim();
        print_line(fmt(buf, sizeof(buf), "%-14s %-24s %-4s %s%-6s%s %-8s %-8s %-6u %s%s%s",
            ni.name.c_str(), ni.address.c_str(), ni.v6 ? "v6" : "v4",
            state_color, ni.up ? "up" : "down", col_reset(),
            ni.loopback ? "loop" : "normal", ni.kind.c_str(), ni.mtu,
            usable ? col_green() : col_dim(), usable ? "usable" : "not usable", col_reset()));
    }
    print_line(sep);
    end_frame();
}

static int cmd_ifconfig(std::vector<std::string> args) {
    double watch_secs = 0.0; // 0 = one-shot (default, unchanged from before)
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "-w" || a == "--watch") {
            bool next_is_number = (i + 1 < args.size()) && !args[i + 1].empty() &&
                (std::isdigit(static_cast<unsigned char>(args[i + 1][0])) || args[i + 1][0] == '.');
            watch_secs = next_is_number ? std::atof(take_next(args, i).c_str()) : 5.0;
        } else if (a == "-h" || a == "--help") { print_ifconfig_usage(); return 0; }
    }
    if (watch_secs <= 0.0) { render_interfaces(false); return 0; }
    // BUG FIX: this used to print a raw, unconditional "\033[2J\033[H"
    // every cycle — exactly the same class of bug reported against
    // tracert-mtr (see term_init()'s doc comment above): on a console that
    // hasn't opted into VT processing, those bytes are inert, so each
    // cycle just appended a fresh copy below the last instead of updating
    // in place. Now goes through the same begin_frame()/print_line()/
    // end_frame() machinery tracert-mtr uses, which is a no-op fallback
    // (plain sequential printing, no escape codes at all) when g_use_ansi
    // is false rather than emitting bytes that might not be honored. Also
    // wrapped in enter_live_view()/leave_live_view() — same terminal-
    // corruption fix as tracert-mtr's live view, and the same reason: this
    // is a continuously-redrawing view too, and needed the same alternate-
    // screen-buffer treatment.
    enter_live_view();
#ifdef _WIN32
    std::string savedTitle = get_terminal_title();
#endif
    set_terminal_title("npulse ifconfig (watching)");
    while (!g_stop.load()) {
        render_interfaces(true);
        char msg[128];
        std::snprintf(msg, sizeof(msg), "(auto-refreshing every %.0fs - Ctrl-C to stop)", watch_secs);
        print_line(std::string(col_dim()) + msg + col_reset());
        for (double waited = 0.0; waited < watch_secs && !g_stop.load(); waited += 0.2)
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    leave_live_view(); // MUST run regardless of exit path (Ctrl-C included) — see its own doc comment
#ifdef _WIN32
    if (!savedTitle.empty()) set_terminal_title(savedTitle);
#endif
    return 0;
}

// ---------------------------------------------------------------------------
// `dns` / `nslookup` — plain forward (A/AAAA)/reverse (PTR) lookup via the
// system resolver (getaddrinfo/getnameinfo). Deliberately NOT the desktop
// app's own native async DNS tool (a hickory-resolver-based Rust component
// on the Tauri side) — pulling that dependency into this C++ binary isn't
// worth it for a lookup this simple; a blocking getaddrinfo call is exactly
// what every OS's own `nslookup`/`dig` does under the hood too.
static bool looks_like_ip(const std::string& s) {
    return s.find(':') != std::string::npos ||
           (!s.empty() && (std::isdigit(static_cast<unsigned char>(s[0]))) && s.find_first_not_of("0123456789.") == std::string::npos);
}

static int cmd_dns(std::vector<std::string> args) {
    if (args.empty() || args[0] == "-h" || args[0] == "--help") {
        std::fprintf(stderr, "usage: npulse dns HOST_OR_IP\n");
        return args.empty() ? 1 : 0;
    }
    std::string q = args[0];
    (void)list_interfaces(); // touches ensure_winsock_ready() on Windows before any getaddrinfo/getnameinfo call below

    if (looks_like_ip(q)) {
        struct addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_UNSPEC;
        hints.ai_flags = AI_NUMERICHOST;
        if (getaddrinfo(q.c_str(), nullptr, &hints, &res) != 0 || !res) {
            std::fprintf(stderr, "\"%s\" is not a valid address\n", q.c_str());
            return 1;
        }
        char host[NI_MAXHOST] = {0};
        int rc = getnameinfo(res->ai_addr, static_cast<socklen_t>(res->ai_addrlen), host, sizeof(host), nullptr, 0, NI_NAMEREQD);
        freeaddrinfo(res);
        if (rc != 0) { std::printf("%s: no PTR record\n", q.c_str()); return 0; }
        std::printf("%s -> %s\n", q.c_str(), host);
        return 0;
    }

    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    if (getaddrinfo(q.c_str(), nullptr, &hints, &res) != 0 || !res) {
        std::fprintf(stderr, "could not resolve %s\n", q.c_str());
        return 1;
    }
    bool any = false;
    for (auto* p = res; p; p = p->ai_next) {
        char ip[INET6_ADDRSTRLEN] = {0};
        if (p->ai_family == AF_INET) {
            inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(p->ai_addr)->sin_addr, ip, sizeof(ip));
            std::printf("A     %s\n", ip); any = true;
        } else if (p->ai_family == AF_INET6) {
            inet_ntop(AF_INET6, &reinterpret_cast<sockaddr_in6*>(p->ai_addr)->sin6_addr, ip, sizeof(ip));
            std::printf("AAAA  %s\n", ip); any = true;
        }
    }
    freeaddrinfo(res);
    if (!any) std::printf("no A/AAAA records for %s\n", q.c_str());
    return 0;
}

// ---------------------------------------------------------------------------
// `portscan` — a plain TCP connect scan. Deliberately self-contained rather
// than reusing the desktop app's native port-scanner tool: that tool's
// engine lives behind the same Tauri FFI boundary as everything else in
// this file's doc comment describes, but a connect-scan is simple enough
// (and different enough in shape — many short-lived sockets, not one
// long-running Session) that duplicating it here in ~20 lines is clearer
// than threading a shared abstraction through both call sites for it.
static int cmd_portscan(std::vector<std::string> args) {
    std::string target;
    std::vector<int> ports;
    double timeout_secs = 1.0;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "-p" || a == "--ports") {
            std::string spec = take_next(args, i);
            size_t dash = spec.find('-');
            if (dash != std::string::npos) {
                int lo = std::atoi(spec.substr(0, dash).c_str());
                int hi = std::atoi(spec.substr(dash + 1).c_str());
                for (int p = lo; p <= hi; ++p) ports.push_back(p);
            } else {
                size_t start = 0, comma;
                while ((comma = spec.find(',', start)) != std::string::npos) {
                    ports.push_back(std::atoi(spec.substr(start, comma - start).c_str()));
                    start = comma + 1;
                }
                ports.push_back(std::atoi(spec.substr(start).c_str()));
            }
        } else if (a == "-W" || a == "--timeout") { timeout_secs = std::atof(take_next(args, i).c_str()); }
        else if (a == "-h" || a == "--help") { std::fprintf(stderr, "usage: npulse portscan HOST -p PORT[,PORT|-PORT]\n"); return 0; }
        else if (!a.empty() && a[0] != '-') { target = a; }
    }
    if (target.empty() || ports.empty()) {
        std::fprintf(stderr, "usage: npulse portscan HOST -p PORT[,PORT|-PORT]\n");
        return 1;
    }
    (void)list_interfaces(); // ensure_winsock_ready() before any socket() call below, same as cmd_dns

    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(target.c_str(), nullptr, &hints, &res) != 0 || !res) {
        std::fprintf(stderr, "could not resolve %s\n", target.c_str());
        return 1;
    }
    std::printf("Scanning %s (%zu port%s)\n", target.c_str(), ports.size(), ports.size() == 1 ? "" : "s");
    int open_count = 0;
    for (int port : ports) {
        if (g_stop) break;
        sockaddr_storage ss{};
        socklen_t sslen = 0;
        if (res->ai_family == AF_INET) {
            sockaddr_in a4 = *reinterpret_cast<sockaddr_in*>(res->ai_addr);
            a4.sin_port = htons(static_cast<uint16_t>(port));
            std::memcpy(&ss, &a4, sizeof(a4)); sslen = sizeof(a4);
        } else {
            sockaddr_in6 a6 = *reinterpret_cast<sockaddr_in6*>(res->ai_addr);
            a6.sin6_port = htons(static_cast<uint16_t>(port));
            std::memcpy(&ss, &a6, sizeof(a6)); sslen = sizeof(a6);
        }
#ifdef _WIN32
        SOCKET fd = socket(res->ai_family, SOCK_STREAM, IPPROTO_TCP);
        if (fd == INVALID_SOCKET) continue;
        u_long nb = 1; ioctlsocket(fd, FIONBIO, &nb);
#else
        int fd = socket(res->ai_family, SOCK_STREAM, IPPROTO_TCP);
        if (fd < 0) continue;
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
#endif
        connect(fd, reinterpret_cast<sockaddr*>(&ss), sslen); // non-blocking — expected to return "in progress"
        fd_set wfds; FD_ZERO(&wfds); FD_SET(fd, &wfds);
        timeval tv{}; tv.tv_sec = static_cast<long>(timeout_secs); tv.tv_usec = static_cast<long>((timeout_secs - tv.tv_sec) * 1e6);
        int sel = select(static_cast<int>(fd + 1), nullptr, &wfds, nullptr, &tv);
        bool open = false;
        if (sel > 0) {
            int err = 0; socklen_t elen = sizeof(err);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &elen);
            open = (err == 0);
        }
#ifdef _WIN32
        closesocket(fd);
#else
        close(fd);
#endif
        if (open) { std::printf("%-6d open\n", port); ++open_count; }
    }
    freeaddrinfo(res);
    std::printf("%d of %zu port%s open\n", open_count, ports.size(), ports.size() == 1 ? "" : "s");
    return 0;
}

// ---------------------------------------------------------------------------
// `completion` — see CLI.md's "Shell auto-completion" section. Each branch
// is a static, hand-written script (not generated from the flag list
// programmatically) so it can use each shell's own idiomatic completion
// primitives rather than a lowest-common-denominator scheme.
static int cmd_completion(std::vector<std::string> args) {
    std::string shell = args.empty() ? "" : args[0];
    if (shell == "bash") {
        std::printf(
            "_npulse_complete() {\n"
            "  local cur=\"${COMP_WORDS[COMP_CWORD]}\"\n"
            "  local subs=\"ping tracert traceroute tracert-mtr mtr ifconfig ipconfig interfaces dns nslookup portscan completion help version\"\n"
            "  if [ \"$COMP_CWORD\" -eq 1 ]; then COMPREPLY=($(compgen -W \"$subs\" -- \"$cur\")); return; fi\n"
            "  local flags=\"-4 -6 -P --protocol -p --port -m --max-hops -i --interval -T --trace-interval -c --count -r --report --json -W --timeout -s --size --payload -I --interface --unprivileged -w --watch --continuous -h --help\"\n"
            "  if [[ \"$cur\" == -* ]]; then COMPREPLY=($(compgen -W \"$flags\" -- \"$cur\")); return; fi\n"
            "  local recent=\"$HOME/.local/share/netpulse/cli-recent\"\n"
            "  [ -f \"$recent\" ] && COMPREPLY=($(compgen -W \"$(cat \"$recent\")\" -- \"$cur\"))\n"
            "}\n"
            "complete -F _npulse_complete npulse\n");
    } else if (shell == "zsh") {
        std::printf(
            "#compdef npulse\n"
            "_npulse() {\n"
            "  local -a subs; subs=(ping tracert traceroute tracert-mtr mtr ifconfig ipconfig interfaces dns nslookup portscan completion help version)\n"
            "  if (( CURRENT == 2 )); then _describe 'command' subs; return; fi\n"
            "  local recent=\"$HOME/.local/share/netpulse/cli-recent\"\n"
            "  [ -f \"$recent\" ] && _values 'recent target' $(cat \"$recent\")\n"
            "}\n"
            "compdef _npulse npulse\n");
    } else if (shell == "fish") {
        std::printf(
            "set -l subs ping tracert traceroute tracert-mtr mtr ifconfig ipconfig interfaces dns nslookup portscan completion help version\n"
            "complete -c npulse -n \"__fish_use_subcommand\" -a \"$subs\"\n"
            "complete -c npulse -l protocol -x -a \"icmp udp tcp http\"\n"
            "complete -c npulse -l trace-interval -d 'Route re-discovery interval, seconds'\n"
            "complete -c npulse -l interface -d 'Bind to a specific local interface/source address'\n"
            "complete -c npulse -l unprivileged -d 'Use unprivileged datagram ICMP instead of raw sockets'\n"
            "complete -c npulse -l report -d 'Static report, no live redraw'\n"
            "complete -c npulse -l json -d 'JSON output'\n"
            "complete -c npulse -l watch -d 'Auto-refresh (ifconfig)'\n");
    } else if (shell == "powershell") {
        std::printf(
            "Register-ArgumentCompleter -Native -CommandName npulse -ScriptBlock {\n"
            "  param($wordToComplete, $commandAst, $cursorPosition)\n"
            "  $subs = 'ping','tracert','traceroute','tracert-mtr','mtr','ifconfig','ipconfig','interfaces','dns','nslookup','portscan','completion','help','version'\n"
            "  $subs | Where-Object { $_ -like \"$wordToComplete*\" } | ForEach-Object { [System.Management.Automation.CompletionResult]::new($_, $_, 'ParameterValue', $_) }\n"
            "}\n");
    } else {
        std::fprintf(stderr, "usage: npulse completion bash|zsh|fish|powershell\n");
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
static void print_help() {
    std::printf(
        "npulse - NetPulse command-line tools\n\n"
        "usage: npulse COMMAND [args]\n\n"
        "commands:\n"
        "  ping         Ping a host (ICMP/UDP/TCP) - like ping/ping.exe\n"
        "  tracert      One-shot progressive route trace - also: traceroute\n"
        "               (classic Windows tracert / Linux traceroute style)\n"
        "  tracert-mtr  Live, continuously-refreshing per-hop table - also: mtr\n"
        "               (like the desktop app's Path/MTR view)\n"
        "  ifconfig     List every network adapter (-w to auto-refresh) - also: ipconfig, interfaces\n"
        "  dns          Forward/reverse DNS lookup - also: nslookup\n"
        "  portscan     TCP connect scan\n"
        "  completion   Print a shell auto-completion script (bash|zsh|fish|powershell)\n"
        "  version      Print engine build info\n\n"
        "See cli/CLI.md in the NetPulse repository for the full reference.\n");
}

int main(int argc, char** argv) {
    std::signal(SIGINT, on_sigint);
#ifdef SIGTERM
    std::signal(SIGTERM, on_sigint);
#endif
    // BUG FIX: term_init() (this file's ANSI/VT opt-in, flicker-free
    // redraw, and color infrastructure — see its own doc comment above)
    // was fully written but never actually CALLED anywhere, leaving
    // g_use_ansi permanently false and every live view still doing the
    // exact raw, always-scrolling redraw that prompted writing it in the
    // first place. Must run before any command that might redraw a live
    // view (tracert-mtr, ifconfig -w) — here, once, at the top of main(),
    // is the one place guaranteed to run before all of them (including the
    // console-hosting fallback right below, which prints a colored banner).
    term_init();

    std::string prog = basename_of(argv[0] ? argv[0] : "npulse");
    std::vector<std::string> args(argv + 1, argv + argc);

    std::string mode;
    if (prog == "ping") mode = "ping";
    else if (prog == "tracert" || prog == "traceroute") mode = "tracert";
    else if (prog == "mtr" || prog == "tracert-mtr") mode = "tracert-mtr";
    else if (prog == "ifconfig" || prog == "ipconfig") mode = "ifconfig";
    else if (prog == "nslookup") mode = "dns";
    else {
        // BUG FIX (behavior clarified): the canonical `npulse`/`netpulse`
        // invocation with literally zero arguments used to just print help
        // and exit (on POSIX always; on Windows, only when NOT a freshly-
        // allocated console). Asked for uniformly across every OS and both
        // invocation styles instead: open the "VS Developer Command
        // Prompt"-style console (launch_shell_console(), above) whether
        // typed bare into an already-open terminal OR the file run
        // directly (double-click, a shortcut, a file manager's "Open").
        // An ALIAS invocation (`ping`, `tracert`, `mtr`, `ifconfig`, ...)
        // with no further arguments is a DIFFERENT case, handled by the
        // branches above this `else` — someone who typed `ping` explicitly
        // asked for `ping`'s own behavior (its own usage message on no
        // target), not npulse's general console, so those are unaffected.
        if (args.empty()) { return launch_shell_console(argv[0]); }
        std::string sub = args[0];
        args.erase(args.begin());
        if (sub == "ping") mode = "ping";
        else if (sub == "tracert" || sub == "traceroute") mode = "tracert";
        else if (sub == "tracert-mtr" || sub == "mtr") mode = "tracert-mtr";
        else if (sub == "ifconfig" || sub == "ipconfig" || sub == "interfaces") mode = "ifconfig";
        else if (sub == "dns" || sub == "nslookup") mode = "dns";
        else if (sub == "portscan" || sub == "nmap") mode = "portscan";
        else if (sub == "completion") mode = "completion";
        else if (sub == "version" || sub == "--version") { std::printf("npulse (netpulse-cli)\n"); return 0; }
        else if (sub == "help" || sub == "--help" || sub == "-h") { print_help(); return 0; }
        else { std::fprintf(stderr, "npulse: unknown command '%s'\n\n", sub.c_str()); print_help(); return 1; }
    }

    if (mode == "ping") return cmd_ping(args);
    if (mode == "tracert") return cmd_tracert(args);
    if (mode == "tracert-mtr") return cmd_tracert_mtr(args);
    if (mode == "ifconfig") return cmd_ifconfig(args);
    if (mode == "dns") return cmd_dns(args);
    if (mode == "portscan") return cmd_portscan(args);
    if (mode == "completion") return cmd_completion(args);
    print_help();
    return 1;
}
