#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 ameisevinyl
# Packages a Linux or Windows build into dist/PitchDelay-<version>-<platform>.zip (+ .sha256).
# Usage: scripts/package-portable.sh <build-dir> <version> <linux-x64|windows-x64>
set -euo pipefail

BUILD="${1:?usage: package-portable.sh <build-dir> <version> <linux-x64|windows-x64>}"
VERSION="${2:?usage: package-portable.sh <build-dir> <version> <linux-x64|windows-x64>}"
PLATFORM="${3:?usage: package-portable.sh <build-dir> <version> <linux-x64|windows-x64>}"

case "$PLATFORM" in
    linux-x64)   STANDALONE="PitchDelay";     INSTALL="INSTALL-linux.txt" ;;
    windows-x64) STANDALONE="PitchDelay.exe"; INSTALL="INSTALL-windows.txt" ;;
    *) echo "unknown platform: $PLATFORM" >&2; exit 1 ;;
esac

NAME="PitchDelay-$VERSION-$PLATFORM"
OUT="$PWD/dist"
STAGE="$(mktemp -d)/$NAME"

VST3="$(find "$BUILD" -maxdepth 4 -name PitchDelay.vst3 -print -quit)"
EXE="$(find "$BUILD" -maxdepth 4 -type f -name "$STANDALONE" -path '*Standalone*' -print -quit)"
[[ -n "$VST3" && -n "$EXE" ]] || { echo "VST3 or standalone not found under $BUILD" >&2; exit 1; }

mkdir -p "$STAGE" "$OUT"
cp -R "$VST3" "$STAGE/PitchDelay.vst3"
cp "$EXE" "$STAGE/$STANDALONE"
cp LICENSE "$STAGE/LICENSE"
cp "packaging/$INSTALL" "$STAGE/INSTALL.txt"

# The VST3 bundle must contain its binary for this platform.
case "$PLATFORM" in
    linux-x64)   BIN="$STAGE/PitchDelay.vst3/Contents/x86_64-linux/PitchDelay.so" ;;
    windows-x64) BIN="$STAGE/PitchDelay.vst3/Contents/x86_64-win/PitchDelay.vst3" ;;
esac
[[ -f "$BIN" ]] || { echo "missing $BIN" >&2; find "$STAGE" | head -20 >&2; exit 1; }

rm -f "$OUT/$NAME.zip" "$OUT/$NAME.zip.sha256"
(cd "$(dirname "$STAGE")" && cmake -E tar cf "$OUT/$NAME.zip" --format=zip "$NAME")
(cd "$OUT" && sha256sum "$NAME.zip" > "$NAME.zip.sha256")
echo "wrote $OUT/$NAME.zip"
cat "$OUT/$NAME.zip.sha256"
