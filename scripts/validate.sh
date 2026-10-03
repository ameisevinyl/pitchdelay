#!/usr/bin/env bash
# Validate the built AU and VST3 with pluginval, and the AU with auval.
# Usage: scripts/validate.sh [build-dir]    (env: PLUGINVAL, CI)
set -euo pipefail

BUILD="${1:-build}"
PLUGINVAL="${PLUGINVAL:-/Applications/pluginval.app/Contents/MacOS/pluginval}"
GUI_FLAG=()
[[ -n "${CI:-}" ]] && GUI_FLAG=(--skip-gui-tests)

VST3="$(find "$BUILD" -maxdepth 4 -name PitchDelay.vst3 -print -quit)"
AU="$(find "$BUILD" -maxdepth 4 -name PitchDelay.component -print -quit)"
[[ -n "$VST3" && -n "$AU" ]] || { echo "plugins not found under $BUILD" >&2; exit 1; }

echo "== pluginval VST3: $VST3"
"$PLUGINVAL" --strictness-level 10 --validate-in-process ${GUI_FLAG[@]+"${GUI_FLAG[@]}"} "$VST3"
echo "== pluginval AU: $AU"
"$PLUGINVAL" --strictness-level 10 --validate-in-process ${GUI_FLAG[@]+"${GUI_FLAG[@]}"} "$AU"

echo "== auval"
DEST="$HOME/Library/Audio/Plug-Ins/Components"
mkdir -p "$DEST"
rm -rf "$DEST/PitchDelay.component"
cp -R "$AU" "$DEST/"
killall -9 AudioComponentRegistrar 2>/dev/null || true
auval -v aufx Pdly Amvl
echo "ALL VALIDATION PASSED"
