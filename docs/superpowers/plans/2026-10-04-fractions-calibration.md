# Fractions, Calibration and Linked Delay Fields Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the Custom mode with a revolution-fraction selector and two linked delay fields (samples / milliseconds with ▲▼ buttons and typed entry), and add a calibration mode that replaces the audio with a pulse train whose period equals the delay.

**Architecture:** `core/DelaySettings` gains exact rational base-delay arithmetic and rescaling; a new JUCE-free `core/PulseGenerator` produces the tone-burst train. The processor stores the delay as one atomic frame count (kept in the saved state with its sample rate), recomputes it when Speed or Fraction change, and runs the calibration branch in `processBlock`. The editor gets a reusable `StepField` component. Everything stays testable headlessly; the editor is compiled into the test target.

**Tech Stack:** C++17, CMake + Ninja, JUCE 9.0.3, Catch2 v3.16.0, pluginval, `auval` (unchanged from the existing project).

**Spec:** `docs/superpowers/specs/2026-10-03-pitchdelay-design.md` (revision 2)

**Branch:** `feature/calibration` (already created, spec committed).

## Global Constraints

- All of the existing constraints still hold: delay is a whole number of frames; 100% wet; bit-identical output (NaN/denormals); **no `juce::ScopedNoDenormals`**; reported latency 0; max delay `ceil(10 * sampleRate)` frames; no allocation on the audio thread; buffer wiped on prepare, `reset()` and whenever the host transport is stopped; bypass passes undelayed while still recording; mono/stereo only; 32-bit float only; AU codes `Amvl` / `Pdly`; AGPLv3.
- Base delay: `N = round(60 * sampleRate / (rpm * divisor))`, divisor ∈ {1, 2, 4, 8, 16}, 33⅓ RPM is exactly 100/3, `round` is half away from zero, computed with exact integer arithmetic (not floating point).
- Default: 33⅓ RPM, fraction 1/2 (index 1).
- Calibration pulse: 1 kHz sine burst, `L = round(0.005 * sampleRate)` frames, Hann window `0.5 * (1 - cos(2π i / L))`, level default −20 dB (range −60 to 0, step 0.5), period = current delay in frames, first pulse at frame 0, phase restarts when the period changes, on reset, on stop and when calibration is switched on; N < L truncates the burst; N = 0 is silence.
- Calibration precedence: stopped transport > bypass > calibration. Calibration still records the incoming audio into the delay buffer.
- Host-visible parameters: `mode` (33⅓ / 45; id version 2), `fraction`, `calibration`, `calibrationLevel`, `bypass`. All except bypass are non-automatable. The delay (frames + the sample rate it refers to) lives in the saved state as properties `delayFrames` and `delayRate`, not as a parameter.
- Changing Speed or Fraction overwrites the delay; a sample-rate change rescales it with `round(D * newRate / oldRate)`; typed values are clamped to `[0, maxDelayFrames]`.
- First-release sessions (`customUnit`, `customMs`, `customSamples`, `mode` = 2 for Custom) must load: Custom → its old delay (ms converted at the rate once known, samples as is); 33⅓/45 → that speed at 1/2 revolution.
- Every commit message ends with the trailer `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>`.
- Run commands from `/Users/ameise/src/pitchdelay`. The test binaries are `build/pitchdelay_core_tests` and `build/pitchdelay_processor_tests_artefacts/Release/pitchdelay_processor_tests`; `ctest --test-dir build --output-on-failure` runs both.

## Review Focus

1. **First-release sessions** load correctly in every shape: Custom ms before `prepareToPlay` (rate unknown), Custom samples, 33⅓, 45, and a state with extra unknown content (Task 3).
2. **Sample-rate change** rescales a *tuned* delay (not just base delays) and clamps when the new rate would exceed the maximum (Tasks 1 and 3).
3. **Calibration edge cases:** delay 0 → silence; delay shorter than the 5 ms burst; delay changed mid-run restarts the phase; calibration switched on mid-playback starts with a pulse at frame 0; stop then start restarts the phase (Tasks 2 and 3).
4. **Garbage typed into the fields** ("abc", empty, "-5", "12x", a 20-digit number) must not crash or change the delay in a surprising way (Task 4).
5. **Typing while the 15 Hz refresh runs:** the refresh must not overwrite a field the user is editing (Task 4, `StepField::setDisplayedText`; verified manually in Task 5 because it needs a real window).

---

## File Structure

```
core/DelaySettings.h/.cpp     modify: add Speed, fractions, baseDelayFrames, rescaleFrames, clampFrames,
                              millisecondsForFrames; later remove the first-release API
core/PulseGenerator.h/.cpp    create
plugin/ParameterIds.h         modify: new ids + legacy ids
plugin/PluginProcessor.h/.cpp rewrite
plugin/PluginEditor.h/.cpp    rewrite (StepField + PitchDelayEditor)
tests/DelaySettingsTests.cpp  modify
tests/PulseGeneratorTests.cpp create
tests/ProcessorTests.cpp      rewrite
tests/EditorTests.cpp         create
CMakeLists.txt                modify
docs/manual-logic-test.md     modify
README.md                     modify
```

---

### Task 1: DelaySettings — fractions, exact base delay, rescale (additive, TDD)

**Files:**
- Modify: `core/DelaySettings.h`, `core/DelaySettings.cpp`, `tests/DelaySettingsTests.cpp`

The first-release API (`Mode`, `CustomUnit`, `framesForRpm`, `computeDelayFrames`, `kRpm33`, `kRpm45`, `kMaxCustomMs`, `kMaxCustomSamples`) stays until Task 3 so the tree keeps building.

**Interfaces:**
- Produces (namespace `pitchdelay`):
```cpp
enum class Speed { Rpm33 = 0, Rpm45 = 1 };
inline constexpr int kNumFractions = 5;
inline constexpr int kFractionDivisors[kNumFractions] = { 1, 2, 4, 8, 16 };
inline constexpr int kDefaultFractionIndex = 1;                 // 1/2 revolution
int    baseDelayFrames (Speed, int fractionIndex, double sampleRate);   // exact rational, clamped
int    clampFrames (int frames, double sampleRate);                      // [0, maxDelayFrames]
int    rescaleFrames (int frames, double fromRate, double toRate);       // round(D*to/from), clamped
double millisecondsForFrames (int frames, double sampleRate);
```

- [ ] **Step 1: Append the failing tests to `tests/DelaySettingsTests.cpp`**

Add `#include <catch2/catch_approx.hpp>` to the includes, then append:

```cpp
TEST_CASE ("base delay frames for every speed, fraction and common sample rate")
{
    struct Row { Speed speed; double sr; int frames[kNumFractions]; };
    const Row table[] = {
        { Speed::Rpm33, 44100.0,  { 79380, 39690, 19845, 9923, 4961 } },
        { Speed::Rpm33, 48000.0,  { 86400, 43200, 21600, 10800, 5400 } },
        { Speed::Rpm33, 88200.0,  { 158760, 79380, 39690, 19845, 9923 } },
        { Speed::Rpm33, 96000.0,  { 172800, 86400, 43200, 21600, 10800 } },
        { Speed::Rpm33, 176400.0, { 317520, 158760, 79380, 39690, 19845 } },
        { Speed::Rpm33, 192000.0, { 345600, 172800, 86400, 43200, 21600 } },
        { Speed::Rpm45, 44100.0,  { 58800, 29400, 14700, 7350, 3675 } },
        { Speed::Rpm45, 48000.0,  { 64000, 32000, 16000, 8000, 4000 } },
        { Speed::Rpm45, 88200.0,  { 117600, 58800, 29400, 14700, 7350 } },
        { Speed::Rpm45, 96000.0,  { 128000, 64000, 32000, 16000, 8000 } },
        { Speed::Rpm45, 176400.0, { 235200, 117600, 58800, 29400, 14700 } },
        { Speed::Rpm45, 192000.0, { 256000, 128000, 64000, 32000, 16000 } },
    };
    for (const auto& r : table)
        for (int f = 0; f < kNumFractions; ++f)
        {
            INFO ("speed " << (int) r.speed << ", rate " << r.sr << ", divisor " << kFractionDivisors[f]);
            REQUIRE (baseDelayFrames (r.speed, f, r.sr) == r.frames[f]);
        }
}

TEST_CASE ("the default fraction is one half revolution")
{
    REQUIRE (kFractionDivisors[kDefaultFractionIndex] == 2);
    REQUIRE (baseDelayFrames (Speed::Rpm33, kDefaultFractionIndex, 48000.0) == 43200);
    REQUIRE (baseDelayFrames (Speed::Rpm45, kDefaultFractionIndex, 48000.0) == 32000);
}

TEST_CASE ("an out-of-range fraction index is clamped")
{
    REQUIRE (baseDelayFrames (Speed::Rpm33, -3, 48000.0) == baseDelayFrames (Speed::Rpm33, 0, 48000.0));
    REQUIRE (baseDelayFrames (Speed::Rpm33, 99, 48000.0) == baseDelayFrames (Speed::Rpm33, 4, 48000.0));
}

TEST_CASE ("rescaling keeps the duration")
{
    REQUIRE (rescaleFrames (39690, 44100.0, 48000.0) == 43200);
    REQUIRE (rescaleFrames (43200, 48000.0, 44100.0) == 39690);
    REQUIRE (rescaleFrames (32000, 48000.0, 192000.0) == 128000);
    REQUIRE (rescaleFrames (4961, 44100.0, 48000.0) == 5400);    // tuned value, rounds to nearest
    REQUIRE (rescaleFrames (39700, 44100.0, 48000.0) == 43211);  // 43210.88
    REQUIRE (rescaleFrames (12345, 48000.0, 48000.0) == 12345);  // same rate: unchanged
    REQUIRE (rescaleFrames (1000, 0.0, 48000.0) == 1000);        // unknown source rate: unchanged
}

TEST_CASE ("rescaling clamps to the maximum delay of the new rate")
{
    REQUIRE (rescaleFrames (1000000, 44100.0, 192000.0) == 1920000);   // would be 4353741
    REQUIRE (rescaleFrames (-5, 44100.0, 48000.0) == 0);
}

TEST_CASE ("clampFrames and millisecondsForFrames")
{
    REQUIRE (clampFrames (-5, 48000.0) == 0);
    REQUIRE (clampFrames (10000000, 48000.0) == 480000);
    REQUIRE (clampFrames (1234, 48000.0) == 1234);
    REQUIRE (millisecondsForFrames (43200, 48000.0) == Catch::Approx (900.0));
    REQUIRE (millisecondsForFrames (39690, 44100.0) == Catch::Approx (900.0));
    REQUIRE (millisecondsForFrames (0, 48000.0) == 0.0);
    REQUIRE (millisecondsForFrames (100, 0.0) == 0.0);
}
```

- [ ] **Step 2: Run to verify failure**

```bash
cmake --build build --target pitchdelay_core_tests 2>&1 | grep error | head -3
```
Expected: FAIL — `Speed`, `baseDelayFrames` undeclared.

- [ ] **Step 3: Add the declarations to `core/DelaySettings.h`** (keep everything already there; insert after `int computeDelayFrames (...)`, before the closing `}` of the namespace)

