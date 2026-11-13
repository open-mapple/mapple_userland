#!/bin/sh
# The plug-in's property answers, volume and I/O on this Mac (audio/plugin_test.c).
set -e
cd "$(dirname "$0")"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cc -O2 -Wall -Wno-four-char-constants -framework CoreFoundation plugin_test.c -o "$tmp/plugin_test"
"$tmp/plugin_test"
