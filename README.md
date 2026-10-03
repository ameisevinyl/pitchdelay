# pitchdelay

An exact-frame, 100% wet, bit-transparent delay plugin (AU / VST3 / Standalone) for the
drive channel of a record-cutting lathe: the drive path is delayed by a half lathe
revolution (0.9 s at 33⅓ RPM, 2/3 s at 45 RPM) relative to the undelayed pitch (preview)
path. The plugin reports **0 latency**, so the DAW does not compensate; the delayed path is
late by exactly N sample frames.

Background: https://flokason.ch/pitch13_manual.html
Design: `docs/superpowers/specs/2026-10-03-pitchdelay-design.md`

## Use (Logic / any DAW)
Route the track twice: one path untouched → pitch output; one path through PitchDelay → drive
output. Pick 33⅓ RPM or 45 RPM (or Custom) before cutting. Bypass passes audio undelayed.

## Build
Requires Xcode command line tools, CMake ≥ 3.22, Ninja.

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build
    ctest --test-dir build --output-on-failure
    scripts/validate.sh build

Plugins are copied to `~/Library/Audio/Plug-Ins/` unless `-DPITCHDELAY_INSTALL_AFTER_BUILD=OFF`.
`-DPITCHDELAY_UNIVERSAL=ON` builds arm64 + x86_64.

## Licence
AGPLv3 (see `LICENSE`). Built with [JUCE](https://juce.com) (AGPLv3 / commercial).
VST is a trademark of Steinberg Media Technologies GmbH.