```cpp

// ---- revision 2: speed + revolution fraction -----------------------------------
enum class Speed { Rpm33 = 0, Rpm45 = 1 };

inline constexpr int kNumFractions = 5;
inline constexpr int kFractionDivisors[kNumFractions] = { 1, 2, 4, 8, 16 };
inline constexpr int kDefaultFractionIndex = 1;   // 1/2 revolution

// round (60 * sampleRate / (rpm * divisor)), exact rational arithmetic, half away from zero,
// clamped to [0, maxDelayFrames]. fractionIndex is clamped to the valid range.
int baseDelayFrames (Speed speed, int fractionIndex, double sampleRate);
int clampFrames (int frames, double sampleRate);
// Same duration at another rate: round (frames * toRate / fromRate), clamped for toRate.
// Unchanged when either rate is not positive or the rates are equal.
int rescaleFrames (int frames, double fromRate, double toRate);
double millisecondsForFrames (int frames, double sampleRate);
```

- [ ] **Step 4: Implement in `core/DelaySettings.cpp`**

Add `#include <cstdint>` to the includes and, inside `namespace pitchdelay`, before the closing brace:

```cpp
namespace
{
bool isWhole (double x) { return std::abs (x - std::round (x)) < 1e-6; }

// round (n / d), half away from zero, for n >= 0 and d > 0.
std::int64_t roundedRatio (std::int64_t n, std::int64_t d) { return (2 * n + d) / (2 * d); }
}

int clampFrames (int frames, double sampleRate)
{
    return std::clamp (frames, 0, maxDelayFrames (sampleRate));
}

int baseDelayFrames (Speed speed, int fractionIndex, double sampleRate)
{
    const std::int64_t divisor = kFractionDivisors[std::clamp (fractionIndex, 0, kNumFractions - 1)];
    // rpm as a ratio: 33 1/3 = 100/3, 45 = 45/1
    const std::int64_t rpmNum = speed == Speed::Rpm33 ? 100 : 45;
    const std::int64_t rpmDen = speed == Speed::Rpm33 ? 3 : 1;

    std::int64_t frames;
    if (isWhole (sampleRate))
        frames = roundedRatio (60 * static_cast<std::int64_t> (std::llround (sampleRate)) * rpmDen,
                               rpmNum * divisor);
    else
        frames = std::llround (60.0 * sampleRate * static_cast<double> (rpmDen)
                               / (static_cast<double> (rpmNum) * static_cast<double> (divisor)));

    return clampFrames (static_cast<int> (std::clamp<std::int64_t> (frames, 0, 2000000000)), sampleRate);
}

int rescaleFrames (int frames, double fromRate, double toRate)
{
    if (fromRate <= 0.0 || toRate <= 0.0 || fromRate == toRate)
        return frames;

    const std::int64_t n = std::max (frames, 0);
    std::int64_t result;
    if (isWhole (fromRate) && isWhole (toRate))
        result = roundedRatio (n * static_cast<std::int64_t> (std::llround (toRate)),
                               static_cast<std::int64_t> (std::llround (fromRate)));
    else
        result = std::llround (static_cast<double> (n) * toRate / fromRate);
    return clampFrames (static_cast<int> (std::clamp<std::int64_t> (result, 0, 2000000000)), toRate);
}

double millisecondsForFrames (int frames, double sampleRate)
{
    return sampleRate > 0.0 ? static_cast<double> (frames) * 1000.0 / sampleRate : 0.0;
}
```
`rescaleFrames` returns `frames` unchanged for unknown or equal rates; the test `rescaleFrames (-5, 44100.0, 48000.0) == 0` goes through the rescaling branch (different rates), so it clamps to 0.

- [ ] **Step 5: Run to verify pass**

```bash
cmake --build build --target pitchdelay_core_tests 2>&1 | grep -E "error|warning:" | head; ./build/pitchdelay_core_tests | tail -3
```
Expected: `All tests passed`.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "feat: exact fraction-of-revolution base delay and rate rescale" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
git push
```

---

### Task 2: PulseGenerator (TDD)

**Files:**
- Create: `core/PulseGenerator.h`, `core/PulseGenerator.cpp`, `tests/PulseGeneratorTests.cpp`
- Modify: `CMakeLists.txt`, `docs/superpowers/specs/2026-10-03-pitchdelay-design.md` (one testing line)

**Interfaces:**
- Produces (namespace `pitchdelay`):
```cpp
class PulseGenerator {
public:
    static constexpr double kBurstSeconds = 0.005;
    static constexpr double kToneHz = 1000.0;
    void prepare (double sampleRate);               // builds the burst, resets the phase
    void reset() noexcept;                          // phase = 0
    void setPeriodFrames (int frames) noexcept;     // negative -> 0; a different value restarts the phase
    void setLevelDb (float db) noexcept;            // gain = 10^(db/20)
    int  getBurstFrames() const noexcept;
    void generate (float* out, int numFrames) noexcept;   // one channel; advances the phase
};
```

- [ ] **Step 1: Wire the core library and write the failing tests**

In `CMakeLists.txt` change the core library and the core test sources:
```cmake
add_library(pitchdelay_core STATIC core/DelayLine.cpp core/DelaySettings.cpp core/PulseGenerator.cpp)
```
```cmake
add_executable(pitchdelay_core_tests
    tests/DelayLineTests.cpp tests/DelaySettingsTests.cpp tests/PulseGeneratorTests.cpp)
```
Create stubs `core/PulseGenerator.cpp` (`#include "PulseGenerator.h"`) and `core/PulseGenerator.h` (`#pragma once`), then write `tests/PulseGeneratorTests.cpp`:

```cpp
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "PulseGenerator.h"

using pitchdelay::PulseGenerator;

namespace
{
// Independent reference for burst sample i (unit amplitude), straight from the spec.
float referenceBurst (int i, double sr)
{
    const int L = std::max (1, (int) std::llround (0.005 * sr));
    const double twoPi = 6.283185307179586;
    const double window = 0.5 * (1.0 - std::cos (twoPi * i / L));
    return (float) (window * std::sin (twoPi * 1000.0 * i / sr));
}

float gainFor (float db) { return (float) std::pow (10.0, (double) db / 20.0); }

std::vector<float> generate (PulseGenerator& g, size_t frames, int block = 64)
{
    std::vector<float> out (frames);
    for (size_t pos = 0; pos < frames; pos += (size_t) block)
        g.generate (out.data() + pos, (int) std::min<size_t> ((size_t) block, frames - pos));
    return out;
}

std::uint32_t bitsOf (float f) { std::uint32_t u; std::memcpy (&u, &f, 4); return u; }
}

TEST_CASE ("the burst is 5 ms long")
{
    PulseGenerator g;
    g.prepare (48000.0);
    REQUIRE (g.getBurstFrames() == 240);
    g.prepare (44100.0);
    REQUIRE (g.getBurstFrames() == 221);   // 220.5 rounds away from zero
    g.prepare (192000.0);
    REQUIRE (g.getBurstFrames() == 960);
}

TEST_CASE ("bursts start at exactly 0, N, 2N ... and match the reference waveform")
{
    const double sr = 48000.0;
    const int N = 1000;
    PulseGenerator g;
    g.prepare (sr);
    g.setPeriodFrames (N);
    g.setLevelDb (-20.0f);

    const auto out = generate (g, 3500, 77);   // block size that does not divide N
    const float gain = gainFor (-20.0f);
    for (size_t k = 0; k < out.size(); ++k)
    {
        const int pos = (int) (k % N);
        const float expected = pos < 240 ? gain * referenceBurst (pos, sr) : 0.0f;
        INFO ("frame " << k);
        REQUIRE (out[k] == Catch::Approx (expected).margin (1e-7));
    }
}

TEST_CASE ("the signal is silent between bursts")
{
    PulseGenerator g;
    g.prepare (48000.0);
    g.setPeriodFrames (1000);
    const auto out = generate (g, 2000);
    for (size_t k = 240; k < 1000; ++k) REQUIRE (out[k] == 0.0f);
}

TEST_CASE ("peak level follows the level setting")
{
    PulseGenerator g;
    g.prepare (48000.0);
    g.setPeriodFrames (1000);
    g.setLevelDb (-20.0f);
    float peak = 0.0f;
    for (float v : generate (g, 1000)) peak = std::max (peak, std::abs (v));
    REQUIRE (peak <= 0.1f * 1.0001f);
    REQUIRE (peak >= 0.1f * 0.95f);

    g.reset();
    g.setLevelDb (-6.0f);
    peak = 0.0f;
    for (float v : generate (g, 1000)) peak = std::max (peak, std::abs (v));
    REQUIRE (peak <= gainFor (-6.0f) * 1.0001f);
    REQUIRE (peak >= gainFor (-6.0f) * 0.95f);
}

TEST_CASE ("a period shorter than the burst truncates it and restarts it")
{
    const double sr = 48000.0;
    PulseGenerator g;
    g.prepare (sr);
    g.setPeriodFrames (100);
    g.setLevelDb (-20.0f);
    const auto out = generate (g, 300);
    const float gain = gainFor (-20.0f);
    for (int k = 0; k < 300; ++k)
        REQUIRE (out[(size_t) k] == Catch::Approx (gain * referenceBurst (k % 100, sr)).margin (1e-7));
}

TEST_CASE ("period 0 is silence, also after running")
{
    PulseGenerator g;
    g.prepare (48000.0);
    for (float v : generate (g, 200)) REQUIRE (v == 0.0f);
    g.setPeriodFrames (500);
    generate (g, 100);
    g.setPeriodFrames (0);
    for (float v : generate (g, 200)) REQUIRE (v == 0.0f);
    g.setPeriodFrames (-7);   // negative behaves like 0
    for (float v : generate (g, 50)) REQUIRE (v == 0.0f);
}

TEST_CASE ("reset restarts the phase")
{
    PulseGenerator g;
    g.prepare (48000.0);
    g.setPeriodFrames (1000);
    const auto first = generate (g, 150);
    g.reset();
    const auto second = generate (g, 150);
    REQUIRE (first == second);
}

TEST_CASE ("a different period restarts the phase, the same period does not")
{
    const double sr = 48000.0;
    PulseGenerator g;
    g.prepare (sr);
    g.setPeriodFrames (1000);
    g.setLevelDb (-20.0f);
    generate (g, 300);                       // phase is now 300

    g.setPeriodFrames (1000);                // same value: keeps running
    const auto cont = generate (g, 5);
    for (int k = 0; k < 5; ++k)
        REQUIRE (cont[(size_t) k] == Catch::Approx (gainFor (-20.0f) * referenceBurst (300 + k, sr)).margin (1e-7));

    g.setPeriodFrames (500);                 // new value: restarts at the burst start
    const auto restart = generate (g, 5);
    for (int k = 0; k < 5; ++k)
        REQUIRE (restart[(size_t) k] == Catch::Approx (gainFor (-20.0f) * referenceBurst (k, sr)).margin (1e-7));
}

TEST_CASE ("identical settings give bit-identical output regardless of block size")
{
    PulseGenerator a, b;
    for (auto* g : { &a, &b }) { g->prepare (44100.0); g->setPeriodFrames (4961); g->setLevelDb (-20.0f); }
    const auto x = generate (a, 20000, 64);
    const auto y = generate (b, 20000, 513);
    for (size_t k = 0; k < x.size(); ++k) REQUIRE (bitsOf (x[k]) == bitsOf (y[k]));
}
```

