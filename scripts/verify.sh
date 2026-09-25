#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Full-stack verification. Runs every check that can be performed without a
# live network or a Windows host, so that "it builds" is a fact rather than an
# assumption.
#
# Why this exists: this project spans four languages (C++ engine, Rust shell,
# JS/JSX frontend, JSON IPC manifests) and a change in one can silently break
# another. Two real examples this script would have caught immediately:
#   * A new Tauri command registered in lib.rs but NOT granted in
#     capabilities/default.json is silently unreachable at runtime — no build
#     error, no warning, it just fails when invoked.
#   * A JSX string containing a literal "\u2022" renders as visible garbage
#     rather than a bullet; only an actual parse/render catches that class of
#     mistake.
#
# Exit code is non-zero if any stage fails, so it drops straight into CI.
# Stages degrade gracefully: a missing optional toolchain is reported as
# SKIP, never as a silent pass.
# ---------------------------------------------------------------------------
set -uo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"
FAIL=0
pass() { printf '  \033[32mPASS\033[0m %s\n' "$1"; }
fail() { printf '  \033[31mFAIL\033[0m %s\n' "$1"; FAIL=1; }
skip() { printf '  \033[33mSKIP\033[0m %s\n' "$1"; }
hdr()  { printf '\n\033[1m== %s ==\033[0m\n' "$1"; }

# --------------------------------------------------------------- 1. C++ core
hdr "C++ core (build + unit tests + strict warnings)"
if command -v cmake >/dev/null && command -v c++ >/dev/null; then
  BD="$ROOT/.verify-build"
  rm -rf "$BD"; mkdir -p "$BD"
  if cmake -S "$ROOT" -B "$BD" -DCMAKE_BUILD_TYPE=Release \
       -DCMAKE_CXX_FLAGS="-Wall -Wextra" >/dev/null 2>&1 \
     && cmake --build "$BD" -j"$(nproc 2>/dev/null || echo 2)" >"$BD/build.log" 2>&1; then
    pass "core + tests compile"
    if "$BD/netpulse_tests" >"$BD/test.log" 2>&1; then
      pass "unit tests ($(grep -c '  ok:' "$BD/test.log" || echo 0) assertions)"
    else
      fail "unit tests"; tail -20 "$BD/test.log"
    fi
    # Warning budget: fail on NEW warnings, tolerate the known pre-existing
    # ones so the gate stays meaningful instead of being permanently red.
    WARNS=$(grep -c 'warning:' "$BD/build.log" || true)
    if [ "$WARNS" -le 2 ]; then pass "warning budget ($WARNS <= 2 known)"
    else fail "warning budget ($WARNS > 2 — new warnings introduced)"
         grep 'warning:' "$BD/build.log" | head -10; fi
  else
    fail "C++ build"; tail -25 "$BD/build.log" 2>/dev/null
  fi
else
  skip "C++ (cmake/c++ not installed)"
fi

# ------------------------------------------------------------- 2. Frontend JS
hdr "Per-translation-unit compile (catches masked missing #includes)"
# CMake links every core/src/*.cpp into ONE static library, so a .cpp that
# forgets to #include something it uses can still compile clean there purely
# because a DIFFERENT .cpp in the same link happens to include it first â the
# exact bug a live Windows/MSVC build hit: netpulse_ffi.cpp called
# process_is_elevated()/capture_driver_present() without including
# platform.hpp, compiled fine in every sandbox check here (which only ever
# built the whole lib together), and only failed on a real machine where
# netpulse_ffi.cpp is its OWN translation unit with its OWN include list.
# -fsyntax-only on each .cpp individually is what actually catches this: it
# reproduces exactly how a compiler treats one .cpp, include by include, with
# nothing "borrowed" from a neighboring file.
if command -v c++ >/dev/null; then
  TU_FAIL=0
  while IFS= read -r f; do
    if ! c++ -std=c++20 -fsyntax-only -I "$ROOT/core/include" "$f" >/tmp/tu.log 2>&1; then
      fail "standalone compile: $f"; head -8 /tmp/tu.log; TU_FAIL=1
    fi
  done < <(find "$ROOT/core/src" -name '*.cpp' | sort)
  [ "$TU_FAIL" = 0 ] && pass "every core/src/*.cpp compiles standalone"

  # netpulse_ffi.cpp needs the cxx bridge's rust::String/Str/Slice types,
  # which only exist once cargo has generated them. A minimal, structurally
  # faithful mock (same public surface: implicit std::string conversion,
  # size()/data()) is enough to genuinely syntax-check this file the same
  # way â it is not a weaker check, since the exact bug above involved no
  # cxx-bridge machinery at all, just a missing #include.
  MOCKDIR="$(mktemp -d)"
  mkdir -p "$MOCKDIR/rust"
  cat > "$MOCKDIR/rust/cxx.h" <<'CXXMOCK'
