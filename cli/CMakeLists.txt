# netpulse-cli (npulse) — see main.cpp's own top-of-file doc comment for the
# full design rationale. One executable, linked against the exact same
# netpulse_core static library the Tauri desktop app and netpulse_tests use
# — no engine logic lives in this directory, only argument parsing and
# terminal rendering.
add_executable(npulse main.cpp)
target_link_libraries(npulse PRIVATE netpulse_core)

# Sidecar naming for Tauri's externalBin bundling convention
# (tauri.conf.json's bundle.externalBin — see tauri-app/src-tauri's own
# config and ARCHITECTURE.md's CLI section): Tauri expects
# "<name>-<target-triple><ext>" and copies that exact file into the
# installer. Producing it under that name directly here means the release
# workflow's "Build CLI sidecar" step needs no separate rename/copy step
# beyond pointing CMake's output directory at the right place — see that
# workflow step for the triple this resolves to per OS/arch.
if(NETPULSE_CLI_SIDECAR_TRIPLE)
  set_target_properties(npulse PROPERTIES
    OUTPUT_NAME "npulse-${NETPULSE_CLI_SIDECAR_TRIPLE}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/sidecar"
  )
endif()