- [ ] **Step 2: Run to verify failure**

```bash
cmake -S . -B build >/dev/null && cmake --build build --target pitchdelay_core_tests 2>&1 | grep error | head -3
```
Expected: FAIL — `PulseGenerator` not declared.

- [ ] **Step 3: Implement**

`core/PulseGenerator.h`:
```cpp
#pragma once
#include <vector>

namespace pitchdelay
{
// Periodic tone-burst generator for calibration: one Hann-windowed 1 kHz burst of 5 ms
// every `period` frames. The burst is precomputed in prepare().
class PulseGenerator
{
public:
    static constexpr double kBurstSeconds = 0.005;
    static constexpr double kToneHz = 1000.0;

    void prepare (double sampleRate);
    void reset() noexcept { phase_ = 0; }
    void setPeriodFrames (int frames) noexcept;
    void setLevelDb (float db) noexcept;
    int getBurstFrames() const noexcept { return static_cast<int> (burst_.size()); }

    // Writes numFrames samples of one channel and advances the phase.
    void generate (float* out, int numFrames) noexcept;

private:
    std::vector<float> burst_;
    int period_ = 0;
    int phase_ = 0;
    float gain_ = 0.1f;   // -20 dB
};
}
```

`core/PulseGenerator.cpp`:
```cpp
#include "PulseGenerator.h"

#include <algorithm>
#include <cmath>

namespace pitchdelay
{
void PulseGenerator::prepare (double sampleRate)
{
    const int length = std::max (1, static_cast<int> (std::llround (kBurstSeconds * sampleRate)));
    burst_.assign (static_cast<size_t> (length), 0.0f);
    const double twoPi = 6.283185307179586;
    for (int i = 0; i < length; ++i)
    {
        const double window = 0.5 * (1.0 - std::cos (twoPi * i / length));
        burst_[static_cast<size_t> (i)] = static_cast<float> (window * std::sin (twoPi * kToneHz * i / sampleRate));
    }
    phase_ = 0;
}

void PulseGenerator::setPeriodFrames (int frames) noexcept
{
    frames = std::max (0, frames);
    if (frames != period_)
    {
        period_ = frames;
        phase_ = 0;
    }
}

void PulseGenerator::setLevelDb (float db) noexcept
{
    gain_ = static_cast<float> (std::pow (10.0, static_cast<double> (db) / 20.0));
}

void PulseGenerator::generate (float* out, int numFrames) noexcept
{
    if (period_ <= 0 || burst_.empty())
    {
        std::fill (out, out + numFrames, 0.0f);
        return;
    }
    const int length = static_cast<int> (burst_.size());
    int phase = phase_;
    for (int i = 0; i < numFrames; ++i)
    {
        out[i] = phase < length ? gain_ * burst_[static_cast<size_t> (phase)] : 0.0f;
        if (++phase >= period_)
            phase = 0;
    }
    phase_ = phase;
}
}
```

- [ ] **Step 4: Run to verify pass**

```bash
cmake --build build --target pitchdelay_core_tests 2>&1 | grep -E "error|warning:" | head; ./build/pitchdelay_core_tests | tail -3
```
Expected: `All tests passed`.

- [ ] **Step 5: Fix the spec's peak wording and commit**

In `docs/superpowers/specs/2026-10-03-pitchdelay-design.md`, in the Testing list replace
``peak amplitude `10^(level/20)`;`` with
``peak between 0.95× and 1.0× of the linear level `10^(level/20)` (the Hann window and the sine phase keep the sampled peak just below it);``.

```bash
git add -A
git commit -m "feat: PulseGenerator tone-burst train for calibration" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
git push
```

---

### Task 3: Processor rework — parameters, linked delay, state migration, calibration (TDD)

**Files:**
- Modify: `plugin/ParameterIds.h`, `plugin/PluginProcessor.h`, `plugin/PluginProcessor.cpp`, `core/DelaySettings.h`, `core/DelaySettings.cpp`, `tests/DelaySettingsTests.cpp`, `tests/ProcessorTests.cpp` (full rewrite), `CMakeLists.txt`

The editor is rewritten in Task 4. In this task the processor reports no editor and `PluginEditor.cpp` is removed from the plugin target, so the tree builds and validates without it.

**Interfaces:**
- Consumes: `baseDelayFrames`, `clampFrames`, `rescaleFrames`, `framesForMilliseconds`, `maxDelayFrames`, `Speed`, `kDefaultFractionIndex`, `PulseGenerator`, `DelayLine`.
- Produces:
```cpp
class PitchDelayProcessor {
public:
    int  getCurrentDelayFrames() const noexcept;     // what the fields show; atomic
    void setDelayFrames (int frames);                // clamps to [0, max]; ignored before prepareToPlay
    juce::AudioProcessorValueTreeState apvts;        // params: mode, fraction, calibration, calibrationLevel, bypass
    void reset() override;                           // clears delay buffer + pulse phase
};
// plugin/ParameterIds.h: ids::mode, fraction, calibration, calibrationLevel, bypass,
//                        legacyCustomUnit, legacyCustomMs, legacyCustomSamples
```

- [ ] **Step 1: Replace `plugin/ParameterIds.h`**

```cpp
#pragma once

namespace pitchdelay::ids
{
inline constexpr const char* mode = "mode";
inline constexpr const char* fraction = "fraction";
inline constexpr const char* calibration = "calibration";
inline constexpr const char* calibrationLevel = "calibrationLevel";
inline constexpr const char* bypass = "bypass";

// First-release parameters: read only to migrate old sessions, never created.
inline constexpr const char* legacyCustomUnit = "customUnit";
inline constexpr const char* legacyCustomMs = "customMs";
inline constexpr const char* legacyCustomSamples = "customSamples";
}
```

- [ ] **Step 2: Write the new `tests/ProcessorTests.cpp` (complete replacement)**