#pragma once
#include <string>
#include <cstdint>
namespace rust {
class String { public: String()=default; String(const std::string&s):s_(s){} String(const char*s):s_(s){} operator std::string()const{return s_;} private: std::string s_; };
class Str { public: Str(const char*s=""):s_(s){} Str(const std::string&s):s_(s){} operator std::string()const{return s_;} size_t size()const{return s_.size();} private: std::string s_; };
template <typename T> class Slice { public: Slice()=default; const T* data()const{return nullptr;} size_t size()const{return 0;} const T* begin()const{return nullptr;} const T* end()const{return nullptr;} };
}
CXXMOCK
  FFI="$ROOT/tauri-app/src-tauri/native/netpulse_ffi.cpp"
  if [ -f "$FFI" ]; then
    if c++ -std=c++20 -fsyntax-only -I "$MOCKDIR" -I "$ROOT/core/include" \
         -I "$ROOT/tauri-app/src-tauri/native" "$FFI" >/tmp/ffi_tu.log 2>&1; then
      pass "netpulse_ffi.cpp compiles standalone (mocked cxx bridge)"
    else
      fail "netpulse_ffi.cpp standalone compile"; head -12 /tmp/ffi_tu.log
    fi
  fi
  rm -rf "$MOCKDIR"
else
  skip "per-TU compile (c++ not installed)"
fi

hdr "GitHub Actions workflow YAML"
# Deployment-audit gap this closes: several real bugs were found by hand in
# these files (a checkout step with no `ref:`, silently building whatever
# `main` currently looks like instead of the tagged commit a
# workflow_dispatch explicitly asked to rebuild; a missing macOS universal-
# binary target, meaning Intel Mac users got no working release at all) —
# both were only caught by actually reading the files end to end, which
# nothing here had ever done before. This stage can't catch THAT class of
# semantic bug automatically, but a broken YAML file failing silently until
# someone happens to push a tag is a much worse way to find out than a
# parse check on every run.
if command -v python3 >/dev/null; then
  python3 - "$ROOT" <<'PY'
import sys, pathlib
try:
    import yaml
except ImportError:
    print("  \033[33mSKIP\033[0m PyYAML not installed"); sys.exit(0)
root = pathlib.Path(sys.argv[1])
wf_dir = root / '.github' / 'workflows'
ok = True
for f in sorted(wf_dir.glob('*.yml')):
    try:
        yaml.safe_load(f.read_text(encoding='utf-8'))
        print(f"  \033[32mPASS\033[0m {f.name}")
    except Exception as e:
        print(f"  \033[31mFAIL\033[0m {f.name}: {e}")
        ok = False
sys.exit(0 if ok else 1)
PY
  [ $? -ne 0 ] && FAIL=1
else
  skip "workflow YAML (python3 not available)"
fi

hdr "Windows cross-compile (catches Win32-only header/API bugs)"
# The C++ core has real, historically-proven Windows-only failure modes that
# NOTHING checked so far can see: native g++ on Linux never even parses the
# `#ifdef _WIN32` branches (the preprocessor drops them before the compiler
# ever looks at them), and MSVC/the real Windows SDK headers are not
# available here at all. Two concrete, real bugs slipped through purely
# because of this gap: (1) platform.hpp used HANDLE/TOKEN_ELEVATION/
# LoadLibraryExW with no #include <windows.h> anywhere reachable from it on
# some paths, and (2) manager.hpp's own <windows.h> include, with no
# <winsock2.h>-first ordering, caused a real MSVC build to fail with
# hundreds of "struct redefinition" errors compiling netpulse_ffi.cpp. A
# MinGW-w64 cross-compile is not a perfect substitute for the real Microsoft
# SDK headers (their winsock.h/winsock2.h implementation is stricter about
# this exact class of conflict than MinGW\'s), but it DOES compile the actual
# _WIN32 code paths against a real windows.h/winsock2.h, which is strictly
# more coverage than every other stage in this script combined provides.
MINGW=x86_64-w64-mingw32-g++
if command -v "$MINGW" >/dev/null; then
  WIN_FAIL=0
  while IFS= read -r f; do
    if ! "$MINGW" -std=c++20 -fsyntax-only -DNOMINMAX -I "$ROOT/core/include" "$f" >/tmp/win_tu.log 2>&1; then
      fail "Windows compile: $f"; head -10 /tmp/win_tu.log; WIN_FAIL=1
    fi
  done < <(find "$ROOT/core/src" -name '*.cpp' | sort)
  [ "$WIN_FAIL" = 0 ] && pass "every core/src/*.cpp compiles for Windows (MinGW-w64)"

  FFI="$ROOT/tauri-app/src-tauri/native/netpulse_ffi.cpp"
  MOCKDIR="$(mktemp -d)"; mkdir -p "$MOCKDIR/rust"
  cat > "$MOCKDIR/rust/cxx.h" <<'CXXMOCK'
