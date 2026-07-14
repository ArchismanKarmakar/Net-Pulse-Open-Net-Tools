# Obfuscated build (second build variant)

This project has **two** build configurations for the C++ engine:

1. **Normal** (default) — what you develop and test against day to day.
2. **Obfuscated** (`-DNETPULSE_OBFUSCATE=ON` for CMake, `--features obfuscate`
   for the Tauri/Rust side) — the same source, compiled with
   `NETPULSE_OBFUSCATE` defined, which turns on
   [`core/include/netpulse/obfuscate.hpp`](../core/include/netpulse/obfuscate.hpp)'s
   source-level obfuscation: compile-time string encryption and opaque-predicate
   branches, meant to raise the cost of casual static/binary analysis of the
   shipped engine.

Read [`SECURITY.md`](../SECURITY.md)'s "Reverse engineering / IP protection"
section first. Short version: this raises attacker *cost*, it does not make
the binary unreadable to a determined reverse engineer — no client-side
technique can do that. It's a legitimate, real technique (used in commercial
anti-tamper/DRM); it's just not a guarantee, and this project's own
AGPL-3.0-or-later license means the exact matching source is public anyway —
that's the whole reason this project doesn't chase a heavier toolchain
dependency to squeeze out marginally more protection (see below).

## What this actually does

Two macros, defined unconditionally when `NETPULSE_OBFUSCATE` is on and
reducing to zero-cost passthroughs when it's off:

- **`NETPULSE_OBF_STR(s)`** — encrypts a string literal at compile time with a
  per-string XOR key (derived from the string's own length and byte position,
  not one fixed global key), and decrypts it into a `thread_local` buffer on
  first use per thread. Applied to real embedded constants worth hiding from a
  casual `strings`/binary-diff pass — e.g. the engine build tag, protocol
  sentinels — **never** to functional output (JSON field names, user-supplied
  hostnames), since those have to stay byte-exact to not break behavior.
- **`NETPULSE_OPAQUE(cond)`** — wraps a boolean expression behind a
  runtime-opaque, always-true predicate (`y*(y+1)` is always even, seeded from
  a static object's own address so it isn't compile-time foldable), in the
  spirit of classic "bogus control flow" obfuscation: a genuine extra branch
  that always converges to the same result, raising the cost of reading the
  control-flow graph statically without changing what it does. Applied to a
  handful of input-validation checks in `netpulse_ffi.cpp`.

Both are pure C++20 (structural non-type template parameters for the
compile-time string encryption) — **no special compiler is required**. MSVC,
GCC, and mainline Clang all work identically; this is the entire point of the
design.

## What I tried first, and why it's not what shipped

An earlier version of this document (and this project's original plan)
described obfuscation via a real **out-of-tree LLVM new-PM pass plugin** —
`-fpass-plugin=...` implementing `StringEncryptionPass`/`BogusControlFlowPass`
at the IR level, as a second layer on top of the source-level header, mirroring
what tools like Hikari/obfuscator-llvm do. I built this for real in this
environment (not just planned it) to see if it was viable:

- Built a minimal pass plugin against conda-forge's `llvmdev` package,
  correctly registered via `llvmGetPassPluginInfo()` with `__declspec(dllexport)`
  (Windows PE/COFF doesn't export `extern "C"` symbols the way ELF does — a
  gap the header's own forward declaration doesn't warn about).
- Fixed a `RuntimeLibrary` CRT-linkage mismatch (`/MT` vs `/MD`) using clang's
  `-fms-runtime-lib=dll` flag to match conda's `/MD`-built LLVM.
- Got the correct, complete link line via `llvm-config.exe --libs core support
  passes analysis` rather than hand-guessing library names/order, plus
  `zdll`/`zstd` for LLVM's compression support.
- Result: the plugin built and linked cleanly, but **crashed with
  `STATUS_HEAP_CORRUPTION` (exit code `-1073740940` / `0xC0000374`) the moment
  any clang.exe tried to load it** — reproduced against both winget's
  `LLVM.LLVM` package and, at the user's request, Visual Studio 2026's own
  bundled Clang (`VC/Tools/Llvm`, v22.1.3). Both are separately-built copies of
  LLVM from conda-forge's `llvmdev`, and even at matching major.minor.patch
  version strings, **prebuilt LLVM copies from different build pipelines are
  not ABI-compatible** — global state (pass registries, `cl::opt` globals,
  etc.) collides between the two copies loaded into one process.

The standard fix for this class of problem is building LLVM/Clang from source
so the plugin and the compiler share one exact build — a multi-hour,
several-GB undertaking. For this project, that cost isn't justified: the app
is open-source (AGPL-3.0-or-later) already, so binary obfuscation is a
"raise the bar" measure, not a security boundary, and the source-level header
above delivers the same category of protection (string hiding,
CFG-reading friction) with zero toolchain risk. So the pass-plugin path was
abandoned in favor of source-level-only obfuscation, which is what actually
ships.

## Setting it up

```bash
cmake -S . -B build-obf -DCMAKE_BUILD_TYPE=Release -DNETPULSE_OBFUSCATE=ON
cmake --build build-obf --config Release
ctest --test-dir build-obf -C Release --output-on-failure
```

No special `CMAKE_CXX_COMPILER` is needed — this works with whatever compiler
you'd normally use (MSVC `cl.exe`, GCC, or Clang).

For the Tauri app, `cargo build --features obfuscate` in `tauri-app/src-tauri/`
does the equivalent: `build.rs` defines `NETPULSE_OBFUSCATE` when compiling the
C++ engine via `cxx_build`, using whatever `CXX`/host compiler is already in
use for the rest of the build.

## Acceptance bar

The obfuscated binary must pass the **exact same** `tests/test_core.cpp` suite
as the normal build, unmodified — obfuscation is required to be
behavior-preserving. If a test only passes on one variant, that's a bug in the
obfuscation header (or a call site it was applied to), not something to
special-case away.