```cpp
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <juce_audio_processors/juce_audio_processors.h>

#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

#include "DelaySettings.h"
#include "ParameterIds.h"
#include "PluginProcessor.h"
#include "PulseGenerator.h"

namespace ids = pitchdelay::ids;

namespace
{
uint32_t bitsOf (float f) { uint32_t u; std::memcpy (&u, &f, 4); return u; }
float fromBits (uint32_t u) { float f; std::memcpy (&f, &u, 4); return f; }

// Minimal host transport.
struct FakePlayHead : juce::AudioPlayHead
{
    bool playing = true;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setIsPlaying (playing);
        return info;
    }
};

struct Fixture
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    PitchDelayProcessor proc;
    juce::MidiBuffer midi;

    explicit Fixture (double sr = 48000.0, int block = 512, int channels = 2, bool prepare = true)
    {
        proc.setPlayConfigDetails (channels, channels, sr, block);
        if (prepare)
            proc.prepareToPlay (sr, block);
    }

    // Sets a parameter to a plain (denormalised) value. Listener callbacks that JUCE defers to the
    // message thread are flushed so the effect is visible immediately.
    void set (const char* id, float plainValue)
    {
        auto* p = proc.apvts.getParameter (id);
        p->setValueNotifyingHost (p->convertTo0to1 (plainValue));
        juce::MessageManager::getInstance()->runDispatchLoopUntil (5);
    }

    float get (const char* id) { return proc.apvts.getRawParameterValue (id)->load(); }

    // Runs channel 0 of `signal` (other channels get the same data) through the processor.
    std::vector<float> run (const std::vector<float>& signal, int block = 512, int channels = 2)
    {
        std::vector<float> out;
        out.reserve (signal.size());
        for (size_t pos = 0; pos < signal.size(); pos += (size_t) block)
        {
            const int n = (int) std::min<size_t> ((size_t) block, signal.size() - pos);
            juce::AudioBuffer<float> buf (channels, n);
            for (int c = 0; c < channels; ++c)
                std::memcpy (buf.getWritePointer (c), signal.data() + pos, (size_t) n * sizeof (float));
            proc.processBlock (buf, midi);
            out.insert (out.end(), buf.getReadPointer (0), buf.getReadPointer (0) + n);
        }
        return out;
    }
};

std::vector<float> ramp (size_t n, float start = 1.0f)
{
    std::vector<float> r (n);
    for (size_t i = 0; i < n; ++i) r[i] = start + (float) i;
    return r;
}

int firstNonZero (const std::vector<float>& v)
{
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i] != 0.0f) return (int) i;
    return -1;
}

std::vector<float> pulseTrain (double sr, int period, float levelDb, size_t frames)
{
    pitchdelay::PulseGenerator g;
    g.prepare (sr);
    g.setPeriodFrames (period);
    g.setLevelDb (levelDb);
    std::vector<float> out (frames);
    g.generate (out.data(), (int) frames);
    return out;
}

// A first-release saved state (parameters mode/customUnit/customMs/customSamples/bypass).
juce::MemoryBlock legacyState (int mode, int unit, double ms, int samples)
{
    juce::ValueTree s ("PitchDelayState");
    const auto add = [&s] (const char* id, double v)
    {
        juce::ValueTree p ("PARAM");
        p.setProperty ("id", id, nullptr);
        p.setProperty ("value", v, nullptr);
        s.appendChild (p, nullptr);
    };
    add ("mode", mode);
    add ("customUnit", unit);
    add ("customMs", ms);
    add ("customSamples", samples);
    add ("bypass", 0.0);
    juce::MemoryBlock block;
    juce::AudioProcessor::copyXmlToBinary (*s.createXml(), block);
    return block;
}
}

TEST_CASE ("reports zero latency")
{
    Fixture f;
    REQUIRE (f.proc.getLatencySamples() == 0);
}

TEST_CASE ("defaults are 33 1/3 RPM, 1/2 revolution, calibration off at -20 dB")
{
    Fixture f (48000.0);
    REQUIRE (f.get (ids::mode) == 0.0f);
    REQUIRE (f.get (ids::fraction) == (float) pitchdelay::kDefaultFractionIndex);
    REQUIRE (f.get (ids::calibration) == 0.0f);
    REQUIRE (f.get (ids::calibrationLevel) == -20.0f);
    REQUIRE (f.proc.getCurrentDelayFrames() == 43200);
}

TEST_CASE ("33 1/3 RPM at 48 kHz delays by exactly 43200 frames")
{
    Fixture f (48000.0);
    std::vector<float> in (43200 + 2000, 0.0f);
    in[10] = 1.0f;
    const auto out = f.run (in);
    REQUIRE (firstNonZero (out) == 10 + 43200);
    int nonZero = 0;
    for (float v : out) nonZero += v != 0.0f;
    REQUIRE (nonZero == 1);
}

TEST_CASE ("45 RPM at 96 kHz delays by exactly 64000 frames")
{
    Fixture f (96000.0);
    f.set (ids::mode, 1);
    std::vector<float> in (64000 + 1000, 0.0f);
    in[0] = 1.0f;
    REQUIRE (f.proc.getCurrentDelayFrames() == 64000);
    REQUIRE (firstNonZero (f.run (in)) == 64000);
}

TEST_CASE ("every fraction gives its base delay")
{
    Fixture f (48000.0);
    const int expected[] = { 86400, 43200, 21600, 10800, 5400 };
    for (int i = 0; i < 5; ++i)
    {
        f.set (ids::fraction, (float) i);
        REQUIRE (f.proc.getCurrentDelayFrames() == expected[i]);
    }

    Fixture g (44100.0);
    g.set (ids::fraction, 4);   // 1/16 of 33 1/3 RPM at 44.1 kHz
    REQUIRE (g.proc.getCurrentDelayFrames() == 4961);
}

TEST_CASE ("changing speed or fraction overwrites a fine-tuned delay")
{
    Fixture f (48000.0);
    f.proc.setDelayFrames (1234);
    REQUIRE (f.proc.getCurrentDelayFrames() == 1234);
    f.set (ids::mode, 1);
    REQUIRE (f.proc.getCurrentDelayFrames() == 32000);
    f.proc.setDelayFrames (777);
    f.set (ids::fraction, 0);
    REQUIRE (f.proc.getCurrentDelayFrames() == 64000);
}

TEST_CASE ("a typed delay sticks, is applied exactly, and is clamped")
{
    Fixture f (48000.0);
    f.proc.setDelayFrames (100);
    std::vector<float> in (400, 0.0f);
    in[0] = 1.0f;
    REQUIRE (firstNonZero (f.run (in)) == 100);

    f.proc.setDelayFrames (2000000000);
    REQUIRE (f.proc.getCurrentDelayFrames() == 480000);   // 10 s at 48 kHz
    f.proc.setDelayFrames (-9);
    REQUIRE (f.proc.getCurrentDelayFrames() == 0);
}

TEST_CASE ("setDelayFrames before prepareToPlay is ignored, the base delay is adopted on prepare")
{
    Fixture f (44100.0, 512, 2, false);
    f.proc.setDelayFrames (555);
    f.proc.prepareToPlay (44100.0, 512);
    REQUIRE (f.proc.getCurrentDelayFrames() == 39690);
}

TEST_CASE ("sample-rate change rescales the delay and keeps fine-tuning")
{
    Fixture f (44100.0);
    REQUIRE (f.proc.getCurrentDelayFrames() == 39690);
    f.proc.prepareToPlay (48000.0, 512);
    REQUIRE (f.proc.getCurrentDelayFrames() == 43200);
    f.proc.prepareToPlay (192000.0, 512);
    REQUIRE (f.proc.getCurrentDelayFrames() == 172800);

    Fixture g (44100.0);
    g.proc.setDelayFrames (39700);                         // tuned
    g.proc.prepareToPlay (48000.0, 512);
    REQUIRE (g.proc.getCurrentDelayFrames() == 43211);     // 43210.88 rounded
    g.proc.prepareToPlay (48000.0, 512);                   // same rate again: unchanged
    REQUIRE (g.proc.getCurrentDelayFrames() == 43211);
}

TEST_CASE ("switching to a higher sample rate re-allocates and still works")
{
    Fixture f (44100.0);
    f.set (ids::mode, 1);
    f.run (std::vector<float> (1000, 0.5f));
    f.proc.setPlayConfigDetails (2, 2, 192000.0, 512);
    f.proc.prepareToPlay (192000.0, 512);
    std::vector<float> in (128000 + 100, 0.0f);
    in[0] = 1.0f;
    REQUIRE (firstNonZero (f.run (in)) == 128000);
}

TEST_CASE ("tail length follows the delay before any audio is processed")
{
    Fixture f (48000.0);
    REQUIRE (f.proc.getTailLengthSeconds() == Catch::Approx (0.9).margin (1e-9));
    f.set (ids::mode, 1);
    REQUIRE (f.proc.getTailLengthSeconds() == Catch::Approx (2.0 / 3.0).margin (1e-9));
    f.proc.setDelayFrames (4800);
    REQUIRE (f.proc.getTailLengthSeconds() == Catch::Approx (0.1).margin (1e-9));
}

TEST_CASE ("bypass passes audio undelayed and un-bypass is immediately correctly delayed")
{
    Fixture f (48000.0);
    f.proc.setDelayFrames (100);
    const auto signal = ramp (1100);

    f.set (ids::bypass, 1.0f);
    const std::vector<float> first (signal.begin(), signal.begin() + 1000);
    REQUIRE (f.run (first) == first);

    f.set (ids::bypass, 0.0f);
    const std::vector<float> rest (signal.begin() + 1000, signal.end());
    const auto out = f.run (rest);
    for (size_t k = 0; k < out.size(); ++k)
        REQUIRE (out[k] == signal[1000 + k - 100]);
    REQUIRE (f.proc.getLatencySamples() == 0);
}

TEST_CASE ("NaN, inf and denormals pass through bit-identically")
{
    Fixture f (48000.0);
    f.proc.setDelayFrames (100);

    std::vector<float> in;
    for (uint32_t s : { 0x7fc12345u, 0x7f800001u, 0x7f800000u, 0xff800000u, 0x00000001u, 0x80000000u })
        in.push_back (fromBits (s));
    std::mt19937 rng (99);
    for (int i = 0; i < 3000; ++i) in.push_back (fromBits ((uint32_t) rng()));

    const auto out = f.run (in);
    for (size_t i = 0; i < in.size(); ++i)
        REQUIRE (bitsOf (out[i]) == (i < 100 ? 0u : bitsOf (in[i - 100])));
}

TEST_CASE ("mono layout works")
{
    Fixture f (48000.0, 512, 1);
    f.proc.setDelayFrames (10);
    std::vector<float> in (50, 0.0f);
    in[0] = 1.0f;
    REQUIRE (firstNonZero (f.run (in, 64, 1)) == 10);
}

TEST_CASE ("stopping the transport wipes the buffer so a restart begins with a full delay of zeros")
{
    Fixture f (48000.0);
    FakePlayHead host;
    f.proc.setPlayHead (&host);
    f.proc.setDelayFrames (100);

    host.playing = true;
    f.run (ramp (1000));

    host.playing = false;
    for (float v : f.run (ramp (300, 5000.0f))) REQUIRE (v == 0.0f);   // silent while stopped

    host.playing = true;
    for (float v : f.run (std::vector<float> (300, 0.0f))) REQUIRE (v == 0.0f);

    f.proc.setPlayHead (nullptr);
}

TEST_CASE ("after a stop, playback delays by exactly N frames again")
{
    Fixture f (48000.0);
    FakePlayHead host;
    f.proc.setPlayHead (&host);
    f.proc.setDelayFrames (100);

    host.playing = false;
    f.run (ramp (200));
    host.playing = true;
    std::vector<float> in (400, 0.0f);
    in[0] = 1.0f;
    REQUIRE (firstNonZero (f.run (in)) == 100);

    f.proc.setPlayHead (nullptr);
}

TEST_CASE ("bypass while the transport is stopped passes audio through and wipes the buffer")
{
    Fixture f (48000.0);
    FakePlayHead host;
    f.proc.setPlayHead (&host);
    f.proc.setDelayFrames (100);

    host.playing = true;
    f.run (ramp (500));
    f.set (ids::bypass, 1.0f);
    host.playing = false;
    const auto r = ramp (200, 9000.0f);
    REQUIRE (f.run (r) == r);

    f.set (ids::bypass, 0.0f);
    host.playing = true;
    for (float v : f.run (std::vector<float> (300, 0.0f))) REQUIRE (v == 0.0f);

    f.proc.setPlayHead (nullptr);
}

TEST_CASE ("reset() clears the delay buffer")
{
    Fixture f (48000.0);
    f.proc.setDelayFrames (100);
    f.run (ramp (1000));
    f.proc.reset();
    for (float v : f.run (std::vector<float> (300, 0.0f))) REQUIRE (v == 0.0f);
}

TEST_CASE ("a host that reports the transport as playing delays normally")
{
    Fixture f (48000.0);
    FakePlayHead host;
    f.proc.setPlayHead (&host);
    f.proc.setDelayFrames (100);
    std::vector<float> in (400, 0.0f);
    in[0] = 1.0f;
    REQUIRE (firstNonZero (f.run (in)) == 100);
    f.proc.setPlayHead (nullptr);
}

// ---- calibration ----------------------------------------------------------------

TEST_CASE ("calibration replaces the audio with the pulse train, on all channels")
{
    Fixture f (48000.0);
    f.proc.setDelayFrames (1000);
    f.set (ids::calibration, 1.0f);

    const auto out = f.run (ramp (3500, 100.0f), 300);
    REQUIRE (out == pulseTrain (48000.0, 1000, -20.0f, 3500));

    juce::AudioBuffer<float> buf (2, 256);
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < 256; ++i) buf.setSample (c, i, 7.0f);
    f.proc.processBlock (buf, f.midi);
    for (int i = 0; i < 256; ++i) REQUIRE (buf.getSample (0, i) == buf.getSample (1, i));
}

TEST_CASE ("calibration level parameter sets the pulse level")
{
    Fixture f (48000.0);
    f.proc.setDelayFrames (1000);
    f.set (ids::calibrationLevel, -6.0f);
    f.set (ids::calibration, 1.0f);
    REQUIRE (f.run (std::vector<float> (2000, 0.0f)) == pulseTrain (48000.0, 1000, -6.0f, 2000));
}

TEST_CASE ("leaving calibration gives correctly delayed audio at once")
{
    Fixture f (48000.0);
    f.proc.setDelayFrames (100);
    const auto signal = ramp (1200);

    f.set (ids::calibration, 1.0f);
    f.run (std::vector<float> (signal.begin(), signal.begin() + 1000));
    f.set (ids::calibration, 0.0f);
    const auto out = f.run (std::vector<float> (signal.begin() + 1000, signal.end()));
    for (size_t k = 0; k < out.size(); ++k)
        REQUIRE (out[k] == signal[1000 + k - 100]);
}

TEST_CASE ("bypass beats calibration")
{
    Fixture f (48000.0);
    f.proc.setDelayFrames (1000);
    f.set (ids::calibration, 1.0f);
    f.set (ids::bypass, 1.0f);
    const auto in = ramp (500);
    REQUIRE (f.run (in) == in);
}

TEST_CASE ("a stopped transport silences calibration and restarts its phase")
{
    Fixture f (48000.0);
    FakePlayHead host;
    f.proc.setPlayHead (&host);
    f.proc.setDelayFrames (1000);
    f.set (ids::calibration, 1.0f);

    host.playing = true;
    f.run (std::vector<float> (700, 0.0f));
    host.playing = false;
    for (float v : f.run (std::vector<float> (100, 0.0f))) REQUIRE (v == 0.0f);
    host.playing = true;
    REQUIRE (f.run (std::vector<float> (1500, 0.0f)) == pulseTrain (48000.0, 1000, -20.0f, 1500));

    f.proc.setPlayHead (nullptr);
}

TEST_CASE ("changing the delay during calibration restarts the pulse phase")
{
    Fixture f (48000.0);
    f.proc.setDelayFrames (1000);
    f.set (ids::calibration, 1.0f);
    f.run (std::vector<float> (300, 0.0f));
    f.proc.setDelayFrames (500);
    REQUIRE (f.run (std::vector<float> (600, 0.0f)) == pulseTrain (48000.0, 500, -20.0f, 600));
}

TEST_CASE ("switching calibration on mid-playback starts with a pulse at frame 0")
{
    Fixture f (48000.0);
    f.proc.setDelayFrames (1000);
    f.run (ramp (777));
    f.set (ids::calibration, 1.0f);
    REQUIRE (f.run (std::vector<float> (1200, 0.0f)) == pulseTrain (48000.0, 1000, -20.0f, 1200));
}

TEST_CASE ("calibration with delay 0 is silent, and with a delay shorter than the burst it truncates")
{
    Fixture f (48000.0);
    f.set (ids::calibration, 1.0f);
    f.proc.setDelayFrames (0);
    for (float v : f.run (ramp (300))) REQUIRE (v == 0.0f);

    f.proc.setDelayFrames (100);
    REQUIRE (f.run (std::vector<float> (500, 0.0f)) == pulseTrain (48000.0, 100, -20.0f, 500));
}

// ---- state ----------------------------------------------------------------------

TEST_CASE ("state round-trips, including the fine-tuned delay")
{
    Fixture a (48000.0);
    a.set (ids::mode, 1);
    a.set (ids::fraction, 3);
    a.proc.setDelayFrames (777);
    a.set (ids::calibration, 1.0f);
    a.set (ids::calibrationLevel, -12.0f);
    juce::MemoryBlock state;
    a.proc.getStateInformation (state);

    Fixture b (48000.0);
    b.proc.setStateInformation (state.getData(), (int) state.getSize());
    REQUIRE (b.get (ids::mode) == 1.0f);
    REQUIRE (b.get (ids::fraction) == 3.0f);
    REQUIRE (b.get (ids::calibration) == 1.0f);
    REQUIRE (b.get (ids::calibrationLevel) == -12.0f);
    REQUIRE (b.proc.getCurrentDelayFrames() == 777);
}

TEST_CASE ("a state saved at another sample rate is rescaled on load")
{
    Fixture a (44100.0);
    a.proc.setDelayFrames (39700);
    juce::MemoryBlock state;
    a.proc.getStateInformation (state);

    Fixture b (48000.0);
    b.proc.setStateInformation (state.getData(), (int) state.getSize());
    REQUIRE (b.proc.getCurrentDelayFrames() == 43211);
}

TEST_CASE ("corrupt saved state is ignored")
{
    Fixture f;
    f.set (ids::mode, 1);
    f.proc.setDelayFrames (1234);
    const char garbage[] = "this is not a valid state blob";
    f.proc.setStateInformation (garbage, (int) sizeof (garbage));
    f.proc.setStateInformation (nullptr, 0);
    REQUIRE (f.get (ids::mode) == 1.0f);
    REQUIRE (f.proc.getCurrentDelayFrames() == 1234);
}

TEST_CASE ("first-release sessions: 33 1/3 and 45 RPM load as that speed at 1/2 revolution")
{
    Fixture a (48000.0);
    auto s45 = legacyState (1, 0, 900.0, 0);
    a.proc.setStateInformation (s45.getData(), (int) s45.getSize());
    REQUIRE (a.get (ids::mode) == 1.0f);
    REQUIRE (a.get (ids::fraction) == (float) pitchdelay::kDefaultFractionIndex);
    REQUIRE (a.proc.getCurrentDelayFrames() == 32000);

    Fixture b (48000.0);
    auto s33 = legacyState (0, 0, 900.0, 0);
    b.proc.setStateInformation (s33.getData(), (int) s33.getSize());
    REQUIRE (b.get (ids::mode) == 0.0f);
    REQUIRE (b.proc.getCurrentDelayFrames() == 43200);
}

TEST_CASE ("first-release sessions: Custom milliseconds and Custom samples keep their delay")
{
    Fixture a (48000.0);
    auto ms = legacyState (2, 0, 900.0, 5);
    a.proc.setStateInformation (ms.getData(), (int) ms.getSize());
    REQUIRE (a.proc.getCurrentDelayFrames() == 43200);
    REQUIRE (a.get (ids::mode) == 0.0f);   // Custom no longer exists

    Fixture b (48000.0);
    auto samples = legacyState (2, 1, 900.0, 12345);
    b.proc.setStateInformation (samples.getData(), (int) samples.getSize());
    REQUIRE (b.proc.getCurrentDelayFrames() == 12345);
}

TEST_CASE ("first-release Custom milliseconds loaded before prepareToPlay resolve at the real rate")
{
    Fixture f (44100.0, 512, 2, false);
    auto ms = legacyState (2, 0, 900.0, 0);
    f.proc.setStateInformation (ms.getData(), (int) ms.getSize());
    f.proc.prepareToPlay (44100.0, 512);
    REQUIRE (f.proc.getCurrentDelayFrames() == 39690);
}

TEST_CASE ("first-release Custom samples loaded before prepareToPlay are kept as is")
{
    Fixture f (44100.0, 512, 2, false);
    auto samples = legacyState (2, 1, 0.0, 12345);
    f.proc.setStateInformation (samples.getData(), (int) samples.getSize());
    f.proc.prepareToPlay (44100.0, 512);
    REQUIRE (f.proc.getCurrentDelayFrames() == 12345);
}

TEST_CASE ("a migrated state does not keep the removed parameters when saved again")
{
    Fixture f (48000.0);
    auto s = legacyState (2, 1, 900.0, 777);
    f.proc.setStateInformation (s.getData(), (int) s.getSize());
    juce::MemoryBlock saved;
    f.proc.getStateInformation (saved);
    const auto xml = juce::AudioProcessor::getXmlFromBinary (saved.getData(), (int) saved.getSize());
    REQUIRE (xml != nullptr);
    REQUIRE (xml->getChildByAttribute ("id", "customMs") == nullptr);
    REQUIRE (xml->getChildByAttribute ("id", "customSamples") == nullptr);
    REQUIRE (xml->getChildByAttribute ("id", "customUnit") == nullptr);
}

TEST_CASE ("parameters are not host-automatable except bypass, which is the host bypass")
{
    Fixture f;
    for (const char* id : { ids::mode, ids::fraction, ids::calibration, ids::calibrationLevel })
        REQUIRE_FALSE (f.proc.apvts.getParameter (id)->isAutomatable());
    REQUIRE (f.proc.getBypassParameter() == f.proc.apvts.getParameter (ids::bypass));
}
```

