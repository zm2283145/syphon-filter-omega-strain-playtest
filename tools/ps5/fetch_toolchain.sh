#!/usr/bin/env bash
# Port-only (132ps5d, research/PS5.md "Build", research/CI.md "PS5"): fetches the PS5 toolchain from public, pinned
# GitHub releases and source revisions into one prefix (nothing is vendored in this repository) and builds its two
# host tools:
#
#   <prefix>/ps5-payload-sdk     ps5-payload-dev/sdk v0.45 (GPL-3.0): prospero-lld, the system modules' link stubs,
#                                the C library headers, libc++ / libc++abi / libunwind
#   <prefix>/deps/{include,lib}  cybervisi0n/ps5-mesa 26.2.0-2, ps5-opengnm-psbc v0.0.2, ps5-gl-runtime v0.0.1: EGL and
#                                OpenGL (Mesa over AGC, linked statically into the title)
#   <prefix>/ps5-app-sdk         cybervisi0n/ps5-app-sdk cf59bd99 (GPL-3.0): the native app's crt, the AGC link stub
#                                sources, the PIE linker script, the symbol map, the ps5-native-tool and
#                                libc-prx-builder sources
#   <prefix>/host/bin            ps5-native-tool (links the PIE into eboot.elf, signs SELFs) and libc-prx-builder, built
#                                with the host C++ compiler against zlib 1.3.2 (built here from its pinned release)
#
# usage: fetch_toolchain.sh [prefix]   (default $PS5_TOOLCHAIN, else ~/ps5-app-toolchain)
# Needs curl or wget, unzip, tar, git, make, a host C/C++ compiler (cc, c++ with C++20) and sha256sum. Every archive is
# checked against its sha256 and the source revision against its commit id; a mismatch stops. A second run with the
# same pins does nothing.
set -euo pipefail

prefix=${1:-${PS5_TOOLCHAIN:-$HOME/ps5-app-toolchain}}
mkdir -p "$prefix/downloads"
prefix=$(cd "$prefix" && pwd)
# WSL: a PATH with the Windows folders makes every failed command lookup slow; the build needs only Linux tools.
case $(uname -r) in *[Mm]icrosoft*) PATH=/usr/local/bin:/usr/bin:/bin ;; esac

# name | url | sha256
archives=(
    "ps5-payload-sdk-v0.45.zip|https://github.com/ps5-payload-dev/sdk/releases/download/v0.45/ps5-payload-sdk.zip|5fac64093d0e3e69f2584ce7935f24f703f622d9681f1495c85b9f00d1291a2c"
    "ps5-mesa-26.2.0-2_release.zip|https://github.com/cybervisi0n/ps5-mesa/releases/download/26.2.0-2/ps5-mesa_release.zip|807cef6f4b20a1702bc9432d53eef0f3ec99052e78969d026551c34b195d92a0"
    "ps5-opengnm-psbc-v0.0.2_release.zip|https://github.com/cybervisi0n/ps5-opengnm-psbc/releases/download/v0.0.2/ps5-opengnm-psbc_release.zip|5fbbde2910d88e9c899f0eca39122190ca4606f3effd290e96967f17a6f48a65"
    "ps5-gl-runtime-v0.0.1_release.zip|https://github.com/cybervisi0n/ps5-gl-runtime/releases/download/v0.0.1/ps5-gl-runtime_release.zip|c5e7534607412507791005c5639e7e792a873c323e0cc2270073728b350244ac"
    "zlib-1.3.2.tar.gz|https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.gz|bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16"
)
app_sdk_url=https://github.com/cybervisi0n/ps5-app-sdk
app_sdk_rev=cf59bd995d4d50bdd97f959f366bf0e869d380b2
# The heap the title's process parameters give the system C library (sceLibc's malloc), as the hardware-tested builds
# of 132ps5c (256 MiB; ps5-native-tool writes "unlimited" by default). The title's own 1 GiB heap is
# src/port/ps5/ps5_heap.cpp.
libc_heap_bytes=0x10000000ULL
recipe=3  # bump when the steps below change

fetch() {
    if command -v curl >/dev/null; then curl -fsSL --retry 3 -o "$2" "$1"; else wget -q -O "$2" "$1"; fi
}

