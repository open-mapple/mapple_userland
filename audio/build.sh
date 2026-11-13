#!/bin/bash
# Builds MappleAudio.driver (audio_plugin.c, the audio server's plug-in) for the phone: iOS 16 arm64, ad-hoc signed.
# It goes to /System/Library/Audio/Plug-Ins/HAL/ on the root (build/mapple tools/root-tweaks.py).
# Usage: audio/build.sh [OUT_DIR]   (default audio/out; the bundle is OUT_DIR/MappleAudio.driver)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$HERE/out}"
B="$OUT/MappleAudio.driver"
rm -rf "$B"
mkdir -p "$B"
cp "$HERE/audio_plugin.plist" "$B/Info.plist"
xcrun --sdk iphoneos clang -target arm64-apple-ios16.0 -O2 -Wall -Wno-four-char-constants -bundle \
	-framework CoreFoundation -o "$B/MappleAudio" "$HERE/audio_plugin.c"
codesign -s - -f "$B"
file "$B/MappleAudio"
