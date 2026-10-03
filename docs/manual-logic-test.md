# Manual Logic test: exact offset between the pitch and drive paths

Goal: confirm Logic does not compensate the plugin and the delayed path is late by exactly N frames.

1. `python3 tools/make_impulse.py ~/Desktop/impulse.wav` (48 kHz, 32-bit float, impulse at frame 48000).
2. Logic: new project at **48 kHz**. Add two audio tracks, both with `impulse.wav` starting at bar 1
   (position exactly 1 1 1 1, no snapping offset).
3. Track "Pitch": no plugins. Track "Drive": insert **PitchDelay** (AU), Mode = 33⅓ RPM
   (readout must show `Delay: 43200 frames = 900.000 ms`).
4. Logic ▸ Settings ▸ Audio ▸ General: note the Plug-in Delay Compensation setting (try both *All* and
   *Audio and Software Instrument*). The result must not depend on it.
5. File ▸ Export ▸ All Tracks as Audio Files. WAV, **32-bit float** (or 24-bit), Normalize **Off**,
   Bypass Effect Plug-ins **Off**, **Include Audio Tail on**. Results: `Pitch.wav`, `Drive.wav`.
6. `python3 tools/measure_offset.py Pitch.wav Drive.wav --expect 43200`
   Expected: `offset: 43200 frames` / `PASS`.
7. Repeat for 45 RPM (`--expect 32000`) and Custom 12345 samples (`--expect 12345`).
8. Toggle the plugin's bypass and export again: offset must be `0`.

Record the Logic version and results in the PR / commit message.
