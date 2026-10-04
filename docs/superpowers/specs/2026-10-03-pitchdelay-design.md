# pitchdelay — Design

Date: 2026-10-03 (revision 2: 2026-10-04 — revolution fractions, calibration mode,
linked delay fields, Custom mode removed)
Status: Draft, awaiting review

## Purpose

An audio plugin (AU + VST3) that delays a track by an exact number of sample
frames, 100% wet and bit-transparent. It is used on the *drive* path of a
record-cutting lathe: the drive channel is delayed by half a lathe revolution
relative to the undelayed *pitch* (preview) channel, so the pitch-correction
system can look ahead. A calibration mode replaces the audio with a pulse train
whose period equals the delay, for measuring the real platter speed.

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
- The buffer is wiped whenever the host transport is stopped (see DelayLine).
- Speed is 33⅓ or 45 RPM only; the delay is a fraction of a revolution
  (1/1, 1/2 default, 1/4, 1/8, 1/16) and can be fine-tuned in milliseconds or
  samples through two linked fields. There is no separate Custom mode.
- Calibration is a separate on/off switch that works with whatever delay is set.
- The calibration pulse is a short tone burst.

## Architecture

```
pitchdelay/
  core/      DelayLine, DelaySettings, PulseGenerator — plain C++, no JUCE dependency
  plugin/    JUCE AudioProcessor, parameters, editor
  tests/     Catch2 unit tests; scripted pluginval / auval
  CMakeLists.txt
```

JUCE is fetched by CMake at a pinned release.

### DelayLine (core)

- Pre-allocated ring buffer per channel; allocated in `prepareToPlay`, never on
  the audio thread.
- Only copies samples: no gain, filter, dither, interpolation or feedback.
  Output is bit-identical to input, including NaN and denormals.
- Delay N is a whole number of frames.
- Per-sample processing, independent of host block size; N may be smaller or
  larger than a block; N = 0 is pass-through.
- Mono and stereo, same delay on all channels.
- Buffer is cleared on prepare and on `reset()`, and **whenever the host transport is
  stopped**: while the host reports "not playing" the buffer is wiped after every block and
  the (non-bypassed) output is silent, so the next start begins with a full delay of zeros.
  Hosts that give no transport information are treated as always playing. Jumps in playback
  position while playing (cycle, relocate) do not clear the buffer.
- First N frames after clearing are silence.

### DelaySettings (core)

- Base delay for a speed and fraction:
  `N = round(60 * sampleRate / (rpm * divisor))`, divisor ∈ {1, 2, 4, 8, 16}
  (fractions 1/1, 1/2, 1/4, 1/8, 1/16 of a revolution; `round` is half away from zero).
  33⅓ RPM is exactly 100/3 RPM.
- 1/2 revolution is the default: 0.9 s at 33⅓ RPM and 2/3 s at 45 RPM.
- Exactness: all fractions are whole frames at every common rate (44.1, 48, 88.2, 96,
  176.4, 192 kHz) for 45 RPM and for 33⅓ RPM, except for 33⅓ RPM at 44.1 kHz with 1/8
  (9922.5 → 9923) and 1/16 (4961.25 → 4961), and at 88.2 kHz with 1/16 (9922.5 → 9923).
  The editor readout always shows the exact frame count in use.
- Sample-rate rescale: a delay of D frames set at rate r1 becomes `round(D * r2 / r1)`
  at rate r2, preserving its duration, clamped to the maximum delay of r2. For base delays
  this is exact whenever the base delay is a whole number of frames at both rates. See
  "Delay value" for how repeated rate changes avoid accumulating rounding errors.
- Millisecond entry converts with `round(ms * sampleRate / 1000)`.
- Maximum delay 10 s (`ceil(10 * sampleRate)` frames); larger values clamp.

### PulseGenerator (core)

- Emits one tone burst every N frames, where N is the delay currently in effect.
- Burst: `L = round(0.005 * sampleRate)` frames (5 ms) of a 1 kHz sine with a Hann
  window: `x(i) = g * 0.5 * (1 - cos(2π i / L)) * sin(2π * 1000 * i / sampleRate)`,
  `i = 0 … L-1`, `g` = linear level. The pulse time is the burst start (`i = 0`).
- If N < L the burst is cut off when the next pulse starts. N = 0 produces silence.
- The first pulse starts at frame 0 of playback. The phase is reset on prepare,
  `reset()` and whenever the host transport is stopped. When N changes, the phase
  restarts at 0 (hard jump).
- The burst is precomputed in `prepare`; identical settings give identical output.
  All channels carry the same pulses.

## Parameters

Host-visible parameters (none is host-automatable except Bypass):

- **Mode:** 33⅓ RPM / 45 RPM (default 33⅓).
- **Fraction:** 1/1, 1/2 (default), 1/4, 1/8, 1/16.
- **Calibration:** off (default) / on.
- **Calibration level:** −60 to 0 dB, default −20 dB.
- **Bypass:** the host's real bypass parameter.

Not host parameters: the **delay in frames**, whether it is tuned, and (when tuned) its anchor
frames and rate are stored in the saved plugin state (see Delay value).

- No mix/dry-wet control.
- The plugin reports its current delay as the **tail length**, so offline bounces keep the
  delayed end of the audio. (Latency stays 0.)
- 32-bit float only in v1.

### Delay value

- The delay is one whole number of frames D (0 … max), stored in the plugin state together
  with the sample rate it refers to.
- Changing Mode or Fraction sets D to the base delay for the new settings at the current
  sample rate, overwriting any fine-tuning.
- The editor's samples and milliseconds fields show D (milliseconds =
  `D / sampleRate * 1000`) and are linked. Typing or stepping either one sets D; milliseconds
  are rounded to the nearest frame.