Also in the test target section of `CMakeLists.txt` make no change yet (the target already compiles `${PITCHDELAY_PROCESSOR_SOURCES}` = `plugin/PluginProcessor.cpp`).

- [ ] **Step 3: Run to verify failure**

```bash
cmake --build build --target pitchdelay_processor_tests 2>&1 | grep error | head -5
```
Expected: FAIL — `ids::fraction`, `setDelayFrames`, etc. not found (the old processor and header are still in place).

- [ ] **Step 4: Replace `plugin/PluginProcessor.h`**

```cpp
#pragma once

#include <atomic>
#include <juce_audio_processors/juce_audio_processors.h>

#include "DelayLine.h"
#include "PulseGenerator.h"

class PitchDelayProcessor final : public juce::AudioProcessor,
                                  private juce::AudioProcessorValueTreeState::Listener
{
public:
    PitchDelayProcessor();
    ~PitchDelayProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    // Clears the delay buffer and the pulse phase so the next start begins from silence.
    void reset() override;
    bool isBusesLayoutSupported (const BusesLayout&) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    // The editor arrives in Task 4.
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }

    const juce::String getName() const override { return "PitchDelay"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override;

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    juce::AudioProcessorParameter* getBypassParameter() const override { return bypassParam; }

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // The delay in sample frames (what the editor fields show). Safe to read from any thread.
    int getCurrentDelayFrames() const noexcept { return delayFrames.load(); }
    // Sets the delay; clamped to [0, max]. Ignored before prepareToPlay (the rate is unknown).
    void setDelayFrames (int frames);

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void parameterChanged (const juce::String& parameterID, float newValue) override;
    void applyBaseDelay();
    void updateDsp();
    bool hostIsStopped() const;

    pitchdelay::DelayLine delayLine;
    pitchdelay::PulseGenerator pulse;
    bool calibrating = false;   // audio thread only: true while the pulse phase is running

    std::atomic<double> sampleRate_ { 0.0 };
    std::atomic<int> delayFrames { 0 };
    std::atomic<double> delayRate { 0.0 };     // rate delayFrames refers to; 0 = adopt the next prepare rate
    std::atomic<bool> delayIsSet { false };    // a delay exists that must be kept (not recomputed) on prepare
    std::atomic<double> pendingLegacyMs { -1.0 };   // first-release Custom ms waiting for a known rate

    juce::AudioParameterChoice* modeParam = nullptr;
    juce::AudioParameterChoice* fractionParam = nullptr;
    juce::AudioParameterBool* calibrationParam = nullptr;
    juce::AudioParameterFloat* calibrationLevelParam = nullptr;
    juce::AudioParameterBool* bypassParam = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PitchDelayProcessor)
};
```

- [ ] **Step 5: Replace `plugin/PluginProcessor.cpp`**