#pragma once
#include <string>
#include <cstdint>
namespace rust {
class String { public: String()=default; String(const std::string&s):s_(s){} String(const char*s):s_(s){} operator std::string()const{return s_;} private: std::string s_; };
class Str { public: Str(const char*s=""):s_(s){} Str(const std::string&s):s_(s){} operator std::string()const{return s_;} size_t size()const{return s_.size();} private: std::string s_; };
template <typename T> class Slice { public: Slice()=default; const T* data()const{return nullptr;} size_t size()const{return 0;} const T* begin()const{return nullptr;} const T* end()const{return nullptr;} };
}
CXXMOCK
  if [ -f "$FFI" ]; then
    # Deliberately NOT passing -DWIN32_LEAN_AND_MEAN here, even though
    # build.rs now defines it for the real build: this test's whole point is
    # to prove the header ordering is correct on its OWN merits (the
    # manager.hpp fix), not to rely on a compiler flag masking a problem
    # that would resurface the moment anyone's include order shifts again.
    if "$MINGW" -std=c++20 -fsyntax-only -DNOMINMAX -I "$MOCKDIR" -I "$ROOT/core/include" \
         -I "$ROOT/tauri-app/src-tauri/native" "$FFI" >/tmp/win_ffi.log 2>&1; then
      if grep -q 'warning' /tmp/win_ffi.log; then
        fail "netpulse_ffi.cpp Windows compile has warnings (header-order fragility):"
        cat /tmp/win_ffi.log
      else
        pass "netpulse_ffi.cpp compiles for Windows, zero warnings (mocked cxx bridge)"
      fi
    else
      fail "netpulse_ffi.cpp Windows compile"; cat /tmp/win_ffi.log
    fi
  fi
  rm -rf "$MOCKDIR"
else
  skip "Windows cross-compile (g++-mingw-w64-x86-64 not installed)"
fi

hdr "Frontend (JSX/JS parse)"
EB=""
for c in "$ROOT/tauri-app/node_modules/.bin/esbuild" "$(command -v esbuild || true)" /tmp/jsxcheck/node_modules/.bin/esbuild; do
  [ -n "$c" ] && [ -x "$c" ] && EB="$c" && break
done
if [ -n "$EB" ]; then
  EXT="--external:react --external:react-dom --external:react-dom/client --external:recharts
       --external:lucide-react --external:xlsx --external:motion --external:tailwindcss
       --external:@tauri-apps/api --external:@tauri-apps/api/core --external:@tauri-apps/api/event
       --external:@tauri-apps/plugin-dialog --external:@tauri-apps/plugin-updater
       --external:@tauri-apps/plugin-process --external:*.css"
  ok=1
  while IFS= read -r f; do
    if ! $EB "$f" --loader:.js=jsx --bundle --format=esm $EXT --outfile=/dev/null >/tmp/eb.err 2>&1; then
      fail "parse $f"; grep -E '^(✘|  )' /tmp/eb.err | head -6; ok=0
    fi
  done < <(find "$ROOT/tauri-app/src" -name '*.jsx' -o -name '*.js' | sort)
  [ "$ok" = 1 ] && pass "all .js/.jsx parse under the JSX loader"
else
  skip "frontend (esbuild not available: npm i -D esbuild)"
fi

# --------------------------------------------------- 3. IPC manifest coverage
# A Tauri command registered in lib.rs but absent from capabilities/default.json
# is silently unreachable at runtime. This is a hard gate.
hdr "Tauri IPC permission coverage"
# Three locations must agree, and a mismatch in ANY direction is a real bug:
#   1. lib.rs invoke_handler![]   -- what the frontend can actually call
#   2. build.rs COMMANDS list     -- what tauri-build GENERATES permissions
#      for at all (capabilities/default.json can only grant a permission
#      that this list caused to exist -- this is the location a real report
#      showed missing: "Permission allow-capabilities not found", because a
#      command was added to (1) and (3) but never to (2), which is the
#      actual source of truth)
#   3. capabilities/default.json  -- what is actually GRANTED to the webview
# All three must contain exactly the same command set, or either the build
# fails outright (missing from 2) or a command silently can't be invoked at
# runtime with no compile error at all (missing from 3).
python3 - "$ROOT" <<'PY'
import re, json, sys, pathlib
root = pathlib.Path(sys.argv[1])
libf = root/'tauri-app/src-tauri/src/lib.rs'
bldf = root/'tauri-app/src-tauri/build.rs'
capf = root/'tauri-app/src-tauri/capabilities/default.json'

