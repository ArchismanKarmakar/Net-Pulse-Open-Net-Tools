{
  "targets": [
    {
      "target_name": "netpulse",
      "sources": [
        "napi.cpp",
        "../core/src/icmp.cpp",
        "../core/src/stats.cpp",
        "../core/src/transport.cpp",
        "../core/src/session.cpp"
      ],
      "include_dirs": [
        "../core/include",
        "<!@(node -p \"require('node-addon-api').include\")"
      ],
      "defines": [ "NAPI_CPP_EXCEPTIONS", "NAPI_VERSION=8" ],
      "cflags_cc": [ "-std=c++17", "-fexceptions" ],
      "cflags_cc!": [ "-fno-exceptions" ],
      "conditions": [
        [ "OS=='linux'", {
          "cflags_cc": [ "-std=c++17" ],
          "libraries": [ "-lpthread" ]
        } ],
        [ "OS=='mac'", {
          "xcode_settings": {
            "CLANG_CXX_LANGUAGE_STANDARD": "c++17",
            "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
            "MACOSX_DEPLOYMENT_TARGET": "10.15"
          }
        } ],
        [ "OS=='win'", {
          "msvs_settings": {
            "VCCLCompilerTool": {
              "ExceptionHandling": 1,
              "AdditionalOptions": [ "/std:c++17" ]
            }
          },
          "defines": [ "NOMINMAX" ],
          "libraries": [ "Ws2_32.lib", "Iphlpapi.lib", "Winmm.lib" ]
        } ]
      ]
    }
  ]
}
