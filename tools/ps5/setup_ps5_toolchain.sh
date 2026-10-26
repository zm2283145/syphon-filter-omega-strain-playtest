#!/usr/bin/env bash
set -euo pipefail
# Port-only (132ps5c, research/CI.md "PS5"): unpacks the PS5 toolchain into ~/ps5 from a folder of its archives
# (PS5_TOOLS_DIR): ps5-payload-sdk-v0.42.zip (public: github.com/ps5-payload-dev/sdk), zlib-1.3.2.tar.gz and
# ps5-opengl-sdk-1.0.1.tar.gz; ~/ps5/bp must hold ps5-native-app-boilerplate 4f531c4.
T="${PS5_TOOLS_DIR:-/mnt/d/Claude/Smash Melee/ps5-tools}"
cd ~/ps5/bp
mkdir -p .deps/native/zlib
cp "$T/ps5-payload-sdk-v0.42.zip" .deps/native/ps5-payload-sdk.zip
echo '8cfbc7cd5811e719eb4f0c47eea668d3dc7b40bc8ab11c4a5031d40c23ec02da  .deps/native/ps5-payload-sdk.zip' | sha256sum -c
unzip -q -o .deps/native/ps5-payload-sdk.zip -d .deps/native
cp "$T/zlib-1.3.2.tar.gz" .deps/native/zlib/
bash tools/setup-native-dependencies.sh
cd ~/ps5
[ -d ps5-opengl-sdk-1.0.1 ] || tar xzf "$T/ps5-opengl-sdk-1.0.1.tar.gz"
mkdir -p gl
[ -d gl/ps5-opengl ] || tar xf ps5-opengl-sdk-1.0.1/sources/ps5-opengl.tar -C gl
ls ~/ps5 ~/ps5/gl/ps5-opengl ~/ps5/ps5-opengl-sdk-1.0.1/sdk/lib