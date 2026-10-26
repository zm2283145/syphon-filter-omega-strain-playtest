#!/bin/sh
# Port-only (132ps5c, research/PS5.md "Build"): the C++ compiler CMake uses for the PS5 target. It runs the native app
# boilerplate's prospero-clang18 wrapper (clang 18, target x86_64-sie-ps5, the payload SDK's headers and libc++) with
# the SDK paths defaulted to the WSL toolchain install (tools/ps5/setup_ps5_toolchain.sh).
: "${PS5_NATIVE_APP_TEMPLATE:=$HOME/ps5/bp}"
: "${PS5_PAYLOAD_SDK:=$PS5_NATIVE_APP_TEMPLATE/.deps/native/ps5-payload-sdk}"
export PS5_NATIVE_APP_TEMPLATE PS5_PAYLOAD_SDK
exec sh "$PS5_NATIVE_APP_TEMPLATE/tooling/prospero-clang18" "$@"