stamp="$prefix/.fetched"
want=$(printf '%s\n' "${archives[@]}" "$app_sdk_rev" "$libc_heap_bytes" "$recipe" | sha256sum | cut -d' ' -f1)
if [[ -f $stamp && $(cat "$stamp") == "$want" ]]; then
    echo "PS5 toolchain up to date: $prefix"
    exit 0
fi

for entry in "${archives[@]}"; do
    IFS='|' read -r name url sum <<< "$entry"
    file="$prefix/downloads/$name"
    if [[ ! -f $file ]] || ! echo "$sum  $file" | sha256sum -c --status; then
        echo "fetching $url"
        fetch "$url" "$file.part"
        mv "$file.part" "$file"
    fi
    echo "$sum  $file" | sha256sum -c
done

rm -rf "$prefix/ps5-payload-sdk" "$prefix/deps" "$prefix/host"
mkdir -p "$prefix/deps" "$prefix/host/bin" "$prefix/host/src"
unzip -q -o "$prefix/downloads/ps5-payload-sdk-v0.45.zip" -d "$prefix"
for name in ps5-mesa-26.2.0-2_release.zip ps5-opengnm-psbc-v0.0.2_release.zip ps5-gl-runtime-v0.0.1_release.zip; do
    unzip -q -o "$prefix/downloads/$name" -d "$prefix/deps"
done

if [[ ! -d $prefix/ps5-app-sdk/.git ]]; then
    rm -rf "$prefix/ps5-app-sdk"
    git init -q "$prefix/ps5-app-sdk"
    git -C "$prefix/ps5-app-sdk" remote add origin "$app_sdk_url"
fi
if [[ $(git -C "$prefix/ps5-app-sdk" rev-parse -q --verify HEAD 2>/dev/null || true) != "$app_sdk_rev" ]]; then
    git -C "$prefix/ps5-app-sdk" fetch -q --depth 1 origin "$app_sdk_rev"
    git -C "$prefix/ps5-app-sdk" checkout -q --force "$app_sdk_rev"
fi
git -C "$prefix/ps5-app-sdk" diff --quiet
test "$(git -C "$prefix/ps5-app-sdk" rev-parse HEAD)" = "$app_sdk_rev"

# zlib for the host tools (static; the system's zlib1g-dev is not needed).
tar xzf "$prefix/downloads/zlib-1.3.2.tar.gz" -C "$prefix/host/src"
(cd "$prefix/host/src/zlib-1.3.2" && CFLAGS="-O2 -fPIC" ./configure --static --prefix="$prefix/host" >/dev/null &&
    make -s -j"$(nproc)" install >/dev/null)

# The host tools, from the pinned ps5-app-sdk sources (its tools/*/meson.build: C++20, zlib). The one change is the
# C library heap size above, made in a copy, checked to apply exactly once.
tool_src="$prefix/host/src/ps5-native-tool"
cp -r "$prefix/ps5-app-sdk/tools/ps5-native-tool" "$tool_src"
heap_default='write_u64(result.data, result.heap_size, std::numeric_limits<std::uint64_t>::max());'
test "$(grep -Fc "$heap_default" "$tool_src/sce_module_writer.cpp")" = 1
sed -i "s/$heap_default/write_u64(result.data, result.heap_size, $libc_heap_bytes);/" "$tool_src/sce_module_writer.cpp"
c++ -std=c++20 -O2 -I"$prefix/host/include" "$tool_src"/*.cpp "$prefix/host/lib/libz.a" -o "$prefix/host/bin/ps5-native-tool"
c++ -std=c++20 -O2 -I"$prefix/host/include" "$prefix/ps5-app-sdk/tools/libc-prx-builder/libc_builder.cpp" \
    "$prefix/host/lib/libz.a" -o "$prefix/host/bin/libc-prx-builder"

test -s "$prefix/ps5-payload-sdk/bin/prospero-lld"
test -s "$prefix/deps/lib/libps5_opengl_core33.a"
"$prefix/host/bin/ps5-native-tool" 2>/dev/null | head -n 1 || true
echo "$want" > "$stamp"
echo "PS5 toolchain: $prefix"
