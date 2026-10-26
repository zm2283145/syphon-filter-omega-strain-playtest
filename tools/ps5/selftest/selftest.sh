#!/usr/bin/env bash
set -euo pipefail
export PS5_NATIVE_APP_TEMPLATE=~/ps5/bp PS5_OPENGL_PREFIX=~/ps5/ps5-opengl-sdk-1.0.1/sdk PS5_OPENGL_SRC=~/ps5/gl/ps5-opengl
export PS5_PAYLOAD_SDK=$PS5_NATIVE_APP_TEMPLATE/.deps/native/ps5-payload-sdk
w=$(cd "$(dirname "$0")" && pwd)
o=~/ps5work/selftest; mkdir -p $o
sh $PS5_NATIVE_APP_TEMPLATE/tooling/prospero-clang18 -x c++ -std=c++20 -O2 -fPIC -fexceptions -frtti -c "$w/cxx_selftest.cpp" -o $o/selftest.o
sh $PS5_NATIVE_APP_TEMPLATE/tooling/prospero-clang18 -std=c11 -O2 -fPIC -c "$w/shim.c" -o $o/shim.o
bash "$w/mkapp.sh" $o/app PPSA99721 "SFOS C++ self-test" $o/selftest.o $o/shim.o 2>&1 | tail -30