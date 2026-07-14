// obfuscate.hpp — source-level obfuscation, no special toolchain required.
//
// This project originally tried to also ship a real LLVM new-PM pass plugin
// (StringEncryptionPass/BogusControlFlowPass) as a second, IR-level layer on
// top of this header — see docs/OBFUSCATED_BUILD.md for why that was
// abandoned: every prebuilt clang.exe available in this environment (winget's
// LLVM.LLVM, and VS 2026's bundled VC/Tools/Llvm) crashes with heap
// corruption when loading a plugin linked against conda-forge's llvmdev
// libs, even at matching major.minor.patch — the two are separately built
// LLVM copies and aren't ABI-compatible despite the shared version number.
// Building LLVM itself from source to get a guaranteed-matching toolchain is
// the usual fix, but that's a multi-hour undertaking not worth it for this
// project. So this header is the *entire* obfuscation mechanism now, not a
// fallback for when the plugin is unavailable — it is unconditionally what
// NETPULSE_OBFUSCATE/`obfuscate` turns on.
//
// Two tools:
//   NETPULSE_OBF_STR(s)   — compile-time XOR-obfuscates a string literal,
//                            decrypts into a thread-local buffer on first use
//                            per thread. Apply to real embedded constants
//                            (build tags, protocol sentinels) — never to
//                            functional output (JSON keys, user-supplied
//                            hostnames) since those must stay exactly as
//                            written to not break behavior.
//   NETPULSE_OPAQUE(cond) — wraps a boolean expression so it's evaluated
//                            behind an always-true runtime branch that isn't
//                            a compile-time constant, in the spirit of
//                            classic "bogus control flow" obfuscation:
//                            extra branches that always converge to the same
//                            behavior, raising the cost of static CFG
//                            reading without changing it.
//
// Both are compiled in unconditionally when NETPULSE_OBFUSCATE (CMake) /
// `obfuscate` (Cargo feature) is on; with it off, NETPULSE_OBF_STR(s)
// reduces to the plain literal `s` and NETPULSE_OPAQUE(cond) reduces to
// `(cond)` — zero overhead, zero behavior change, verified by the exact same
// tests/test_core.cpp suite in both configurations.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#if defined(NETPULSE_OBFUSCATE)

namespace netpulse::obfuscate {

// A structural NTTP wrapper around a string literal (C++20 allows class-type
// non-type template parameters when every member is public, which is what
// makes it legal to pass a string literal's contents as a template argument
// here — this is what lets NETPULSE_OBF_STR encrypt at compile time with no
// runtime cost for the encryption step itself).
template <std::size_t N>
struct ObfLiteral {
    char data[N]{};
    constexpr ObfLiteral(const char (&s)[N]) {
        for (std::size_t i = 0; i < N; ++i) data[i] = s[i];
    }
};

template <ObfLiteral Lit>
class ObfString {
    static constexpr std::uint8_t key(std::size_t i) {
        // Per-byte, per-string key derived from the string's own length and
        // position — not a fixed global key, so two different strings don't
        // share ciphertext structure. Not cryptography; just raises the cost
        // of a naive "search the binary for XOR 0x5A" pass.
        return static_cast<std::uint8_t>(0x5A ^ (sizeof(Lit.data) * 31u) ^ (i * 17u));
    }

    static constexpr auto encrypted() {
        std::array<char, sizeof(Lit.data)> out{};
        for (std::size_t i = 0; i < sizeof(Lit.data); ++i) {
            out[i] = static_cast<char>(static_cast<std::uint8_t>(Lit.data[i]) ^ key(i));
        }
        return out;
    }

public:
    static const char* decrypt() {
        static constexpr auto kEncrypted = encrypted();
        thread_local char buf[sizeof(Lit.data)];
        thread_local bool ready = false;
        if (!ready) {
            for (std::size_t i = 0; i < sizeof(Lit.data); ++i) {
                buf[i] = static_cast<char>(static_cast<std::uint8_t>(kEncrypted[i]) ^ key(i));
            }
            ready = true;
        }
        return buf;
    }
};

// y*(y+1) is always even for any integer y — one of two consecutive integers
// is necessarily even. Seeded from a static object's own runtime address (not
// knowable at compile time, so an optimizer can't constant-fold this into a
// literal `true`) rather than a fixed number, so it isn't recognizable as one
// specific magic constant either.
inline bool opaque_true() {
    static const int seed_obj = 0;
    long y = static_cast<long>(reinterpret_cast<std::uintptr_t>(&seed_obj) & 0xFFFF);
    return ((y * (y + 1)) % 2) == 0;
}

} // namespace netpulse::obfuscate

#define NETPULSE_OBF_STR(s) (::netpulse::obfuscate::ObfString<::netpulse::obfuscate::ObfLiteral(s)>::decrypt())
// opaque_true() is always true but not foldable at compile time, so ANDing
// it in front of a real condition inserts a genuine extra branch (evaluated
// left-to-right, short-circuited) without ever changing the result.
#define NETPULSE_OPAQUE(cond) (::netpulse::obfuscate::opaque_true() && static_cast<bool>(cond))

#else

#define NETPULSE_OBF_STR(s) (s)
#define NETPULSE_OPAQUE(cond) (cond)

#endif // NETPULSE_OBFUSCATE
