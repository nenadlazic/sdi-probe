#!/usr/bin/env bash
#
# Build sdi-probe against a Blackmagic DeckLink SDK.
#
#   ./build.sh                        use the SDK named in the vendor dir's ACTIVE file
#   SDK_VERSION=11_5_1 ./build.sh     pick another vendored SDK by folder suffix
#   SDK=/opt/decklink-sdk ./build.sh  use an SDK from anywhere (must hold DeckLinkAPI.h)
#   VENDOR_DIR=/path ./build.sh       look for DeckLink_SDK_* folders somewhere else
#
# Why the binary is not fully static:
#   libDeckLinkAPI.so is loaded at runtime with dlopen() by DeckLinkAPIDispatch.cpp, and a
#   fully static glibc binary cannot dlopen. So -static is not an option. What we do link
#   statically is libstdc++/libgcc, which is what actually breaks portability across
#   distributions (the C++ ABI). The result depends on glibc only, so build it on the
#   oldest OS you need to support and it will run on newer ones.
#
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
# Default: a "sdk" directory next to this script, holding either the SDK itself or one or
# more DeckLink_SDK_* folders plus an ACTIVE file naming the one to use.
VENDOR_DIR="${VENDOR_DIR:-$HERE/sdk}"
OUTPUT="${OUTPUT:-$HERE/sdi-probe}"

list_available_sdks() {
    [ -d "$VENDOR_DIR" ] || return 0
    local found
    found=$(find "$VENDOR_DIR" -maxdepth 1 -type d -name 'DeckLink_SDK_*' -printf '  SDK_VERSION=%f\n' \
        | sed 's/DeckLink_SDK_//')
    if [ -n "$found" ]; then
        echo "Available in $VENDOR_DIR:"
        echo "$found"
    else
        echo "No DeckLink_SDK_* folder in $VENDOR_DIR yet."
    fi
}

# Resolve which SDK to build against: explicit path wins, then explicit version,
# then whatever the vendor dir's ACTIVE file points at.
if [ -n "${SDK:-}" ]; then
    SDK_DIR="$SDK"
    SDK_SOURCE="SDK env var"
elif [ -n "${SDK_VERSION:-}" ]; then
    SDK_DIR="$VENDOR_DIR/DeckLink_SDK_${SDK_VERSION}"
    SDK_SOURCE="SDK_VERSION env var"
elif [ -f "$VENDOR_DIR/ACTIVE" ]; then
    SDK_DIR="$VENDOR_DIR/$(tr -d '[:space:]' < "$VENDOR_DIR/ACTIVE")"
    SDK_SOURCE="ACTIVE file"
else
    echo "ERROR: no SDK selected and no ACTIVE file in $VENDOR_DIR" >&2
    echo "       Unpack the DeckLink SDK into $VENDOR_DIR (see $VENDOR_DIR/README.md)," >&2
    echo "       then name the folder in $VENDOR_DIR/ACTIVE, or pass one explicitly:" >&2
    echo "         SDK_VERSION=<suffix> $0" >&2
    echo "         SDK=<path> $0" >&2
    list_available_sdks >&2
    exit 1
fi

if [ ! -f "$SDK_DIR/DeckLinkAPI.h" ] || [ ! -f "$SDK_DIR/DeckLinkAPIDispatch.cpp" ]; then
    echo "ERROR: $SDK_DIR does not look like a DeckLink SDK" >&2
    echo "       (needs DeckLinkAPI.h and DeckLinkAPIDispatch.cpp)" >&2
    echo "       Selected via $SDK_SOURCE; change it there, or unpack the SDK" >&2
    echo "       into $VENDOR_DIR (see $VENDOR_DIR/README.md)." >&2
    list_available_sdks >&2
    exit 1
fi

# BLACKMAGIC_DECKLINK_API_VERSION is a packed hex constant, 0x0c040100 meaning 12.4.1.
sdk_version_string() {
    local hex
    hex=$(grep -oE '0x[0-9a-fA-F]{8}' "$SDK_DIR/DeckLinkAPIVersion.h" | head -1)
    [ -n "$hex" ] || { echo "unknown"; return; }
    printf '%d.%d.%d\n' $(( (hex >> 24) & 0xFF )) $(( (hex >> 16) & 0xFF )) $(( (hex >> 8) & 0xFF ))
}

echo "SDK      : $SDK_DIR"
echo "version  : $(sdk_version_string)   (selected via $SDK_SOURCE)"
echo "compiler : $(g++ --version | head -1)"
echo "to change: edit $VENDOR_DIR/ACTIVE, or run  SDK_VERSION=<suffix> $0  |  SDK=<path> $0"

g++ -std=c++17 -O2 -Wall -Wextra \
    -I"$SDK_DIR" \
    "$HERE/sdi_probe.cpp" "$SDK_DIR/DeckLinkAPIDispatch.cpp" \
    -o "$OUTPUT" \
    -static-libstdc++ -static-libgcc \
    -ldl -lpthread

echo
echo "built    : $OUTPUT"
ldd "$OUTPUT" | sed 's/^/  /'
