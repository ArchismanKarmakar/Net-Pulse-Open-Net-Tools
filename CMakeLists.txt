cmake_minimum_required(VERSION 3.16)

# Must be set BEFORE project() to take effect — CMake reads this during
# compiler/platform detection triggered by project(), not after. Same root
# cause as tauri-app/src-tauri/build.rs's identical fix: core/src/stats.cpp
# uses std::filesystem (ColdStore's on-disk persistence), which Apple's
# libc++ marks explicitly UNAVAILABLE below macOS 10.15 (Catalina) — without
# an explicit deployment target, CMake/Xcode can fall back to a much older
# default that doesn't support it, turning every std::filesystem use into a
# hard compile error rather than a warning. if(NOT DEFINED ...) so an
# explicit -DCMAKE_OSX_DEPLOYMENT_TARGET=... from the command line (e.g. a
# packager targeting something newer) is never silently overridden.
if(APPLE AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Minimum macOS version")
endif()

project(netpulse_core LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# NETPULSE_OBFUSCATE — second, hardened build variant.
#
# OFF (default): the normal, easy-to-debug build everyone should develop and
# test against day to day.
#
# ON: compiles the same source with NETPULSE_OBFUSCATE defined, which turns on
# core/include/netpulse/obfuscate.hpp's source-level obfuscation (compile-time
# string XOR-encryption + opaque-predicate branches on selected validation
# checks). This works with ANY C++20 compiler (MSVC, GCC, Clang) — no special
# toolchain required. An out-of-tree LLVM pass-plugin approach was attempted
# and abandoned; see docs/OBFUSCATED_BUILD.md for why. Behavior-preserving:
# the same tests/test_core.cpp suite must pass in both variants.
option(NETPULSE_OBFUSCATE "Build with source-level obfuscation (string encryption + opaque predicates)" OFF)


file(GLOB CORE_SRC core/src/*.cpp)
add_library(netpulse_core STATIC ${CORE_SRC})
target_include_directories(netpulse_core PUBLIC core/include)
if(WIN32)
  target_compile_definitions(netpulse_core PUBLIC NOMINMAX)
  target_link_libraries(netpulse_core PUBLIC ws2_32 iphlpapi winmm)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(netpulse_core PUBLIC Threads::Threads)
endif()

if(NETPULSE_OBFUSCATE)
  target_compile_definitions(netpulse_core PRIVATE NETPULSE_OBFUSCATE)
endif()

# Hardening that's real today regardless of NETPULSE_OBFUSCATE — doesn't need
# a special toolchain, doesn't change behavior, applies to both build variants.
if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(netpulse_core PRIVATE -ffunction-sections -fdata-sections -fvisibility=hidden -fvisibility-inlines-hidden)
    if(APPLE)
      target_link_options(netpulse_core PRIVATE -Wl,-dead_strip)
    else()
      target_link_options(netpulse_core PRIVATE -Wl,--gc-sections)
    endif()
  endif()
endif()


enable_testing()
add_executable(netpulse_tests tests/test_core.cpp)
target_link_libraries(netpulse_tests PRIVATE netpulse_core)
add_test(NAME core COMMAND netpulse_tests)