handler_cmds = set(re.findall(r'commands::(\w+)', libf.read_text(encoding='utf-8')))

bld_src = bldf.read_text(encoding='utf-8')
m = re.search(r'const COMMANDS:\s*&\[&str\]\s*=\s*&\[(.*?)\];', bld_src, re.S)
if not m:
    print("  \033[31mFAIL\033[0m build.rs: could not locate `const COMMANDS: &[&str] = &[...]`")
    sys.exit(1)
build_cmds = set(re.findall(r'"(\w+)"', m.group(1)))

try:
    perms = set(json.loads(capf.read_text(encoding='utf-8'))['permissions'])
except Exception as e:
    print(f"  \033[31mFAIL\033[0m capabilities/default.json unreadable: {e}")
    sys.exit(1)
granted_cmds = {p[len('allow-'):].replace('-', '_') for p in perms if p.startswith('allow-')}

ok = True
missing_from_build = handler_cmds - build_cmds
if missing_from_build:
    print(f"  \033[31mFAIL\033[0m in lib.rs but missing from build.rs COMMANDS "
          f"(build will fail with \"Permission allow-x not found\"): {sorted(missing_from_build)}")
    ok = False
missing_from_caps = handler_cmds - granted_cmds
if missing_from_caps:
    print(f"  \033[31mFAIL\033[0m in lib.rs but not granted in capabilities/default.json "
          f"(silently uninvokable at runtime, no build error): {sorted(missing_from_caps)}")
    ok = False
orphaned_in_build = build_cmds - handler_cmds
if orphaned_in_build:
    print(f"  \033[33mWARN\033[0m in build.rs COMMANDS but not registered in lib.rs "
          f"invoke_handler (dead permission): {sorted(orphaned_in_build)}")
if ok:
    print(f"  \033[32mPASS\033[0m all {len(handler_cmds)} commands agree across "
          f"lib.rs / build.rs / capabilities/default.json")
else:
    sys.exit(1)
PY
[ $? -ne 0 ] && FAIL=1

# --------------------------------------------------------- 4. JSON well-formed
hdr "Config files"
for j in tauri-app/src-tauri/tauri.conf.json tauri-app/src-tauri/capabilities/default.json tauri-app/package.json; do
  if python3 -c "import json,sys;json.load(open(sys.argv[1]))" "$ROOT/$j" 2>/dev/null; then pass "$j"; else fail "$j (invalid JSON)"; fi
done

hdr "Version consistency (VERSION -> tauri.conf.json / Cargo.toml / package.json)"
if command -v node >/dev/null && [ -f "$ROOT/scripts/sync-version.mjs" ]; then
  if OUT=$(cd "$ROOT" && node scripts/sync-version.mjs --check 2>&1); then
    pass "$OUT"
  else
    fail "version drift detected"; echo "$OUT"
  fi
else
  skip "version consistency (node or scripts/sync-version.mjs not available)"
fi

# ----------------------------------------------- 5. No literal unicode escapes
# A "\uXXXX" sequence inside JSX text renders as those six characters, not the
# glyph. Caught in review once; gate it so it cannot recur.
hdr "JSX text hygiene"
if grep -rn '\\\\u[0-9a-fA-F]\{4\}' "$ROOT/tauri-app/src" --include='*.jsx' >/tmp/esc.txt 2>/dev/null; then
  fail "literal \\uXXXX escapes in JSX (render as text, not glyphs)"; head -5 /tmp/esc.txt
else
  pass "no literal \\uXXXX escapes in JSX"
fi

# ------------------------------------------------------------------ 6. Rust
hdr "Rust shell"
if command -v cargo >/dev/null; then
  if cargo check --manifest-path "$ROOT/tauri-app/src-tauri/Cargo.toml" --quiet >/tmp/rs.log 2>&1; then
    pass "cargo check"
  else
    # The bundled Tauri stack often needs a newer toolchain than a distro
    # rustc provides; that is an environment limitation, not a code defect,
    # so report it honestly rather than as a false failure.
    if grep -qiE 'requires rustc|rust-version|edition20(21|24) is required|feature .* is required|lock file version|next-lockfile-bump' /tmp/rs.log; then
      skip "cargo check (toolchain: $(rustc --version 2>/dev/null))"
      grep -iE 'requires rustc|rust-version|lock file version' /tmp/rs.log | head -3
    else
      fail "cargo check"; tail -25 /tmp/rs.log
    fi
  fi
else
  skip "Rust (cargo not installed)"
fi

hdr "Result"
if [ "$FAIL" = 0 ]; then printf '\033[32mAll executed checks passed.\033[0m\n'; else printf '\033[31mOne or more checks FAILED.\033[0m\n'; fi
exit $FAIL
