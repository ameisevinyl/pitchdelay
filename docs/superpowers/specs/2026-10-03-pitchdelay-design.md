# pitchdelay — Design

Date: 2026-10-03
Status: Draft, awaiting review

## Purpose

An audio plugin (AU + VST3) that delays a track by an exact number of sample
frames, 100% wet and bit-transparent. It is used on the *drive* path of a
record-cutting lathe: the drive channel is delayed by half a lathe revolution
relative to the undelayed *pitch* (preview) channel, so the pitch-correction
system can look ahead.

Technology background: https://flokason.ch/pitch13_manual.html
(delay times quoted there: 0.9 s at 33⅓ RPM, 0.667 s at 45 RPM — a half
revolution. Verify against the page; the figures were read via a summarizer.)

## Decisions (agreed with the user)

- Channel delay only. The DAW does the routing: the undelayed path feeds the
  pitch output, a second path through the plugin feeds the drive output.
  A dual-output plugin was rejected: Logic effect plugins have a single output
  bus, and it is less portable.
- The plugin reports **0 latency**. The DAW must not compensate; the delayed
  path is late by exactly N frames.
- Framework: **JUCE** (AGPLv3, open source). Chosen over iPlug2 for
  documentation, CMake support, parameter handling and scriptable validation.
  Rejected: DPF and nih-plug (no AU), plain Steinberg VST3 SDK (VST3 only,
  and Logic hosts only AU).
- Formats: AU and VST3 (plus Standalone for testing). macOS arm64/x86_64 first.
- Hard jump when the delay changes during playback (may click). Accepted.
- Host bypass is honoured: bypassed audio passes through undelayed.

## Architecture

```
pitchdelay/
  core/      DelayLine — plain C++, no JUCE dependency
  plugin/    JUCE AudioProcessor, parameters, minimal editor
  tests/     Catch2 unit tests; scripted pluginval / auval
  CMakeLists.txt
```

JUCE is fetched by CMake at a pinned release.

### DelayLine (core)

- Pre-allocated ring buffer per channel; allocated in `prepareToPlay`, never on
  the audio thread.
- Only copies samples: no gain, filter, dither, interpolation or feedback.
  Output is bit-identical to input, including NaN and denormals.
- Delay N is a whole number of frames. Speed modes:
  `N = round(30 * sampleRate / rpm)` — 0.9 s at 33⅓ RPM and 2/3 s at 45 RPM,
  which is an exact integer at 44.1, 48, 88.2, 96, 176.4 and 192 kHz.
- Per-sample processing, independent of host block size; N may be smaller or
  larger than a block; N = 0 is pass-through.
- Mono and stereo, same delay on all channels.
- Buffer is cleared on prepare, not on transport start/stop.
- First N frames after clearing are silence.

## Parameters

- Mode: 33⅓ RPM / 45 RPM / Custom.
- Custom unit: milliseconds / samples.
- Custom milliseconds: 0–10,000, rounded to the nearest frame.
- Custom samples: 0 up to the 10 s equivalent at the current rate; used as is.
- Maximum delay 10 s (about 15 MB at 192 kHz stereo); larger values clamp.
- No mix/dry-wet control. Parameters are not host-automatable.
- Sample-rate change: frame count recomputed in `prepareToPlay`; speed modes
  and custom-ms follow the rate, custom-samples stays fixed.
- 32-bit float only in v1.

## Bypass

- Exposed as the host's real bypass parameter.
- Bypassed: output = input (undelayed), while the input keeps being written to
  the buffer, so un-bypassing outputs correctly delayed audio immediately.
- Hard switch. Reported latency stays 0 in both states.

## Editor

Default JUCE look: mode selector; unit selector and value field in Custom mode;
readout such as `39,690 frames = 900.000 ms @ 44.1 kHz` and `Reported latency: 0`.

## Build

CMake, JUCE pinned. Targets AU, VST3, Standalone. Post-build copy to
`~/Library/Audio/Plug-Ins/`. Ad-hoc signing for local use.

## Testing

1. Core unit tests (Catch2): impulse appears exactly N frames later and nowhere
   else; random blocks compared with `memcmp` (including NaN/denormal
   patterns); block sizes 1, 63, 512 and larger than N; N = 0, 1, max; hard
   jump on delay change; RPM table at every common sample rate.
2. Processor tests: latency 0; bypass passes undelayed; correct delayed audio
   on un-bypass; parameter save/load; sample-rate change recomputes the delay.
3. `pluginval` (high strictness) and `auval` from the command line.
4. Manual Logic test: split a test signal into plain and plugin paths, bounce
   both, cross-correlate; offset must be exactly N frames, confirming Logic
   does not compensate.

CI: GitHub Actions macOS job runs build + tests 1–3.

## Licence

AGPLv3, with JUCE attribution.

## Out of scope for v1

AAX, CLAP, 64-bit processing, custom graphics, notarized installers, host
automation, dual-output mode, Windows/Linux testing.

## Risks

- Logic's AU validation and unique AU manufacturer/plugin codes. Mitigation:
  first implementation task is a minimal loadable AU + VST3.
- Hosts with unusual PDC behaviour; covered by the manual cross-correlation test.
