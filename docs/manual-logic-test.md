# Manual Logic test: exact offset between the pitch and drive paths

Goal: confirm Logic does not compensate the plugin and the delayed path is late by exactly N frames.

1. `python3 tools/make_impulse.py ~/Desktop/impulse.wav` (48 kHz, 32-bit float, impulse at frame 48000).
2. Logic: new project at **48 kHz**. Add two audio tracks, both with `impulse.wav` starting at bar 1
   (position exactly 1 1 1 1, no snapping offset).
3. Track "Pitch": no plugins. Track "Drive": insert **PitchDelay** (AU), Speed = 33⅓ RPM,
   Fraction = 1/2 (readout must show `Delay: 43200 frames = 900.000 ms @ 48.0 kHz`).
4. Logic ▸ Settings ▸ Audio ▸ General: note the Plug-in Delay Compensation setting (try both *All* and
   *Audio and Software Instrument*). The result must not depend on it.
5. File ▸ Export ▸ All Tracks as Audio Files. WAV, **32-bit float** (or 24-bit), Normalize **Off**,
   Bypass Effect Plug-ins **Off**, **Include Audio Tail on**. Results: `Pitch.wav`, `Drive.wav`.
6. `python3 tools/measure_offset.py Pitch.wav Drive.wav --expect 43200`
   Expected: `offset: 43200 frames` / `PASS`.
7. Repeat for 45 RPM (`--expect 32000`), for Fraction 1/4 at 33⅓ RPM (`--expect 21600`), and for a
   typed delay of 12345 samples (`--expect 12345`).
8. Toggle the plugin's bypass and export again: offset must be `0`.

Record the Logic version and results in the PR / commit message.

## Stop and restart (clear on stop)

1. Play a few seconds of audio through the Drive track, press Stop, move to the start, press Play.
2. The Drive channel must be silent for the whole delay (0.9 s at 33⅓ RPM, 1/2) before the audio starts.

## Calibration pulses

1. Switch **Calibration** on on the Drive track (level −20 dB). The status line shows
   `CALIBRATION: pulse every 43200 frames`.
2. Play and bounce the Drive track for 5 s. Open the bounce: one 5 ms 1 kHz burst should start at
   0 s, then every 0.900 s (43200 frames at 48 kHz); the incoming audio must not be heard.
3. Change Fraction to 1/4: pulses every 0.450 s. Press Stop, then Play: the first pulse is again
   at the very start.
4. Switch Calibration off: the delayed audio returns at once.

## Fine-tuning fields

1. Type `43210` into the samples field and press Return: the milliseconds field shows `900.208`.
2. Type `900.5` into the milliseconds field: the samples field shows `43224`.
3. Hold ▲ on the samples field: the value counts up, speeding up while held. While the field is
   being typed in, the display must not be overwritten.
4. Choose another Fraction: both fields jump to the new base delay.
5. Save the project, reopen it: the fine-tuned delay is still there. Open a project saved with the
   previous plugin version (Custom mode): it keeps its old delay.
