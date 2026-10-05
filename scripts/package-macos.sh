#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 ameisevinyl
# Packages a universal macOS build into dist/PitchDelay-<version>-macOS.zip (+ .sha256).
# Ad-hoc signs the bundles and checks that every binary contains Intel and Apple Silicon code.
# Usage: scripts/package-macos.sh <build-dir> <version>
set -euo pipefail

BUILD="${1:?usage: package-macos.sh <build-dir> <version>}"
VERSION="${2:?usage: package-macos.sh <build-dir> <version>}"
NAME="PitchDelay-$VERSION-macOS"
OUT="$PWD/dist"
STAGE="$(mktemp -d)/$NAME"

find_bundle() { find "$BUILD" -maxdepth 4 -name "$1" -print -quit; }
AU="$(find_bundle PitchDelay.component)"
VST3="$(find_bundle PitchDelay.vst3)"
APP="$(find_bundle PitchDelay.app)"
[[ -n "$AU" && -n "$VST3" && -n "$APP" ]] || { echo "bundles not found under $BUILD" >&2; exit 1; }

mkdir -p "$STAGE" "$OUT"
ditto "$AU"   "$STAGE/PitchDelay.component"
ditto "$VST3" "$STAGE/PitchDelay.vst3"
ditto "$APP"  "$STAGE/PitchDelay.app"
cp LICENSE packaging/INSTALL.txt "$STAGE/"

for bundle in "$STAGE"/PitchDelay.component "$STAGE"/PitchDelay.vst3 "$STAGE"/PitchDelay.app; do
    codesign --force --deep --sign - "$bundle"
    codesign --verify --deep --strict "$bundle"
    for exe in "$bundle"/Contents/MacOS/*; do
        archs="$(lipo -archs "$exe")"
        echo "$(basename "$bundle"): $archs"
        [[ " $archs " == *" x86_64 "* && " $archs " == *" arm64 "* ]] \
            || { echo "$exe is not universal ($archs)" >&2; exit 1; }
    done
done

rm -f "$OUT/$NAME.zip" "$OUT/$NAME.zip.sha256"
ditto -c -k --keepParent "$STAGE" "$OUT/$NAME.zip"
(cd "$OUT" && shasum -a 256 "$NAME.zip" > "$NAME.zip.sha256")
echo "wrote $OUT/$NAME.zip"
cat "$OUT/$NAME.zip.sha256"