```cpp
#include "PluginProcessor.h"

#include "DelaySettings.h"
#include "ParameterIds.h"

using namespace pitchdelay;

namespace
{
const juce::Identifier delayFramesProperty { "delayFrames" };
const juce::Identifier delayRateProperty { "delayRate" };

struct LegacyCustom
{
    bool isCustom = false;
    bool inMilliseconds = true;
    double milliseconds = 0.0;
    int samples = 0;
};

double paramValue (const juce::ValueTree& tree, const char* id, double fallback)
{
    const auto child = tree.getChildWithProperty ("id", id);
    return child.isValid() ? (double) child.getProperty ("value", fallback) : fallback;
}

// Reads the first-release Custom settings from a saved state tree (mode 2 = Custom).
LegacyCustom readLegacyCustom (const juce::ValueTree& tree)
{
    LegacyCustom l;
    l.isCustom = juce::roundToInt (paramValue (tree, ids::mode, 0.0)) == 2;
    l.inMilliseconds = juce::roundToInt (paramValue (tree, ids::legacyCustomUnit, 0.0)) == 0;
    l.milliseconds = paramValue (tree, ids::legacyCustomMs, 900.0);
    l.samples = juce::roundToInt (paramValue (tree, ids::legacyCustomSamples, 0.0));
    return l;
}

// Drops the removed parameters so they are not saved again; Custom becomes the first speed.
void stripLegacy (juce::ValueTree tree, bool wasCustom)
{
    for (const char* id : { ids::legacyCustomUnit, ids::legacyCustomMs, ids::legacyCustomSamples })
    {
        auto child = tree.getChildWithProperty ("id", id);
        if (child.isValid())
            tree.removeChild (child, nullptr);
    }
    if (wasCustom)
    {
        auto mode = tree.getChildWithProperty ("id", ids::mode);
        if (mode.isValid())
            mode.setProperty ("value", 0, nullptr);
    }
}
}

juce::AudioProcessorValueTreeState::ParameterLayout PitchDelayProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<AudioParameterChoice> (
        ParameterID { ids::mode, 2 }, "Speed",
        StringArray { "33 1/3 RPM", "45 RPM" }, 0,
        AudioParameterChoiceAttributes().withAutomatable (false)));

    layout.add (std::make_unique<AudioParameterChoice> (
        ParameterID { ids::fraction, 1 }, "Fraction",
        StringArray { "1/1", "1/2", "1/4", "1/8", "1/16" }, kDefaultFractionIndex,
        AudioParameterChoiceAttributes().withAutomatable (false)));

    layout.add (std::make_unique<AudioParameterBool> (
        ParameterID { ids::calibration, 1 }, "Calibration", false,
        AudioParameterBoolAttributes().withAutomatable (false)));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { ids::calibrationLevel, 1 }, "Calibration level",
        NormalisableRange<float> (-60.0f, 0.0f, 0.5f), -20.0f,
        AudioParameterFloatAttributes().withLabel ("dB").withAutomatable (false)));

    layout.add (std::make_unique<AudioParameterBool> (
        ParameterID { ids::bypass, 1 }, "Bypass", false));

    return layout;
}

PitchDelayProcessor::PitchDelayProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PitchDelayState", createLayout())
{
    modeParam = dynamic_cast<juce::AudioParameterChoice*> (apvts.getParameter (ids::mode));
    fractionParam = dynamic_cast<juce::AudioParameterChoice*> (apvts.getParameter (ids::fraction));
    calibrationParam = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter (ids::calibration));
    calibrationLevelParam = dynamic_cast<juce::AudioParameterFloat*> (apvts.getParameter (ids::calibrationLevel));
    bypassParam = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter (ids::bypass));
    jassert (modeParam && fractionParam && calibrationParam && calibrationLevelParam && bypassParam);

    apvts.addParameterListener (ids::mode, this);
    apvts.addParameterListener (ids::fraction, this);
}

PitchDelayProcessor::~PitchDelayProcessor()
{
    apvts.removeParameterListener (ids::mode, this);
    apvts.removeParameterListener (ids::fraction, this);
}

bool PitchDelayProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return out == layouts.getMainInputChannelSet();
}

void PitchDelayProcessor::reset()
{
    delayLine.reset();
    pulse.reset();
    calibrating = false;
}

void PitchDelayProcessor::applyBaseDelay()
{
    const double sr = sampleRate_.load();
    if (sr <= 0.0)
        return;
    delayFrames.store (baseDelayFrames (static_cast<Speed> (modeParam->getIndex()),
                                        fractionParam->getIndex(), sr));
    delayRate.store (sr);
    delayIsSet.store (true);
    pendingLegacyMs.store (-1.0);
}

void PitchDelayProcessor::parameterChanged (const juce::String&, float)
{
    // Speed or Fraction changed: the delay follows them (overwriting any fine-tuning).
    applyBaseDelay();
}

void PitchDelayProcessor::setDelayFrames (int frames)
{
    const double sr = sampleRate_.load();
    if (sr <= 0.0)
        return;
    delayFrames.store (clampFrames (frames, sr));
    delayRate.store (sr);
    delayIsSet.store (true);
    pendingLegacyMs.store (-1.0);
}

void PitchDelayProcessor::prepareToPlay (double sampleRate, int)
{
    sampleRate_.store (sampleRate);

    const double legacyMs = pendingLegacyMs.load();
    if (legacyMs >= 0.0)
    {
        delayFrames.store (clampFrames (framesForMilliseconds (legacyMs, sampleRate), sampleRate));
        delayRate.store (sampleRate);
        delayIsSet.store (true);
        pendingLegacyMs.store (-1.0);
    }
    else if (delayIsSet.load())
    {
        const double from = delayRate.load();
        delayFrames.store (from > 0.0 ? rescaleFrames (delayFrames.load(), from, sampleRate)
                                      : clampFrames (delayFrames.load(), sampleRate));
        delayRate.store (sampleRate);
    }
    else
    {
        applyBaseDelay();
    }

    const int channels = std::max ({ 1, getTotalNumInputChannels(), getTotalNumOutputChannels() });
    delayLine.prepare (channels, maxDelayFrames (sampleRate));   // allocates and clears
    pulse.prepare (sampleRate);
    calibrating = false;
    updateDsp();
}

double PitchDelayProcessor::getTailLengthSeconds() const
{
    const double sr = sampleRate_.load();
    return sr > 0.0 ? static_cast<double> (getCurrentDelayFrames()) / sr : 0.0;
}

void PitchDelayProcessor::updateDsp()
{
    const int frames = delayFrames.load();
    delayLine.setDelayFrames (frames);
    pulse.setPeriodFrames (frames);
    pulse.setLevelDb (calibrationLevelParam->get());
}

bool PitchDelayProcessor::hostIsStopped() const
{
    // Hosts without transport information (e.g. Standalone) are treated as always playing.
    if (auto* playHead = getPlayHead())
        if (const auto position = playHead->getPosition())
            return ! position->getIsPlaying();
    return false;
}

void PitchDelayProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    // Deliberately NO juce::ScopedNoDenormals: the output must stay bit-identical to the input.
    if (bypassParam->get())
    {
        processBlockBypassed (buffer, midi);
        return;
    }
    if (hostIsStopped())
    {
        // Nothing may survive a stop: the next start must begin with a full delay of silence.
        delayLine.reset();
        pulse.reset();
        calibrating = false;
        buffer.clear();
        return;
    }

    updateDsp();
    const int numChannels = buffer.getNumChannels();
    const int numSamples = buffer.getNumSamples();

    if (calibrationParam->get() && numChannels > 0)
    {
        if (! calibrating)
        {
            pulse.reset();   // switched on: the first pulse starts at this block's first frame
            calibrating = true;
        }
        // Keep recording the incoming audio so leaving calibration is seamless.
        delayLine.record (buffer.getArrayOfReadPointers(), numChannels, numSamples);
        float* first = buffer.getWritePointer (0);
        pulse.generate (first, numSamples);
        for (int c = 1; c < numChannels; ++c)
            std::copy (first, first + numSamples, buffer.getWritePointer (c));
        return;
    }

    calibrating = false;
    delayLine.process (buffer.getArrayOfWritePointers(), numChannels, numSamples);
}

void PitchDelayProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    // Output stays the input (undelayed); keep recording so un-bypass is seamless,
    // except while the host is stopped, when the buffer and the pulse phase are wiped instead.
    if (hostIsStopped())
    {
        delayLine.reset();
        pulse.reset();
        calibrating = false;
        return;
    }
    updateDsp();
    delayLine.record (buffer.getArrayOfReadPointers(), buffer.getNumChannels(), buffer.getNumSamples());
}

void PitchDelayProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.setProperty (delayFramesProperty, delayFrames.load(), nullptr);
    state.setProperty (delayRateProperty, delayRate.load(), nullptr);
    if (const auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void PitchDelayProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes <= 0)
        return;
    const auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    auto tree = juce::ValueTree::fromXml (*xml);

    // Read what we need before the tree is handed to the parameters.
    const bool hasDelay = tree.hasProperty (delayFramesProperty) && tree.hasProperty (delayRateProperty);
    const int storedFrames = juce::jmax (0, (int) tree.getProperty (delayFramesProperty, 0));
    const double storedRate = (double) tree.getProperty (delayRateProperty, 0.0);
    const auto legacy = readLegacyCustom (tree);
    stripLegacy (tree, legacy.isCustom);

    apvts.replaceState (tree);   // may run the Speed/Fraction listener; resolved explicitly below

    const double sr = sampleRate_.load();
    if (hasDelay && storedRate > 0.0)
    {
        pendingLegacyMs.store (-1.0);
        delayIsSet.store (true);
        if (sr > 0.0)
        {
            delayFrames.store (rescaleFrames (storedFrames, storedRate, sr));
            delayRate.store (sr);
        }
        else
        {
            delayFrames.store (storedFrames);
            delayRate.store (storedRate);   // rescaled in prepareToPlay
        }
    }
    else if (legacy.isCustom && legacy.inMilliseconds)
    {
        if (sr > 0.0)
        {
            delayFrames.store (clampFrames (framesForMilliseconds (legacy.milliseconds, sr), sr));
            delayRate.store (sr);
            delayIsSet.store (true);
            pendingLegacyMs.store (-1.0);
        }
        else
        {
            pendingLegacyMs.store (juce::jmax (0.0, legacy.milliseconds));   // resolved in prepareToPlay
        }
    }
    else if (legacy.isCustom)
    {
        pendingLegacyMs.store (-1.0);
        delayIsSet.store (true);
        delayFrames.store (sr > 0.0 ? clampFrames (legacy.samples, sr) : juce::jmax (0, legacy.samples));
        delayRate.store (sr);   // 0 before prepare: kept as is when the rate becomes known
    }
    else
    {
        delayIsSet.store (false);
        applyBaseDelay();   // no-op before prepareToPlay, which then computes it
    }
}
```

- [ ] **Step 6: Remove the first-release API from DelaySettings**

In `core/DelaySettings.h` delete: `enum class Mode`, `enum class CustomUnit`, the constants `kRpm33`, `kRpm45`, `kMaxCustomMs`, `kMaxCustomSamples`, and the declarations of `framesForRpm` and `computeDelayFrames` (and their comments). Keep `kMaxDelaySeconds`, `maxDelayFrames`, `framesForMilliseconds` and everything added in Task 1.
In `core/DelaySettings.cpp` delete the definitions of `framesForRpm` and `computeDelayFrames`.
In `tests/DelaySettingsTests.cpp` delete the test cases `"half-revolution frame counts are exact at common sample rates"` and `"computeDelayFrames selects by mode and clamps"`.

- [ ] **Step 7: Stop compiling the old editor in the plugin target**

In `CMakeLists.txt` remove the line `plugin/PluginEditor.cpp` from `target_sources(PitchDelay PRIVATE ...)`. (The file stays on disk; Task 4 rewrites it.)

- [ ] **Step 8: Build and run all tests**

```bash
cmake -S . -B build >/dev/null && cmake --build build 2>&1 | grep -E "error|warning:" | head -10
ctest --test-dir build --output-on-failure 2>&1 | tail -6
```
Expected: no errors; both suites pass. If a Speed/Fraction test fails because the parameter listener did not fire, check that `Fixture::set` pumps the message loop (`runDispatchLoopUntil`) and that `ScopedJuceInitialiser_GUI` is alive; do not weaken the test.

- [ ] **Step 9: Validate the plugins and commit**

```bash
scripts/validate.sh build 2>&1 | tail -2
git add -A
git commit -m "feat: fractions, linked delay value, calibration mode and state migration" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
git push
```
Expected: `ALL VALIDATION PASSED`.

---

### Task 4: Editor — StepField, linked fields, calibration controls (TDD)

**Files:**
- Modify: `plugin/PluginEditor.h`, `plugin/PluginEditor.cpp` (full rewrite), `plugin/PluginProcessor.h`, `CMakeLists.txt`
- Create: `tests/EditorTests.cpp`