- A delay is either **untouched** (set by Speed/Fraction) or **tuned** (typed or stepped).
  - An untouched delay is recomputed exactly from Speed and Fraction at every sample rate
    (one rounding, e.g. 33⅓ RPM, 1/16: 4961 frames at 44.1 kHz, 9923 at 88.2 kHz).
  - A tuned delay remembers the frames and the sample rate it was set at (its anchor). Every later
    rate is derived from the anchor, never from a previously rounded value, so after any number of
    rate changes its duration is within half a frame of the time the user set, and returning to
    the original rate restores the original frame count exactly. The anchor is kept even when the
    delay is temporarily clamped at a low rate.
  - Choosing Speed or Fraction makes the delay untouched again.
- New plugin instance: D is the base delay for 33⅓ RPM, 1/2 revolution.
- D is read on the audio thread through an atomic; editor and listeners write it.

### Saved-state compatibility

- A session saved by the second release (frames + rate, no tuned flag) loads as a tuned delay
  anchored at the stored frames and rate.
- A session saved by the first release (parameters `mode` ∈ {33⅓, 45, Custom}, `customUnit`,
  `customMs`, `customSamples`) loads as follows: `mode` 33⅓ or 45 → that speed at 1/2 revolution
  with D = base delay; `mode` Custom → D = the old custom delay (milliseconds converted at the
  sample rate in effect once known, samples used as is); Fraction defaults to 1/2.
  The old custom parameters are read only for this migration.
- State with unrecognised content or garbage is ignored and leaves the current values.

## Calibration mode

- While Calibration is on (and the host is playing and Bypass is off), the drive output is
  the pulse train from the PulseGenerator and the incoming audio is muted.
- The incoming audio is still recorded into the delay buffer, so switching Calibration off
  gives correctly delayed audio immediately.
- Bypass overrides Calibration (input passes through undelayed). A stopped transport
  overrides both (silent output, buffer wiped, pulse phase reset).
- The pulse period always equals the delay shown in the readout; the purpose is to cut the
  pulses and measure how much of a revolution their spacing covers, giving the real platter
  speed.
- Reported latency stays 0; the tail length stays the delay.

## Bypass

- Exposed as the host's real bypass parameter.
- Bypassed: output = input (undelayed), while the input keeps being written to
  the buffer, so un-bypassing outputs correctly delayed audio immediately.
- Hard switch. Reported latency stays 0 in both states.

## Editor

Default JUCE look. Controls:

- Mode selector (33⅓ / 45) and Fraction selector.
- Two linked fields for the delay: **samples** and **milliseconds**, each with ▲/▼ buttons
  (hold to repeat; ▲/▼ step 1 sample and 1 ms) and typed entry (milliseconds accept
  0.001 ms precision). No sliders.
- Calibration switch and Calibration level field.
- Readout such as `Delay: 43200 frames = 900.000 ms @ 48.0 kHz`, `Reported latency: 0`, and
  `CALIBRATION` while that mode is on.

## Build

CMake, JUCE pinned. Targets AU, VST3, Standalone. Post-build copy to
`~/Library/Audio/Plug-Ins/`. Ad-hoc signing for local use.

## Testing

1. Core unit tests (Catch2): impulse appears exactly N frames later and nowhere
   else; random blocks compared with `memcmp` (including NaN/denormal
   patterns); block sizes 1, 63, 512 and larger than N; N = 0, 1, max; hard
   jump on delay change. DelaySettings: frame counts for all five fractions at six sample
   rates for both speeds (including the rounded cases), millisecond rounding, rate
   rescale exactness. PulseGenerator: onsets at exactly 0, N, 2N …; peak between
   0.95x and 1.0x of the linear level `10^(level/20)` (the Hann window and the sine phase keep the
   sampled peak just below it); burst length 5 ms; truncation when N < L; reset restarts the phase;
   identical settings give identical output.
2. Processor tests: latency 0; bypass passes undelayed; correct delayed audio
   on un-bypass; parameter save/load; stop wipes the buffer; Mode/Fraction change
   overwrites D; typed values stick; sample-rate change rescales D; calibration output is
   the pulse train with the input never appearing; leaving calibration gives correct
   delayed audio; bypass beats calibration; stop silences calibration and restarts its
   phase; migration of first-release states (33⅓, 45, Custom ms, Custom samples); 1/16 at
   33⅓ RPM and 44.1 kHz reads 4961.
3. `pluginval` (high strictness) and `auval` from the command line.
4. Manual Logic tests: (a) split a test signal into plain and plugin paths, bounce
   both, cross-correlate; offset must be exactly N frames, confirming Logic
   does not compensate; (b) stop/restart gives a full delay of silence; (c) calibration
   pulses at the expected spacing.

CI: GitHub Actions macOS job runs build + tests 1–3.

## Licence

AGPLv3, with JUCE attribution.

## Out of scope for v1

AAX, CLAP, 64-bit processing, custom graphics, notarized installers, host
automation, dual-output mode, Windows/Linux testing, an analysis tool that
converts a recording of the calibration pulses into an RPM figure, adjustable
burst tone/length, buffer wipe on position jumps while playing.

## Risks

- Logic's AU validation and unique AU manufacturer/plugin codes. Mitigation:
  first implementation task is a minimal loadable AU + VST3. (Resolved: validated.)
- Hosts with unusual PDC behaviour; covered by the manual cross-correlation test.
- Saved-state migration for first-release sessions; covered by processor tests.
- Calibration pulses through a real cutting chain (bandwidth, pre-emphasis) may need a
  different tone or length; the burst is deliberately fixed in v1 so it can be tuned
  after the first real cut.
