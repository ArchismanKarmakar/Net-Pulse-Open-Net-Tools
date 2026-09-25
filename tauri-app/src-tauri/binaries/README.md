# tauri-app/src-tauri/binaries/

Empty in a fresh checkout. Populated at CI build time (and optionally by a
local developer building the CLI themselves — see below) with the
`netpulse-cli`/`npulse` sidecar binary, named per Tauri's `externalBin`
convention: `npulse-<target-triple>` (`.exe` appended on Windows), e.g.
`npulse-x86_64-pc-windows-msvc.exe`, `npulse-aarch64-apple-darwin`,
`npulse-x86_64-unknown-linux-gnu`.

`tauri.conf.json`'s `bundle.externalBin: ["binaries/npulse"]` tells Tauri to
look here (Tauri itself appends the `-<target-triple>` suffix when
resolving which exact file to bundle) and package whatever it finds
alongside the main app in every installer it builds.

## Building the sidecar locally

From the repository root:

```sh
TRIPLE=$(rustc -vV | sed -n 's/^host: //p')
cmake -S . -B build-cli -DCMAKE_BUILD_TYPE=Release -DNETPULSE_CLI_SIDECAR_TRIPLE="$TRIPLE"
cmake --build build-cli --config Release --target npulse
cp "$(find build-cli -name "npulse-${TRIPLE}*" -type f)" tauri-app/src-tauri/binaries/
```

Then `tauri build`/`tauri dev` from `tauri-app/` will pick it up
automatically. See `.github/workflows/tauri-release.yml`'s "Build CLI
sidecar" step for the exact same sequence CI runs per OS/architecture.