**Interfaces:**
- Consumes: `PitchDelayProcessor::{getCurrentDelayFrames, setDelayFrames, apvts}`, `getSampleRate()`, `pitchdelay::{framesForMilliseconds, millisecondsForFrames}`, `ids::*`.
- Produces:
```cpp
class StepField : public juce::Component {   // number box + ▲▼ (hold to repeat)
public:
    std::function<void (const juce::String&)> onTextEntered;
    std::function<void (int direction)> onStep;              // +1 / -1
    void setDisplayedText (const juce::String&);             // ignored while the user is editing
    juce::Label& getLabel(); juce::TextButton& getUpButton(); juce::TextButton& getDownButton();
};
class PitchDelayEditor : public juce::AudioProcessorEditor {
public:
    explicit PitchDelayEditor (PitchDelayProcessor&);
    void updateFromProcessor();                              // fields + readout (also run by a 15 Hz timer)
    StepField& getSamplesField(); StepField& getMillisecondsField();
    juce::String getReadoutText() const; juce::String getStatusText() const;
};
```

- [ ] **Step 1: Compile the editor into the test target and write the failing tests**

In `CMakeLists.txt` set
```cmake
set(PITCHDELAY_PROCESSOR_SOURCES plugin/PluginProcessor.cpp plugin/PluginEditor.cpp)
```
The plugin target already compiles `${PITCHDELAY_PROCESSOR_SOURCES}`, so it picks up the editor again from this line. Add `tests/EditorTests.cpp` to the test target's `target_sources`.

In `plugin/PluginProcessor.h` replace the two editor lines with
```cpp
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
```
(and delete the "The editor arrives in Task 4." comment).

Create `tests/EditorTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <juce_audio_processors/juce_audio_processors.h>

#include "DelaySettings.h"
#include "ParameterIds.h"
#include "PluginEditor.h"
#include "PluginProcessor.h"

namespace ids = pitchdelay::ids;

namespace
{
struct EditorFixture
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    PitchDelayProcessor proc;
    std::unique_ptr<PitchDelayEditor> editor;

    EditorFixture()
    {
        proc.setPlayConfigDetails (2, 2, 48000.0, 512);
        proc.prepareToPlay (48000.0, 512);
        editor = std::make_unique<PitchDelayEditor> (proc);
    }

    void typeSamples (const juce::String& text)
    {
        editor->getSamplesField().getLabel().setText (text, juce::sendNotificationSync);
    }
    void typeMilliseconds (const juce::String& text)
    {
        editor->getMillisecondsField().getLabel().setText (text, juce::sendNotificationSync);
    }
};
}

TEST_CASE ("the fields show the delay in samples and milliseconds, linked")
{
    EditorFixture f;
    f.editor->updateFromProcessor();
    REQUIRE (f.editor->getSamplesField().getLabel().getText() == "43200");
    REQUIRE (f.editor->getMillisecondsField().getLabel().getText() == "900.000");

    f.proc.setDelayFrames (43210);
    f.editor->updateFromProcessor();
    REQUIRE (f.editor->getSamplesField().getLabel().getText() == "43210");
    REQUIRE (f.editor->getMillisecondsField().getLabel().getText() == "900.208");
}

TEST_CASE ("typing samples sets the delay exactly")
{
    EditorFixture f;
    f.typeSamples ("43210");
    REQUIRE (f.proc.getCurrentDelayFrames() == 43210);
    f.typeSamples ("  7 ");
    REQUIRE (f.proc.getCurrentDelayFrames() == 7);
    f.typeSamples ("0043210");
    REQUIRE (f.proc.getCurrentDelayFrames() == 43210);
    REQUIRE (f.editor->getSamplesField().getLabel().getText() == "43210");   // canonical text restored
}

TEST_CASE ("typing milliseconds rounds to the nearest frame")
{
    EditorFixture f;
    f.typeMilliseconds ("900.5");
    REQUIRE (f.proc.getCurrentDelayFrames() == 43224);
    f.typeMilliseconds ("1000");
    REQUIRE (f.proc.getCurrentDelayFrames() == 48000);
    f.typeMilliseconds ("0,5");              // decimal comma
    REQUIRE (f.proc.getCurrentDelayFrames() == 24);
    f.typeMilliseconds ("0.0104");           // 0.4992 frames -> 0
    REQUIRE (f.proc.getCurrentDelayFrames() == 0);
}

TEST_CASE ("garbage typed into a field leaves the delay unchanged")
{
    EditorFixture f;
    f.proc.setDelayFrames (5000);
    for (const char* bad : { "abc", "", "   ", "-5", "12x", "1.5", "1e3", "--" })
    {
        INFO ("samples text '" << bad << "'");
        f.typeSamples (bad);
        REQUIRE (f.proc.getCurrentDelayFrames() == 5000);
    }
    for (const char* bad : { "abc", "", "-5", "1.2.3", "12x", "1e3" })
    {
        INFO ("ms text '" << bad << "'");
        f.typeMilliseconds (bad);
        REQUIRE (f.proc.getCurrentDelayFrames() == 5000);
    }
    REQUIRE (f.editor->getSamplesField().getLabel().getText() == "5000");   // display restored
}

TEST_CASE ("huge numbers clamp to the maximum delay instead of overflowing")
{
    EditorFixture f;
    f.typeSamples ("99999999999999999999");
    REQUIRE (f.proc.getCurrentDelayFrames() == 480000);
    f.typeMilliseconds ("99999999999999999999");
    REQUIRE (f.proc.getCurrentDelayFrames() == 480000);
}

TEST_CASE ("the sample buttons step by one frame and stop at zero")
{
    EditorFixture f;
    f.proc.setDelayFrames (100);
    f.editor->getSamplesField().getUpButton().triggerClick();
    REQUIRE (f.proc.getCurrentDelayFrames() == 101);
    f.editor->getSamplesField().getDownButton().triggerClick();
    f.editor->getSamplesField().getDownButton().triggerClick();
    REQUIRE (f.proc.getCurrentDelayFrames() == 99);

    f.proc.setDelayFrames (0);
    f.editor->getSamplesField().getDownButton().triggerClick();
    REQUIRE (f.proc.getCurrentDelayFrames() == 0);
}

TEST_CASE ("the millisecond buttons step by one millisecond")
{
    EditorFixture f;                       // 43200 frames = 900 ms at 48 kHz
    f.editor->getMillisecondsField().getUpButton().triggerClick();
    REQUIRE (f.proc.getCurrentDelayFrames() == 43248);
    f.editor->getMillisecondsField().getDownButton().triggerClick();
    REQUIRE (f.proc.getCurrentDelayFrames() == 43200);

    f.proc.setDelayFrames (10);            // less than 1 ms: down clamps to 0
    f.editor->getMillisecondsField().getDownButton().triggerClick();
    REQUIRE (f.proc.getCurrentDelayFrames() == 0);
}

TEST_CASE ("changing the speed through the parameter updates the fields")
{
    EditorFixture f;
    auto* p = f.proc.apvts.getParameter (ids::mode);
    p->setValueNotifyingHost (p->convertTo0to1 (1.0f));
    juce::MessageManager::getInstance()->runDispatchLoopUntil (5);
    f.editor->updateFromProcessor();
    REQUIRE (f.editor->getSamplesField().getLabel().getText() == "32000");
    REQUIRE (f.editor->getMillisecondsField().getLabel().getText() == "666.667");
}

TEST_CASE ("the readout shows the delay and the calibration status")
{
    EditorFixture f;
    f.editor->updateFromProcessor();
    REQUIRE (f.editor->getReadoutText() == "Delay: 43200 frames = 900.000 ms @ 48.0 kHz");
    REQUIRE (f.editor->getStatusText().isEmpty());

    auto* p = f.proc.apvts.getParameter (ids::calibration);
    p->setValueNotifyingHost (1.0f);
    f.editor->updateFromProcessor();
    REQUIRE (f.editor->getStatusText().contains ("CALIBRATION"));
    REQUIRE (f.editor->getStatusText().contains ("43200"));
}
```

- [ ] **Step 2: Run to verify failure**

```bash
cmake -S . -B build >/dev/null && cmake --build build --target pitchdelay_processor_tests 2>&1 | grep error | head -5
```
Expected: FAIL — the old `PluginEditor.h` has no `StepField`/`getSamplesField` (and does not compile against the new processor).

- [ ] **Step 3: Replace `plugin/PluginEditor.h`**

```cpp
#pragma once

#include <functional>
#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginProcessor.h"

// A number box with ▲/▼ buttons. Typing goes through the label; the buttons repeat while held.
class StepField final : public juce::Component
{
public:
    StepField();

    std::function<void (const juce::String&)> onTextEntered;   // user typed + committed text
    std::function<void (int direction)> onStep;                // +1 (▲) or -1 (▼)

    // Ignored while the user is editing the text, so a refresh never eats typing.
    void setDisplayedText (const juce::String& text);

    juce::Label& getLabel() noexcept { return field; }
    juce::TextButton& getUpButton() noexcept { return up; }
    juce::TextButton& getDownButton() noexcept { return down; }

    void resized() override;

private:
    juce::Label field;
    juce::TextButton up, down;
};

class PitchDelayEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit PitchDelayEditor (PitchDelayProcessor&);
    ~PitchDelayEditor() override = default;

    void resized() override;

    // Refreshes the fields and the readout from the processor (also run by the timer).
    void updateFromProcessor();

    StepField& getSamplesField() noexcept { return samplesField; }
    StepField& getMillisecondsField() noexcept { return millisecondsField; }
    juce::String getReadoutText() const { return readout.getText(); }
    juce::String getStatusText() const { return status.getText(); }

private:
    void timerCallback() override { updateFromProcessor(); }
    void samplesTyped (const juce::String& text);
    void millisecondsTyped (const juce::String& text);

    PitchDelayProcessor& proc;

    juce::Label speedLabel { {}, "Speed" }, fractionLabel { {}, "Fraction" },
                samplesLabel { {}, "Samples" }, millisecondsLabel { {}, "Milliseconds" },
                levelLabel { {}, "Pulse level" }, readout, status, latency;
    juce::ComboBox speedBox, fractionBox;
    StepField samplesField, millisecondsField;
    juce::ToggleButton calibrationToggle { "Calibration" };
    juce::Slider levelSlider;

    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> speedAtt, fractionAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> calibrationAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> levelAtt;
};
```

- [ ] **Step 4: Replace `plugin/PluginEditor.cpp`**

