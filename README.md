# pitchdelay

An exact delay plugin (AU / VST3) for the drive channel of a record-cutting lathe. It delays audio by a
fraction of a platter revolution, so the pitch control can look ahead of the cut. Default is half a
revolution: 0.9 s at 33⅓ RPM, ⅔ s at 45 RPM ([background](https://flokason.ch/pitch13_manual.html)).

- 100 % wet and bit-identical, delayed by a whole number of sample frames
- Reports **0 latency**: the DAW does not compensate, the delayed path is late by exactly N frames
- 33⅓ / 45 RPM, fraction 1/1 … 1/16 of a revolution, fine-tuning in samples or milliseconds
- **Calibration**: a 1 kHz tone burst every N frames, for measuring the real platter speed
- Stopping the transport clears the buffer; bypass passes the audio through undelayed

## Use

Route the track twice: one path untouched to the pitch output, one path through PitchDelay to the drive
output. Choose speed and fraction, fine-tune if needed. All keys stay with the host's transport.

## Install (macOS, Intel and Apple Silicon)

Download the zip from [Releases](https://github.com/ameisevinyl/pitchdelay/releases), copy
`PitchDelay.component` to `~/Library/Audio/Plug-Ins/Components/` and/or `PitchDelay.vst3` to
`~/Library/Audio/Plug-Ins/VST3/`, then remove the quarantine flag (the build is signed ad hoc, not
notarized): `xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/Components/PitchDelay.component`.

## Build

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build
    ctest --test-dir build --output-on-failure
    scripts/validate.sh build        # pluginval + auval

Needs Xcode command line tools, CMake ≥ 3.22 and Ninja. Plugins are copied to
`~/Library/Audio/Plug-Ins/` unless `-DPITCHDELAY_INSTALL_AFTER_BUILD=OFF`;
`-DPITCHDELAY_UNIVERSAL=ON` builds for Intel and Apple Silicon.

## Licence

Copyright (C) 2026 ameisevinyl. [AGPL-3.0-only](LICENSE), built with [JUCE](https://juce.com) (AGPLv3).
VST is a trademark of Steinberg Media Technologies GmbH.