```cpp
#include "PluginEditor.h"

#include <algorithm>
#include <optional>

#include "DelaySettings.h"
#include "ParameterIds.h"

namespace
{
constexpr long long kHugeNumber = 2000000000LL;

// Digits only (surrounding spaces allowed). Anything else is rejected.
std::optional<long long> parseWholeNumber (const juce::String& text)
{
    const auto t = text.trim();
    if (t.isEmpty() || ! t.containsOnly ("0123456789"))
        return std::nullopt;
    if (t.length() > 12)
        return kHugeNumber;              // far above any maximum; clamped by the processor
    return std::min (t.getLargeIntValue(), kHugeNumber);
}

// A non-negative decimal with '.' or ',' as separator.
std::optional<double> parseDecimal (const juce::String& text)
{
    const auto t = text.trim().replaceCharacter (',', '.');
    if (t.isEmpty() || ! t.containsOnly ("0123456789.") || t.indexOfChar ('.') != t.lastIndexOfChar ('.'))
        return std::nullopt;
    return t.getDoubleValue();
}
}

StepField::StepField()
{
    field.setEditable (true);
    field.setJustificationType (juce::Justification::centredRight);
    field.setColour (juce::Label::outlineColourId, juce::Colours::grey);
    field.onTextChange = [this] { if (onTextEntered) onTextEntered (field.getText()); };

    up.setButtonText (juce::CharPointer_UTF8 ("\xe2\x96\xb2"));     // ▲
    down.setButtonText (juce::CharPointer_UTF8 ("\xe2\x96\xbc"));   // ▼
    for (auto* b : { &up, &down })
    {
        b->setTriggeredOnMouseDown (true);
        b->setRepeatSpeed (400, 60);
    }
    up.onClick = [this] { if (onStep) onStep (1); };
    down.onClick = [this] { if (onStep) onStep (-1); };

    addAndMakeVisible (field);
    addAndMakeVisible (up);
    addAndMakeVisible (down);
}

void StepField::setDisplayedText (const juce::String& text)
{
    if (! field.isBeingEdited())
        field.setText (text, juce::dontSendNotification);
}

void StepField::resized()
{
    auto area = getLocalBounds();
    down.setBounds (area.removeFromRight (30));
    up.setBounds (area.removeFromRight (30));
    area.removeFromRight (4);
    field.setBounds (area);
}

PitchDelayEditor::PitchDelayEditor (PitchDelayProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    speedBox.addItemList ({ "33 1/3 RPM", "45 RPM" }, 1);
    fractionBox.addItemList ({ "1/1", "1/2", "1/4", "1/8", "1/16" }, 1);

    levelSlider.setSliderStyle (juce::Slider::IncDecButtons);
    levelSlider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 80, 24);
    levelSlider.setTextValueSuffix (" dB");

    speedAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
        proc.apvts, pitchdelay::ids::mode, speedBox);
    fractionAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
        proc.apvts, pitchdelay::ids::fraction, fractionBox);
    calibrationAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        proc.apvts, pitchdelay::ids::calibration, calibrationToggle);
    levelAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        proc.apvts, pitchdelay::ids::calibrationLevel, levelSlider);

    samplesField.onTextEntered = [this] (const juce::String& t) { samplesTyped (t); };
    samplesField.onStep = [this] (int direction)
    {
        proc.setDelayFrames (proc.getCurrentDelayFrames() + direction);
        updateFromProcessor();
    };
    millisecondsField.onTextEntered = [this] (const juce::String& t) { millisecondsTyped (t); };
    millisecondsField.onStep = [this] (int direction)
    {
        const double sr = proc.getSampleRate();
        if (sr <= 0.0)
            return;
        const double ms = pitchdelay::millisecondsForFrames (proc.getCurrentDelayFrames(), sr)
                          + static_cast<double> (direction);
        proc.setDelayFrames (pitchdelay::framesForMilliseconds (std::max (0.0, ms), sr));
        updateFromProcessor();
    };

    latency.setText ("Reported latency: 0", juce::dontSendNotification);
    readout.setFont (juce::FontOptions (16.0f, juce::Font::bold));
    status.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    status.setColour (juce::Label::textColourId, juce::Colours::orange);

    for (juce::Component* c : std::initializer_list<juce::Component*> {
             &speedLabel, &speedBox, &fractionLabel, &fractionBox, &samplesLabel, &samplesField,
             &millisecondsLabel, &millisecondsField, &calibrationToggle, &levelLabel, &levelSlider,
             &readout, &status, &latency })
        addAndMakeVisible (c);

    setSize (460, 360);
    updateFromProcessor();
    startTimerHz (15);
}

void PitchDelayEditor::resized()
{
    auto area = getLocalBounds().reduced (16);
    const auto row = [&area] (juce::Component& label, juce::Component& control)
    {
        auto r = area.removeFromTop (28);
        label.setBounds (r.removeFromLeft (130));
        control.setBounds (r);
        area.removeFromTop (6);
    };
    row (speedLabel, speedBox);
    row (fractionLabel, fractionBox);
    row (samplesLabel, samplesField);
    row (millisecondsLabel, millisecondsField);
    area.removeFromTop (8);
    calibrationToggle.setBounds (area.removeFromTop (28));
    area.removeFromTop (6);
    row (levelLabel, levelSlider);
    area.removeFromTop (8);
    readout.setBounds (area.removeFromTop (28));
    status.setBounds (area.removeFromTop (24));
    latency.setBounds (area.removeFromTop (24));
}

void PitchDelayEditor::samplesTyped (const juce::String& text)
{
    if (const auto value = parseWholeNumber (text))
        proc.setDelayFrames (static_cast<int> (*value));
    updateFromProcessor();   // restores the canonical text, also after rejected input
}

void PitchDelayEditor::millisecondsTyped (const juce::String& text)
{
    const double sr = proc.getSampleRate();
    // 1.0e7 ms is far above the 10 s maximum and keeps the frame conversion away from overflow;
    // the processor clamps to the real maximum.
    if (const auto value = parseDecimal (text); value && sr > 0.0)
        proc.setDelayFrames (pitchdelay::framesForMilliseconds (std::min (*value, 1.0e7), sr));
    updateFromProcessor();
}

void PitchDelayEditor::updateFromProcessor()
{
    const int frames = proc.getCurrentDelayFrames();
    const double sr = proc.getSampleRate();
    const double ms = pitchdelay::millisecondsForFrames (frames, sr);

    samplesField.setDisplayedText (juce::String (frames));
    millisecondsField.setDisplayedText (juce::String (ms, 3));

    readout.setText ("Delay: " + juce::String (frames) + " frames = " + juce::String (ms, 3)
                         + " ms @ " + juce::String (sr / 1000.0, 1) + " kHz",
                     juce::dontSendNotification);

    const bool calibrating = proc.apvts.getRawParameterValue (pitchdelay::ids::calibration)->load() >= 0.5f;
    status.setText (calibrating ? "CALIBRATION: pulse every " + juce::String (frames) + " frames"
                                : juce::String(),
                    juce::dontSendNotification);
}

juce::AudioProcessorEditor* PitchDelayProcessor::createEditor()
{
    return new PitchDelayEditor (*this);
}
```

- [ ] **Step 5: Build and run the tests**

```bash
cmake --build build 2>&1 | grep -E "error|warning:" | head -10
ctest --test-dir build --output-on-failure 2>&1 | tail -6
```
Expected: both suites pass. If `"0.0104"` rounds differently than the test expects (0.4992 frames → 0), keep the test and fix the code, not the test.

- [ ] **Step 6: Validate (pluginval opens the editor) and commit**

```bash
scripts/validate.sh build 2>&1 | tail -2
git add -A
git commit -m "feat: editor with linked delay fields, fraction selector and calibration controls" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
git push
```
Expected: `ALL VALIDATION PASSED`.

---

### Task 5: Documentation, manual Logic steps and final check

**Files:**
- Modify: `docs/manual-logic-test.md`, `README.md`

- [ ] **Step 1: Extend `docs/manual-logic-test.md`**

Replace the numbered list's step 3 text "Mode = 33⅓ RPM (readout must show `Delay: 43200 frames = 900.000 ms`)" with "Speed = 33⅓ RPM, Fraction = 1/2 (readout must show `Delay: 43200 frames = 900.000 ms @ 48.0 kHz`)", change step 7 to "Repeat for 45 RPM (`--expect 32000`), for Fraction 1/4 at 33⅓ RPM (`--expect 21600`), and for a typed delay of 12345 samples (`--expect 12345`).", and append:

```markdown
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
```

- [ ] **Step 2: Update `README.md`**

Replace the "Use" paragraph's last sentence "Pick 33⅓ RPM or 45 RPM (or Custom) before cutting. Bypass passes audio undelayed." with:

```markdown
Pick the speed (33⅓ or 45 RPM) and the fraction of a revolution (1/2 by default; 1/1 … 1/16 for
testing). Fine-tune the delay in samples or milliseconds in the two fields. **Calibration** replaces
the audio with a 1 kHz tone burst every N frames (N = the delay, default level −20 dB) for measuring
the real platter speed. Bypass passes audio undelayed; a stopped transport silences the output and
clears the buffer.
```

- [ ] **Step 3: Final run on the whole branch, then commit**

```bash
cmake --build build 2>&1 | grep -E "error|warning:" | head -3
ctest --test-dir build --output-on-failure 2>&1 | tail -4
scripts/validate.sh build 2>&1 | tail -1
git add -A
git commit -m "docs: manual Logic steps for fractions, calibration and fine-tuning fields" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
git push
```
Expected: no build errors, `100% tests passed`, `ALL VALIDATION PASSED`.

- [ ] **Step 4: Hand over the manual checks**

Do **not** run the Logic procedure yourself. Tell the user the new build is installed in `~/Library/Audio/Plug-Ins/` (restart Logic, or `killall -9 AudioComponentRegistrar` if the old AU is still loaded) and ask them to run the three new manual sections (stop/restart, calibration pulses, fine-tuning fields) — the "typing is not overwritten" behaviour in particular needs a real window.

---

## Self-Review

**Spec coverage:** fractions + exact rational base delay + rounding cases → Task 1; rescale + clamp + ms conversion → Task 1; PulseGenerator (burst, period, truncation, reset, phase restart, determinism) → Task 2; parameters (mode v2, fraction, calibration, level, bypass, none automatable) → Task 3; delay stored as frames + rate in state, listener on Speed/Fraction, typed values, rescale on rate change → Task 3; calibration precedence (stopped > bypass > calibration), recording during calibration, mono-to-all-channels, switch-on phase reset → Task 3; first-release migration (33⅓, 45, Custom ms/samples, before/after prepare, stale params stripped) → Task 3; stop-wipe and bit-transparency tests retained → Task 3; tail length → Task 3; editor (linked fields, ▲▼ with repeat, typed entry, no sliders, fraction/speed combos, calibration toggle and level, readout/status) → Task 4; manual Logic tests for stop/restart, calibration, fields → Task 5; spec peak-wording fix → Task 2. Out-of-scope items (RPM analysis tool, adjustable burst, wipe on position jumps) are not planned.

**Placeholder scan:** none; the one draft/clean-version duplication in Task 4 Step 4 (`millisecondsTyped`) is explicitly resolved in the text.

**Type consistency:** `Speed`, `kFractionDivisors`, `kDefaultFractionIndex`, `baseDelayFrames`, `clampFrames`, `rescaleFrames`, `millisecondsForFrames`, `framesForMilliseconds` (Task 1 / existing) are used with identical signatures in Tasks 3–4; `PulseGenerator::{prepare,reset,setPeriodFrames,setLevelDb,generate,getBurstFrames}` (Task 2) match Task 3 usage; `PitchDelayProcessor::{getCurrentDelayFrames,setDelayFrames,apvts}` (Task 3) match Task 4 and its tests; ids come only from `pitchdelay::ids`.
