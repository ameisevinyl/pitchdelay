# pitchdelay Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build an AU + VST3 (+ Standalone) plugin that delays audio by an exact whole number of sample frames, 100% wet and bit-transparent, reporting 0 latency, with 33⅓ RPM / 45 RPM / Custom delay modes.

**Architecture:** A JUCE-free C++ core (`core/`: ring-buffer `DelayLine` + `DelaySettings` frame arithmetic) is unit-tested with Catch2. A thin JUCE shell (`plugin/`) owns parameters (APVTS), bypass, state and a minimal editor. The processor sources are compiled into both the plugin target and a headless Catch2 console-app test target (the pamplejuce pattern) — no intermediate static library holds JUCE modules.

**Tech Stack:** C++17, CMake ≥ 3.22 + Ninja, JUCE 9.0.3 (AGPLv3, fetched via FetchContent), Catch2 v3.16.0, pluginval, Apple `auval`, Python 3 (stdlib only) for the offset-measurement tool. macOS arm64 first.

**Spec:** `docs/superpowers/specs/2026-10-03-pitchdelay-design.md`

## Global Constraints

- Delay is a whole number of sample frames: `N = round(30 * sampleRate / rpm)` for speed modes; exactly 0.9 s (33⅓ RPM) and 2/3 s (45 RPM).
- 100% wet. No gain, filter, dither, interpolation, feedback or mix control. Output is bit-identical to input, including NaN payloads, infinities and denormals. **Never use `juce::ScopedNoDenormals` in the audio path.**
- `getLatencySamples()` must return 0 (the host must not compensate). Do not call `setLatencySamples`.
- Max delay 10 s (`ceil(10 * sampleRate)` frames); larger values clamp. Allocation only in `prepareToPlay`; none on the audio thread.
- Parameters: Mode {33⅓ RPM, 45 RPM, Custom}, Custom unit {Milliseconds, Samples}, Custom ms 0–10000 (rounded to nearest frame), Custom samples 0–1,920,000 (clamped to the current max, used as is, no rate scaling), Bypass. Mode/unit/ms/samples are **not** host-automatable.
- Host bypass is honoured: bypassed audio passes through undelayed while the input keeps being recorded, so un-bypassing outputs correctly delayed audio immediately.
- Delay change during playback is a hard jump of the read position (no smoothing).
- Buffer is cleared only in `prepareToPlay`/`DelayLine::prepare`. Do **not** override `AudioProcessor::reset()` (transport start/stop must not clear it).
- Mono and stereo only; 32-bit float only (no double precision).
- Formats: AU, VST3, Standalone. AU codes: manufacturer `Amvl`, plugin `Pdly`. Bundle id `com.ameisevinyl.pitchdelay`.
- Licence AGPLv3 (`LICENSE`), JUCE attribution in README.
- Every commit message ends with the trailer `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>`.
- JUCE headers for reference: after the first configure they are in `build/_deps/juce-src/modules/` (e.g. `juce_audio_processors_headless/` for parameter classes).

## Review Focus

Failure modes the spec implies but no obvious test covers; each has a test in the owning task.

1. **Bounce cuts the delayed tail** — the drive track's last N frames vanish unless the plugin reports a tail. Plugin reports `getTailLengthSeconds() == delay` (Task 4).
2. **Switching to a higher sample rate** — the ring is sized per prepare; reuse of a stale smaller buffer would crash or corrupt. Prepare at 44.1 kHz then 192 kHz then process (Task 4).
3. **Host block much larger than the ring** (small max delay, `numFrames > ring size`, multiple wraps) (Task 2).
4. **NaN / denormal / infinity pass-through** end to end through the processor — catches accidental denormal flushing (Task 4).
5. **Corrupt or foreign saved state** — `setStateInformation` with garbage bytes must not crash or change parameters (Task 4).

---

## File Structure

```
CMakeLists.txt                 build graph: core lib, plugin, tests
LICENSE                        AGPLv3
README.md                      what/why, build, test, Logic routing
core/DelayLine.h/.cpp          ring buffer; process() and record()
core/DelaySettings.h/.cpp      Mode/CustomUnit enums, frame arithmetic
plugin/ParameterIds.h          parameter id strings
plugin/PluginProcessor.h/.cpp  APVTS, bypass, state, wiring (no JucePlugin_ macros)
plugin/PluginEditor.h/.cpp     minimal editor; defines PitchDelayProcessor::createEditor
plugin/PluginEntry.cpp         createPluginFilter()
tests/DelayLineTests.cpp       core tests
tests/DelaySettingsTests.cpp   core tests
tests/ProcessorTests.cpp       headless processor tests (JUCE)
scripts/validate.sh            pluginval + auval
tools/make_impulse.py          test signal generator
tools/measure_offset.py        bounce offset measurement
docs/manual-logic-test.md      manual Logic procedure
.github/workflows/ci.yml       macOS build + tests + validation
```

---

### Task 1: Project skeleton and a loadable pass-through AU + VST3

**Files:**
- Create: `CMakeLists.txt`, `LICENSE`, `README.md`, `plugin/PluginProcessor.h`, `plugin/PluginProcessor.cpp`, `plugin/PluginEntry.cpp`, `scripts/validate.sh`
- Modify: `.gitignore` (already has `build/`)

**Interfaces:**
- Produces: CMake target `PitchDelay` (formats AU VST3 Standalone), class `PitchDelayProcessor` (replaced in Task 4), `scripts/validate.sh [builddir]`, CMake option `PITCHDELAY_INSTALL_AFTER_BUILD` (default ON), `PITCHDELAY_UNIVERSAL` (default OFF).

- [ ] **Step 1: Fetch the licence**

```bash
cd /Users/ameise/src/pitchdelay
curl -fsSL https://www.gnu.org/licenses/agpl-3.0.txt -o LICENSE
head -3 LICENSE
```
Expected: `GNU AFFERO GENERAL PUBLIC LICENSE` / `Version 3, 19 November 2007`.

- [ ] **Step 2: Write `CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.22)

# macOS settings must precede project().
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
option(PITCHDELAY_UNIVERSAL "Build arm64 + x86_64" OFF)
if(PITCHDELAY_UNIVERSAL)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
endif()

project(PitchDelay VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # for clangd

option(PITCHDELAY_INSTALL_AFTER_BUILD "Copy plugins to ~/Library/Audio/Plug-Ins after build" ON)

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 9.0.3
    GIT_SHALLOW TRUE)
FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.16.0
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE Catch2)

# ---- plugin -------------------------------------------------------------
juce_add_plugin(PitchDelay
    COMPANY_NAME "ameisevinyl"
    BUNDLE_ID com.ameisevinyl.pitchdelay
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    COPY_PLUGIN_AFTER_BUILD ${PITCHDELAY_INSTALL_AFTER_BUILD}
    PLUGIN_MANUFACTURER_CODE Amvl
    PLUGIN_CODE Pdly
    FORMATS AU VST3 Standalone
    PRODUCT_NAME "PitchDelay"
    AU_MAIN_TYPE kAudioUnitType_Effect
    VST3_CATEGORIES Fx Delay
    NEEDS_WEB_BROWSER FALSE
    NEEDS_CURL FALSE)

# Sources shared by the plugin and the headless test target.
set(PITCHDELAY_PROCESSOR_SOURCES plugin/PluginProcessor.cpp)

target_sources(PitchDelay PRIVATE
    ${PITCHDELAY_PROCESSOR_SOURCES}
    plugin/PluginEntry.cpp)
target_include_directories(PitchDelay PRIVATE plugin)
target_compile_definitions(PitchDelay PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)
target_link_libraries(PitchDelay
    PRIVATE juce::juce_audio_utils
    PUBLIC juce::juce_recommended_config_flags
           juce::juce_recommended_lto_flags
           juce::juce_recommended_warning_flags)
```

- [ ] **Step 3: Write the minimal pass-through processor**

`plugin/PluginProcessor.h`:
```cpp
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

class PitchDelayProcessor final : public juce::AudioProcessor
{
public:
    PitchDelayProcessor();

    void prepareToPlay (double, int) override {}
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }

    const juce::String getName() const override { return "PitchDelay"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}
};
```

`plugin/PluginProcessor.cpp`:
```cpp
#include "PluginProcessor.h"

PitchDelayProcessor::PitchDelayProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
}

bool PitchDelayProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return out == layouts.getMainInputChannelSet();
}
```

`plugin/PluginEntry.cpp`:
```cpp
#include "PluginProcessor.h"

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PitchDelayProcessor();
}
```

- [ ] **Step 4: Write `scripts/validate.sh`**

```bash
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
```

```bash
chmod +x scripts/validate.sh
```

- [ ] **Step 5: Configure and build**

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target PitchDelay_AU PitchDelay_VST3 PitchDelay_Standalone
```
Expected: configure fetches JUCE 9.0.3 and Catch2 (first run takes minutes), build ends with no errors. If a JUCE CMake option name is rejected, read `build/_deps/juce-src/docs/CMake API.md` and fix the name in `juce_add_plugin`.

- [ ] **Step 6: Validate**

```bash
scripts/validate.sh build
```
Expected: ends with `ALL VALIDATION PASSED`. pluginval prints `SUCCESS`; `auval` prints `AU VALIDATION SUCCEEDED.`. If `auval` cannot find the component, run `killall -9 AudioComponentRegistrar` and retry once; if it still fails, STOP and report (do not weaken the check).

- [ ] **Step 7: README and commit**

`README.md`:
```markdown
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
```

```bash
git add -A
git commit -m "feat: project skeleton with loadable pass-through AU/VST3" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
git push
```

---

### Task 2: `DelayLine` core (TDD)

**Files:**
- Create: `core/DelayLine.h`, `core/DelayLine.cpp`, `tests/DelayLineTests.cpp`
- Modify: `CMakeLists.txt` (append core lib + test target)

**Interfaces:**
- Produces (namespace `pitchdelay`):
```cpp
class DelayLine {
public:
    void prepare (int numChannels, int maxDelayFrames);   // allocates + clears, delay = 0
    void reset() noexcept;                                // clears audio, keeps delay
    void setDelayFrames (int frames) noexcept;            // clamped to [0, maxDelayFrames]
    int  getDelayFrames() const noexcept;
    int  getMaxDelayFrames() const noexcept;
    int  getNumChannels() const noexcept;
    void process (float* const* channels, int numChannels, int numFrames) noexcept;        // in place, delayed
    void record  (const float* const* channels, int numChannels, int numFrames) noexcept;  // buffer only, input untouched
};
```
Channels beyond the prepared count are left untouched; fewer channels than prepared is fine.

- [ ] **Step 1: Append the core library and test target to `CMakeLists.txt`**

```cmake
# ---- core (JUCE-free) -----------------------------------------------------
add_library(pitchdelay_core STATIC core/DelayLine.cpp)
target_include_directories(pitchdelay_core PUBLIC core)
target_compile_features(pitchdelay_core PUBLIC cxx_std_17)

enable_testing()
add_executable(pitchdelay_core_tests tests/DelayLineTests.cpp)
target_link_libraries(pitchdelay_core_tests PRIVATE pitchdelay_core Catch2::Catch2WithMain)
add_test(NAME core COMMAND pitchdelay_core_tests)
```
Create an empty stub `core/DelayLine.cpp` containing only `#include "DelayLine.h"` and a minimal `core/DelayLine.h` with `#pragma once` is NOT enough to compile the tests — do Step 2 first; the build is expected to fail until Step 3.

- [ ] **Step 2: Write the failing tests `tests/DelayLineTests.cpp`**

```cpp
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

#include "DelayLine.h"

using pitchdelay::DelayLine;

namespace
{
uint32_t bitsOf (float f) { uint32_t u; std::memcpy (&u, &f, 4); return u; }
float fromBits (uint32_t u) { float f; std::memcpy (&f, &u, 4); return f; }

// Runs mono `input` through `d` in blocks of `blockSize`; returns the output.
std::vector<float> run (DelayLine& d, const std::vector<float>& input, int blockSize)
{
    std::vector<float> out = input;
    for (size_t pos = 0; pos < out.size(); pos += (size_t) blockSize)
    {
        const int n = (int) std::min<size_t> ((size_t) blockSize, out.size() - pos);
        float* ch[1] = { out.data() + pos };
        d.process (ch, 1, n);
    }
    return out;
}
}

TEST_CASE ("an impulse comes out exactly N frames later and nowhere else")
{
    const int N = GENERATE (0, 1, 5, 100);
    const int block = GENERATE (1, 7, 64, 512);

    DelayLine d;
    d.prepare (1, 1000);
    d.setDelayFrames (N);

    std::vector<float> in (N + 300, 0.0f);
    in[3] = 1.0f;
    const auto out = run (d, in, block);

    for (size_t i = 0; i < out.size(); ++i)
        REQUIRE (out[i] == (i == (size_t) (3 + N) ? 1.0f : 0.0f));
}

TEST_CASE ("output is bit-identical to input, including NaN payloads, inf, denormals, -0")
{
    const int N = 37;
    DelayLine d;
    d.prepare (1, 100);
    d.setDelayFrames (N);

    std::vector<float> in;
    for (uint32_t special : { 0x7fc12345u, 0x7f800001u, 0x7f800000u, 0xff800000u,
                              0x00000001u, 0x007fffffu, 0x80000000u, 0x80000001u })
        in.push_back (fromBits (special));
    std::mt19937 rng (1234);
    for (int i = 0; i < 5000; ++i)
        in.push_back (fromBits ((uint32_t) rng()));

    const auto out = run (d, in, 64);

    for (size_t i = 0; i < in.size(); ++i)
    {
        const uint32_t expected = i < (size_t) N ? 0u : bitsOf (in[i - (size_t) N]);
        REQUIRE (bitsOf (out[i]) == expected);
    }
}

TEST_CASE ("block much larger than the ring wraps correctly")
{
    DelayLine d;
    d.prepare (1, 10);             // ring of 11 frames
    d.setDelayFrames (3);

    std::vector<float> in (500);
    for (size_t i = 0; i < in.size(); ++i) in[i] = (float) (i + 1);
    const auto out = run (d, in, 500);   // one block of 500 frames

    for (size_t i = 0; i < in.size(); ++i)
        REQUIRE (out[i] == (i < 3 ? 0.0f : in[i - 3]));
}

TEST_CASE ("maximum delay works and larger requests clamp")
{
    DelayLine d;
    d.prepare (1, 1000);
    d.setDelayFrames (5000);
    REQUIRE (d.getDelayFrames() == 1000);
    d.setDelayFrames (-4);
    REQUIRE (d.getDelayFrames() == 0);
    d.setDelayFrames (1000);

    std::vector<float> in (1200, 0.0f);
    in[0] = 1.0f;
    const auto out = run (d, in, 128);
    for (size_t i = 0; i < out.size(); ++i)
        REQUIRE (out[i] == (i == 1000 ? 1.0f : 0.0f));
}

TEST_CASE ("channels are independent and share one delay")
{
    DelayLine d;
    d.prepare (2, 100);
    d.setDelayFrames (10);

    std::vector<float> l (50), r (50);
    for (int i = 0; i < 50; ++i) { l[(size_t) i] = (float) (i + 1); r[(size_t) i] = -(float) (i + 1); }
    float* ch[2] = { l.data(), r.data() };
    d.process (ch, 2, 50);

    for (int i = 0; i < 50; ++i)
    {
        REQUIRE (l[(size_t) i] == (i < 10 ? 0.0f : (float) (i - 10 + 1)));
        REQUIRE (r[(size_t) i] == (i < 10 ? 0.0f : -(float) (i - 10 + 1)));
    }
}

TEST_CASE ("changing the delay is a hard jump of the read position")
{
    DelayLine d;
    d.prepare (1, 100);
    d.setDelayFrames (10);

    std::vector<float> a (20);
    for (size_t i = 0; i < 20; ++i) a[i] = (float) (i + 1);     // frames 0..19
    float* pa[1] = { a.data() };
    d.process (pa, 1, 20);

    d.setDelayFrames (4);
    std::vector<float> b (5);
    for (size_t i = 0; i < 5; ++i) b[i] = (float) (21 + i);     // frames 20..24
    float* pb[1] = { b.data() };
    d.process (pb, 1, 5);

    // output at frame 20+k is the input from frame 20+k-4
    for (size_t k = 0; k < 5; ++k)
        REQUIRE (b[k] == (float) (20 + k - 4 + 1));
}

TEST_CASE ("record() keeps the input and fills the buffer for a later process()")
{
    DelayLine d;
    d.prepare (1, 100);
    d.setDelayFrames (10);

    std::vector<float> a (20);
    for (size_t i = 0; i < 20; ++i) a[i] = (float) (i + 1);
    const std::vector<float> copy = a;
    const float* ra[1] = { a.data() };
    d.record (ra, 1, 20);
    REQUIRE (a == copy);

    std::vector<float> zeros (10, 0.0f);
    float* pz[1] = { zeros.data() };
    d.process (pz, 1, 10);
    for (size_t k = 0; k < 10; ++k)
        REQUIRE (zeros[k] == copy[10 + k]);   // frames 10..19 recorded earlier
}

TEST_CASE ("reset() clears audio but keeps the delay")
{
    DelayLine d;
    d.prepare (1, 100);
    d.setDelayFrames (10);
    std::vector<float> a (30, 1.0f);
    run (d, a, 30);
    d.reset();
    REQUIRE (d.getDelayFrames() == 10);
    const auto out = run (d, std::vector<float> (10, 0.0f), 10);
    for (float v : out) REQUIRE (v == 0.0f);
}

TEST_CASE ("channel count mismatches are safe")
{
    DelayLine d;
    d.prepare (2, 100);
    d.setDelayFrames (5);

    // more channels than prepared: the extra channel is left untouched
    std::vector<float> a (10, 1.0f), b (10, 2.0f), c (10, 3.0f);
    float* three[3] = { a.data(), b.data(), c.data() };
    d.process (three, 3, 10);
    for (float v : c) REQUIRE (v == 3.0f);

    // fewer channels than prepared: processes without touching memory it does not own
    std::vector<float> m (10, 1.0f);
    float* one[1] = { m.data() };
    d.process (one, 1, 10);
}
```

- [ ] **Step 3: Run to verify failure**

```bash
cmake -S . -B build && cmake --build build --target pitchdelay_core_tests 2>&1 | tail -5
```
Expected: FAIL — `DelayLine.h: No such file` (or undefined symbols).

- [ ] **Step 4: Implement `core/DelayLine.h` and `core/DelayLine.cpp`**

`core/DelayLine.h`:
```cpp
#pragma once
#include <vector>

namespace pitchdelay
{
// Exact-frame, bit-transparent multichannel delay line.
// Only copies samples: no gain, filtering, interpolation or feedback.
// All delays are in whole sample frames. Allocation happens only in prepare().
class DelayLine
{
public:
    void prepare (int numChannels, int maxDelayFrames);
    void reset() noexcept;

    void setDelayFrames (int frames) noexcept;
    int getDelayFrames() const noexcept { return delay_; }
    int getMaxDelayFrames() const noexcept { return maxDelay_; }
    int getNumChannels() const noexcept { return numChannels_; }

    // In place: every channel becomes itself delayed by getDelayFrames().
    void process (float* const* channels, int numChannels, int numFrames) noexcept;
    // Writes the input into the buffer and leaves the channels untouched.
    void record (const float* const* channels, int numChannels, int numFrames) noexcept;

private:
    std::vector<float> buffer_;   // numChannels_ rings of size_ frames
    int numChannels_ = 0;
    int maxDelay_ = 0;
    int size_ = 1;                // maxDelay_ + 1: write-then-read makes delay 0 valid
    int delay_ = 0;
    int writePos_ = 0;
};
}
```

`core/DelayLine.cpp`:
```cpp
#include "DelayLine.h"

#include <algorithm>

namespace pitchdelay
{
void DelayLine::prepare (int numChannels, int maxDelayFrames)
{
    numChannels_ = std::max (0, numChannels);
    maxDelay_ = std::max (0, maxDelayFrames);
    size_ = maxDelay_ + 1;
    buffer_.assign (static_cast<size_t> (numChannels_) * static_cast<size_t> (size_), 0.0f);
    delay_ = 0;
    writePos_ = 0;
}

void DelayLine::reset() noexcept
{
    std::fill (buffer_.begin(), buffer_.end(), 0.0f);
    writePos_ = 0;
}

void DelayLine::setDelayFrames (int frames) noexcept
{
    delay_ = std::clamp (frames, 0, maxDelay_);
}

void DelayLine::process (float* const* channels, int numChannels, int numFrames) noexcept
{
    const int n = std::min (numChannels, numChannels_);
    for (int c = 0; c < n; ++c)
    {
        float* ring = buffer_.data() + static_cast<size_t> (c) * static_cast<size_t> (size_);
        float* io = channels[c];
        int w = writePos_;
        int r = w - delay_;
        if (r < 0) r += size_;
        for (int i = 0; i < numFrames; ++i)
        {
            ring[w] = io[i];     // write first, so delay 0 reads the sample just written
            io[i] = ring[r];
            if (++w == size_) w = 0;
            if (++r == size_) r = 0;
        }
    }
    writePos_ = static_cast<int> ((static_cast<long long> (writePos_) + numFrames) % size_);
}

void DelayLine::record (const float* const* channels, int numChannels, int numFrames) noexcept
{
    const int n = std::min (numChannels, numChannels_);
    for (int c = 0; c < n; ++c)
    {
        float* ring = buffer_.data() + static_cast<size_t> (c) * static_cast<size_t> (size_);
        const float* in = channels[c];
        int w = writePos_;
        for (int i = 0; i < numFrames; ++i)
        {
            ring[w] = in[i];
            if (++w == size_) w = 0;
        }
    }
    writePos_ = static_cast<int> ((static_cast<long long> (writePos_) + numFrames) % size_);
}
}
```

- [ ] **Step 5: Run to verify pass**

```bash
cmake --build build --target pitchdelay_core_tests && ./build/pitchdelay_core_tests
```
Expected: `All tests passed`. If the bit-exactness test fails on signaling NaN (0x7f800001), the compiler quieted it during the copy: replace the float copies in `process`/`record` with `std::memcpy` of 4 bytes and re-run — do not relax the test.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "feat: bit-transparent DelayLine core with tests" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
git push
```

---

### Task 3: `DelaySettings` frame arithmetic (TDD)

**Files:**
- Create: `core/DelaySettings.h`, `core/DelaySettings.cpp`, `tests/DelaySettingsTests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: nothing.
- Produces (namespace `pitchdelay`):
```cpp
enum class Mode { Rpm33 = 0, Rpm45 = 1, Custom = 2 };           // matches choice index order
enum class CustomUnit { Milliseconds = 0, Samples = 1 };
inline constexpr double kRpm33 = 100.0 / 3.0;
inline constexpr double kRpm45 = 45.0;
inline constexpr double kMaxDelaySeconds = 10.0;
inline constexpr double kMaxCustomMs = 10000.0;
inline constexpr int    kMaxCustomSamples = 1920000;            // 10 s at 192 kHz
int maxDelayFrames (double sampleRate);                         // ceil(10 * sr)
int framesForRpm (double rpm, double sampleRate);               // round(30 * sr / rpm), half revolution
int framesForMilliseconds (double ms, double sampleRate);       // round(ms * sr / 1000)
int computeDelayFrames (Mode, CustomUnit, double customMs, int customSamples, double sampleRate); // clamped to [0, maxDelayFrames]
```

- [ ] **Step 1: Write the failing tests `tests/DelaySettingsTests.cpp`**

```cpp
#include <catch2/catch_test_macros.hpp>
#include <cmath>

#include "DelaySettings.h"

using namespace pitchdelay;

TEST_CASE ("half-revolution frame counts are exact at common sample rates")
{
    struct Row { double sr; int f33; int f45; };
    const Row table[] = {
        { 44100.0,  39690,  29400 },
        { 48000.0,  43200,  32000 },
        { 88200.0,  79380,  58800 },
        { 96000.0,  86400,  64000 },
        { 176400.0, 158760, 117600 },
        { 192000.0, 172800, 128000 },
    };
    for (const auto& r : table)
    {
        INFO ("sample rate " << r.sr);
        REQUIRE (framesForRpm (kRpm33, r.sr) == r.f33);
        REQUIRE (framesForRpm (kRpm45, r.sr) == r.f45);
        // the unrounded value really is an integer (no hidden rounding)
        const double raw33 = 30.0 * r.sr / kRpm33;
        const double raw45 = 30.0 * r.sr / kRpm45;
        REQUIRE (std::abs (raw33 - std::round (raw33)) < 1e-6);
        REQUIRE (std::abs (raw45 - std::round (raw45)) < 1e-6);
    }
}

TEST_CASE ("milliseconds convert to the nearest frame")
{
    REQUIRE (framesForMilliseconds (900.0, 44100.0) == 39690);
    REQUIRE (framesForMilliseconds (1.0, 44100.0) == 44);      // 44.1 -> 44
    REQUIRE (framesForMilliseconds (0.0, 48000.0) == 0);
}

TEST_CASE ("max delay is ten seconds, rounded up")
{
    REQUIRE (maxDelayFrames (44100.0) == 441000);
    REQUIRE (maxDelayFrames (192000.0) == 1920000);
}

TEST_CASE ("computeDelayFrames selects by mode and clamps")
{
    const double sr = 44100.0;
    REQUIRE (computeDelayFrames (Mode::Rpm33, CustomUnit::Milliseconds, 5.0, 7, sr) == 39690);
    REQUIRE (computeDelayFrames (Mode::Rpm45, CustomUnit::Samples, 5.0, 7, sr) == 29400);
    REQUIRE (computeDelayFrames (Mode::Custom, CustomUnit::Milliseconds, 900.0, 7, sr) == 39690);
    // custom samples are used as is, independent of the sample rate
    REQUIRE (computeDelayFrames (Mode::Custom, CustomUnit::Samples, 5.0, 12345, sr) == 12345);
    REQUIRE (computeDelayFrames (Mode::Custom, CustomUnit::Samples, 5.0, 12345, 96000.0) == 12345);
    // clamping
    REQUIRE (computeDelayFrames (Mode::Custom, CustomUnit::Milliseconds, 20000.0, 0, sr) == 441000);
    REQUIRE (computeDelayFrames (Mode::Custom, CustomUnit::Milliseconds, -3.0, 0, sr) == 0);
    REQUIRE (computeDelayFrames (Mode::Custom, CustomUnit::Samples, 0.0, 1920000, sr) == 441000);
    REQUIRE (computeDelayFrames (Mode::Custom, CustomUnit::Samples, 0.0, -5, sr) == 0);
}
```

- [ ] **Step 2: Wire into CMake**

Replace the core lib/test lines in `CMakeLists.txt`:
```cmake
add_library(pitchdelay_core STATIC core/DelayLine.cpp core/DelaySettings.cpp)
```
```cmake
add_executable(pitchdelay_core_tests tests/DelayLineTests.cpp tests/DelaySettingsTests.cpp)
```
Create `core/DelaySettings.cpp` containing only `#include "DelaySettings.h"` and a header with `#pragma once` so CMake configures; the build must still fail (undefined names) — verify in Step 3.

- [ ] **Step 3: Run to verify failure**

```bash
cmake -S . -B build && cmake --build build --target pitchdelay_core_tests 2>&1 | tail -5
```
Expected: FAIL — `framesForRpm` not declared.

- [ ] **Step 4: Implement**

`core/DelaySettings.h`:
```cpp
#pragma once

namespace pitchdelay
{
enum class Mode { Rpm33 = 0, Rpm45 = 1, Custom = 2 };
enum class CustomUnit { Milliseconds = 0, Samples = 1 };

inline constexpr double kRpm33 = 100.0 / 3.0;
inline constexpr double kRpm45 = 45.0;
inline constexpr double kMaxDelaySeconds = 10.0;
inline constexpr double kMaxCustomMs = 10000.0;
inline constexpr int kMaxCustomSamples = 1920000;   // 10 s at 192 kHz

int maxDelayFrames (double sampleRate);
// Half a revolution of the lathe platter: round(30 * sampleRate / rpm) frames.
int framesForRpm (double rpm, double sampleRate);
int framesForMilliseconds (double ms, double sampleRate);
// Resolves the current settings to a frame count clamped to [0, maxDelayFrames].
int computeDelayFrames (Mode mode, CustomUnit unit, double customMs, int customSamples,
                        double sampleRate);
}
```

`core/DelaySettings.cpp`:
```cpp
#include "DelaySettings.h"

#include <algorithm>
#include <cmath>

namespace pitchdelay
{
int maxDelayFrames (double sampleRate)
{
    return static_cast<int> (std::ceil (kMaxDelaySeconds * sampleRate));
}

int framesForRpm (double rpm, double sampleRate)
{
    if (rpm <= 0.0) return 0;
    return static_cast<int> (std::llround (30.0 * sampleRate / rpm));
}

int framesForMilliseconds (double ms, double sampleRate)
{
    return static_cast<int> (std::llround (ms * sampleRate / 1000.0));
}

int computeDelayFrames (Mode mode, CustomUnit unit, double customMs, int customSamples,
                        double sampleRate)
{
    int frames = 0;
    switch (mode)
    {
        case Mode::Rpm33: frames = framesForRpm (kRpm33, sampleRate); break;
        case Mode::Rpm45: frames = framesForRpm (kRpm45, sampleRate); break;
        case Mode::Custom:
            frames = unit == CustomUnit::Milliseconds
                         ? framesForMilliseconds (std::clamp (customMs, 0.0, kMaxCustomMs), sampleRate)
                         : customSamples;
            break;
    }
    return std::clamp (frames, 0, maxDelayFrames (sampleRate));
}
}
```

- [ ] **Step 5: Run to verify pass**

```bash
cmake --build build --target pitchdelay_core_tests && ./build/pitchdelay_core_tests
```
Expected: `All tests passed`.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "feat: DelaySettings frame arithmetic with RPM table tests" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
git push
```

---

### Task 4: Processor — parameters, bypass, state, DSP wiring, headless tests

**Files:**
- Create: `plugin/ParameterIds.h`, `tests/ProcessorTests.cpp`
- Modify: `plugin/PluginProcessor.h`, `plugin/PluginProcessor.cpp` (full rewrite), `CMakeLists.txt`, `docs/superpowers/specs/2026-10-03-pitchdelay-design.md`

**Interfaces:**
- Consumes: `pitchdelay::DelayLine`, `pitchdelay::computeDelayFrames`, `Mode`, `CustomUnit`, `maxDelayFrames`, `kMaxCustomMs`, `kMaxCustomSamples`.
- Produces: `class PitchDelayProcessor` with public `juce::AudioProcessorValueTreeState apvts;`, `int getCurrentDelayFrames() const noexcept;`, ids in `pitchdelay::ids` (`mode`, `customUnit`, `customMs`, `customSamples`, `bypass`), `getBypassParameter()` returns the bypass parameter, `getTailLengthSeconds()` = current delay in seconds.

- [ ] **Step 1: Write `plugin/ParameterIds.h`**

```cpp
#pragma once

namespace pitchdelay::ids
{
inline constexpr const char* mode = "mode";
inline constexpr const char* customUnit = "customUnit";
inline constexpr const char* customMs = "customMs";
inline constexpr const char* customSamples = "customSamples";
inline constexpr const char* bypass = "bypass";
}
```

- [ ] **Step 2: Add the processor test target to `CMakeLists.txt`** (append)

```cmake
# ---- headless processor tests ---------------------------------------------
juce_add_console_app(pitchdelay_processor_tests PRODUCT_NAME "pitchdelay_processor_tests")
target_sources(pitchdelay_processor_tests PRIVATE
    ${PITCHDELAY_PROCESSOR_SOURCES}
    tests/ProcessorTests.cpp)
target_include_directories(pitchdelay_processor_tests PRIVATE plugin)
target_compile_definitions(pitchdelay_processor_tests PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(pitchdelay_processor_tests PRIVATE
    pitchdelay_core
    juce::juce_audio_processors
    juce::juce_recommended_config_flags
    Catch2::Catch2WithMain)
add_test(NAME processor COMMAND pitchdelay_processor_tests)
```
And make the plugin link the core: add `pitchdelay_core` to the `PitchDelay` `PRIVATE` libs:
```cmake
target_link_libraries(PitchDelay PRIVATE pitchdelay_core)
```
Core library definitions must precede these lines — move the `# ---- core` block above `# ---- plugin` if needed.

- [ ] **Step 3: Write the failing tests `tests/ProcessorTests.cpp`**

```cpp
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <juce_audio_processors/juce_audio_processors.h>

#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

#include "ParameterIds.h"
#include "PluginProcessor.h"

namespace
{
uint32_t bitsOf (float f) { uint32_t u; std::memcpy (&u, &f, 4); return u; }
float fromBits (uint32_t u) { float f; std::memcpy (&f, &u, 4); return f; }

struct Fixture
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    PitchDelayProcessor proc;
    juce::MidiBuffer midi;

    explicit Fixture (double sr = 48000.0, int block = 512, int channels = 2)
    {
        proc.setPlayConfigDetails (channels, channels, sr, block);
        proc.prepareToPlay (sr, block);
    }

    void set (const char* id, float plainValue)
    {
        auto* p = proc.apvts.getParameter (id);
        p->setValueNotifyingHost (p->convertTo0to1 (plainValue));
    }

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

int firstNonZero (const std::vector<float>& v)
{
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i] != 0.0f) return (int) i;
    return -1;
}
}

TEST_CASE ("reports zero latency")
{
    Fixture f;
    REQUIRE (f.proc.getLatencySamples() == 0);
}

TEST_CASE ("33 1/3 RPM at 48 kHz delays by exactly 43200 frames")
{
    Fixture f (48000.0);
    f.set (pitchdelay::ids::mode, 0);
    std::vector<float> in (43200 + 2000, 0.0f);
    in[10] = 1.0f;
    const auto out = f.run (in);
    REQUIRE (firstNonZero (out) == 10 + 43200);
    int nonZero = 0;
    for (float v : out) nonZero += v != 0.0f;
    REQUIRE (nonZero == 1);
    REQUIRE (f.proc.getCurrentDelayFrames() == 43200);
}

TEST_CASE ("45 RPM at 96 kHz delays by exactly 64000 frames")
{
    Fixture f (96000.0);
    f.set (pitchdelay::ids::mode, 1);
    std::vector<float> in (64000 + 1000, 0.0f);
    in[0] = 1.0f;
    REQUIRE (firstNonZero (f.run (in)) == 64000);
}

TEST_CASE ("custom milliseconds and custom samples")
{
    Fixture f (44100.0);
    f.set (pitchdelay::ids::mode, 2);
    f.set (pitchdelay::ids::customUnit, 0);
    f.set (pitchdelay::ids::customMs, 900.0f);
    std::vector<float> in (100, 0.0f);
    f.run (in);   // applies the parameters
    REQUIRE (f.proc.getCurrentDelayFrames() == 39690);

    f.set (pitchdelay::ids::customUnit, 1);
    f.set (pitchdelay::ids::customSamples, 12345.0f);
    f.run (in);
    REQUIRE (f.proc.getCurrentDelayFrames() == 12345);
}

TEST_CASE ("tail length equals the delay so bounces keep the delayed end")
{
    Fixture f (48000.0);
    f.set (pitchdelay::ids::mode, 0);
    f.run (std::vector<float> (64, 0.0f));
    REQUIRE (f.proc.getTailLengthSeconds() == Catch::Approx (0.9).margin (1e-9));
}

TEST_CASE ("switching to a higher sample rate re-allocates and still works")
{
    Fixture f (44100.0);
    f.set (pitchdelay::ids::mode, 1);
    f.run (std::vector<float> (1000, 0.5f));
    f.proc.setPlayConfigDetails (2, 2, 192000.0, 512);
    f.proc.prepareToPlay (192000.0, 512);
    std::vector<float> in (128000 + 100, 0.0f);
    in[0] = 1.0f;
    REQUIRE (firstNonZero (f.run (in)) == 128000);
}

TEST_CASE ("bypass passes audio undelayed and un-bypass is immediately correctly delayed")
{
    Fixture f (48000.0);
    f.set (pitchdelay::ids::mode, 2);
    f.set (pitchdelay::ids::customUnit, 1);
    f.set (pitchdelay::ids::customSamples, 100.0f);

    std::vector<float> ramp (1100);
    for (size_t i = 0; i < ramp.size(); ++i) ramp[i] = (float) (i + 1);

    f.set (pitchdelay::ids::bypass, 1.0f);
    const std::vector<float> first (ramp.begin(), ramp.begin() + 1000);
    REQUIRE (f.run (first) == first);                 // undelayed

    f.set (pitchdelay::ids::bypass, 0.0f);
    const std::vector<float> rest (ramp.begin() + 1000, ramp.end());
    const auto out = f.run (rest);                    // frames 1000..1099
    for (size_t k = 0; k < out.size(); ++k)
        REQUIRE (out[k] == ramp[1000 + k - 100]);     // taken from the bypassed period
    REQUIRE (f.proc.getLatencySamples() == 0);
}

TEST_CASE ("NaN, inf and denormals pass through bit-identically")
{
    Fixture f (48000.0);
    f.set (pitchdelay::ids::mode, 2);
    f.set (pitchdelay::ids::customUnit, 1);
    f.set (pitchdelay::ids::customSamples, 100.0f);

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
    f.set (pitchdelay::ids::mode, 2);
    f.set (pitchdelay::ids::customUnit, 1);
    f.set (pitchdelay::ids::customSamples, 10.0f);
    std::vector<float> in (50, 0.0f);
    in[0] = 1.0f;
    REQUIRE (firstNonZero (f.run (in, 64, 1)) == 10);
}

TEST_CASE ("state round-trips")
{
    Fixture a;
    a.set (pitchdelay::ids::mode, 2);
    a.set (pitchdelay::ids::customUnit, 1);
    a.set (pitchdelay::ids::customSamples, 777.0f);
    juce::MemoryBlock state;
    a.proc.getStateInformation (state);

    Fixture b;
    b.proc.setStateInformation (state.getData(), (int) state.getSize());
    REQUIRE (b.proc.apvts.getRawParameterValue (pitchdelay::ids::mode)->load() == 2.0f);
    REQUIRE (b.proc.apvts.getRawParameterValue (pitchdelay::ids::customUnit)->load() == 1.0f);
    REQUIRE (b.proc.apvts.getRawParameterValue (pitchdelay::ids::customSamples)->load() == 777.0f);
}

TEST_CASE ("corrupt saved state is ignored")
{
    Fixture f;
    f.set (pitchdelay::ids::mode, 1);
    const char garbage[] = "this is not a valid state blob";
    f.proc.setStateInformation (garbage, (int) sizeof (garbage));
    f.proc.setStateInformation (nullptr, 0);
    REQUIRE (f.proc.apvts.getRawParameterValue (pitchdelay::ids::mode)->load() == 1.0f);
}

TEST_CASE ("mode and delay parameters are not host-automatable, bypass is the host bypass")
{
    Fixture f;
    REQUIRE_FALSE (f.proc.apvts.getParameter (pitchdelay::ids::mode)->isAutomatable());
    REQUIRE_FALSE (f.proc.apvts.getParameter (pitchdelay::ids::customUnit)->isAutomatable());
    REQUIRE_FALSE (f.proc.apvts.getParameter (pitchdelay::ids::customMs)->isAutomatable());
    REQUIRE_FALSE (f.proc.apvts.getParameter (pitchdelay::ids::customSamples)->isAutomatable());
    REQUIRE (f.proc.getBypassParameter() == f.proc.apvts.getParameter (pitchdelay::ids::bypass));
}
```

- [ ] **Step 4: Run to verify failure**

```bash
cmake -S . -B build && cmake --build build --target pitchdelay_processor_tests 2>&1 | tail -8
```
Expected: FAIL — `apvts`, `getCurrentDelayFrames` not members of `PitchDelayProcessor`.

- [ ] **Step 5: Rewrite `plugin/PluginProcessor.h`**

```cpp
#pragma once

#include <atomic>
#include <juce_audio_processors/juce_audio_processors.h>

#include "DelayLine.h"

class PitchDelayProcessor final : public juce::AudioProcessor
{
public:
    PitchDelayProcessor();

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    // NOTE: reset() is deliberately NOT overridden: transport start/stop must not clear the buffer.
    bool isBusesLayoutSupported (const BusesLayout&) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

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

    // Delay currently in effect, in sample frames (for the editor readout and the tail length).
    int getCurrentDelayFrames() const noexcept { return currentDelayFrames.load(); }

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void updateDelay();

    pitchdelay::DelayLine delayLine;
    double sampleRate_ = 44100.0;
    std::atomic<int> currentDelayFrames { 0 };

    juce::AudioParameterChoice* modeParam = nullptr;
    juce::AudioParameterChoice* unitParam = nullptr;
    juce::AudioParameterFloat* customMsParam = nullptr;
    juce::AudioParameterInt* customSamplesParam = nullptr;
    juce::AudioParameterBool* bypassParam = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PitchDelayProcessor)
};
```

- [ ] **Step 6: Rewrite `plugin/PluginProcessor.cpp`**

```cpp
#include "PluginProcessor.h"

#include "DelaySettings.h"
#include "ParameterIds.h"

using namespace pitchdelay;

juce::AudioProcessorValueTreeState::ParameterLayout PitchDelayProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<AudioParameterChoice> (
        ParameterID { ids::mode, 1 }, "Mode",
        StringArray { "33 1/3 RPM", "45 RPM", "Custom" }, 0,
        AudioParameterChoiceAttributes().withAutomatable (false)));

    layout.add (std::make_unique<AudioParameterChoice> (
        ParameterID { ids::customUnit, 1 }, "Custom unit",
        StringArray { "Milliseconds", "Samples" }, 0,
        AudioParameterChoiceAttributes().withAutomatable (false)));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { ids::customMs, 1 }, "Custom (ms)",
        NormalisableRange<float> (0.0f, (float) kMaxCustomMs, 0.001f), 900.0f,
        AudioParameterFloatAttributes().withLabel ("ms").withAutomatable (false)));

    layout.add (std::make_unique<AudioParameterInt> (
        ParameterID { ids::customSamples, 1 }, "Custom (samples)",
        0, kMaxCustomSamples, 39690,
        AudioParameterIntAttributes().withLabel ("samples").withAutomatable (false)));

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
    unitParam = dynamic_cast<juce::AudioParameterChoice*> (apvts.getParameter (ids::customUnit));
    customMsParam = dynamic_cast<juce::AudioParameterFloat*> (apvts.getParameter (ids::customMs));
    customSamplesParam = dynamic_cast<juce::AudioParameterInt*> (apvts.getParameter (ids::customSamples));
    bypassParam = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter (ids::bypass));
    jassert (modeParam && unitParam && customMsParam && customSamplesParam && bypassParam);
}

bool PitchDelayProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return out == layouts.getMainInputChannelSet();
}

void PitchDelayProcessor::prepareToPlay (double sampleRate, int)
{
    sampleRate_ = sampleRate;
    const int channels = std::max ({ 1, getTotalNumInputChannels(), getTotalNumOutputChannels() });
    delayLine.prepare (channels, maxDelayFrames (sampleRate));   // allocates and clears
    updateDelay();
}

void PitchDelayProcessor::updateDelay()
{
    const int frames = computeDelayFrames (static_cast<Mode> (modeParam->getIndex()),
                                           static_cast<CustomUnit> (unitParam->getIndex()),
                                           (double) customMsParam->get(),
                                           customSamplesParam->get(),
                                           sampleRate_);
    delayLine.setDelayFrames (frames);                           // hard jump when it changes
    currentDelayFrames.store (delayLine.getDelayFrames());
}

double PitchDelayProcessor::getTailLengthSeconds() const
{
    return sampleRate_ > 0.0 ? (double) getCurrentDelayFrames() / sampleRate_ : 0.0;
}

void PitchDelayProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    // Deliberately NO juce::ScopedNoDenormals: the output must stay bit-identical to the input.
    if (bypassParam->get())
    {
        processBlockBypassed (buffer, midi);
        return;
    }
    updateDelay();
    delayLine.process (buffer.getArrayOfWritePointers(), buffer.getNumChannels(), buffer.getNumSamples());
}

void PitchDelayProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    // Output stays the input (undelayed); keep recording so un-bypass is seamless.
    updateDelay();
    delayLine.record (buffer.getArrayOfReadPointers(), buffer.getNumChannels(), buffer.getNumSamples());
}

void PitchDelayProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    const auto state = apvts.copyState();
    if (const auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void PitchDelayProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes <= 0)
        return;
    if (const auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}
```

- [ ] **Step 7: Run to verify pass**

```bash
cmake --build build && ctest --test-dir build --output-on-failure
```
Expected: both `core` and `processor` pass. Likely snags: a JUCE 9 method name differing from this plan (check the header in `build/_deps/juce-src/modules/`); the bypass test relies on `processBlock` seeing the bypass parameter — it does by design.

- [ ] **Step 8: Re-validate the plugins and add the tail-length note to the spec**

```bash
scripts/validate.sh build
```
Expected: `ALL VALIDATION PASSED`.

Append under `## Parameters` → after the "Maximum delay" bullet in `docs/superpowers/specs/2026-10-03-pitchdelay-design.md`:
```markdown
- The plugin reports its current delay as the **tail length**, so offline bounces keep the
  delayed end of the audio. (Latency stays 0.)
```

- [ ] **Step 9: Commit**

```bash
git add -A
git commit -m "feat: processor with parameters, bypass, state and tail length" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
git push
```

---

### Task 5: Minimal editor

**Files:**
- Create: `plugin/PluginEditor.h`, `plugin/PluginEditor.cpp`
- Modify: `plugin/PluginProcessor.h`, `CMakeLists.txt`, `tests/ProcessorTests.cpp`

**Interfaces:**
- Consumes: `PitchDelayProcessor::apvts`, `getCurrentDelayFrames()`, `getSampleRate()`, `pitchdelay::ids::*`.
- Produces: `PitchDelayEditor`; `PitchDelayProcessor::createEditor()` defined in `PluginEditor.cpp`.

- [ ] **Step 1: Write the editor**

`plugin/PluginEditor.h`:
```cpp
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginProcessor.h"

class PitchDelayEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit PitchDelayEditor (PitchDelayProcessor&);
    ~PitchDelayEditor() override = default;

    void resized() override;

private:
    void timerCallback() override;

    PitchDelayProcessor& proc;

    juce::Label modeLabel { {}, "Mode" }, unitLabel { {}, "Custom unit" },
                msLabel { {}, "Custom (ms)" }, samplesLabel { {}, "Custom (samples)" },
                readout, latency;
    juce::ComboBox modeBox, unitBox;
    juce::Slider msSlider, samplesSlider;

    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> modeAtt, unitAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> msAtt, samplesAtt;
};
```

`plugin/PluginEditor.cpp`:
```cpp
#include "PluginEditor.h"

#include "ParameterIds.h"

PitchDelayEditor::PitchDelayEditor (PitchDelayProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    modeBox.addItemList ({ "33 1/3 RPM", "45 RPM", "Custom" }, 1);
    unitBox.addItemList ({ "Milliseconds", "Samples" }, 1);
    for (auto* s : { &msSlider, &samplesSlider })
    {
        s->setSliderStyle (juce::Slider::LinearHorizontal);
        s->setTextBoxStyle (juce::Slider::TextBoxRight, false, 100, 22);
    }

    modeAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
        proc.apvts, pitchdelay::ids::mode, modeBox);
    unitAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
        proc.apvts, pitchdelay::ids::customUnit, unitBox);
    msAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        proc.apvts, pitchdelay::ids::customMs, msSlider);
    samplesAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        proc.apvts, pitchdelay::ids::customSamples, samplesSlider);

    latency.setText ("Reported latency: 0", juce::dontSendNotification);
    readout.setFont (juce::FontOptions (16.0f, juce::Font::bold));

    for (juce::Component* c : std::initializer_list<juce::Component*> {
             &modeLabel, &modeBox, &unitLabel, &unitBox, &msLabel, &msSlider,
             &samplesLabel, &samplesSlider, &readout, &latency })
        addAndMakeVisible (c);

    setSize (460, 250);
    startTimerHz (15);
    timerCallback();
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
    row (modeLabel, modeBox);
    row (unitLabel, unitBox);
    row (msLabel, msSlider);
    row (samplesLabel, samplesSlider);
    area.removeFromTop (8);
    readout.setBounds (area.removeFromTop (28));
    latency.setBounds (area.removeFromTop (24));
}

void PitchDelayEditor::timerCallback()
{
    const bool custom = modeBox.getSelectedItemIndex() == 2;
    const bool ms = unitBox.getSelectedItemIndex() == 0;
    unitBox.setEnabled (custom);
    msSlider.setEnabled (custom && ms);
    samplesSlider.setEnabled (custom && ! ms);

    const int frames = proc.getCurrentDelayFrames();
    const double sr = proc.getSampleRate();
    readout.setText ("Delay: " + juce::String (frames) + " frames = "
                         + juce::String (sr > 0.0 ? frames * 1000.0 / sr : 0.0, 3) + " ms @ "
                         + juce::String (sr / 1000.0, 1) + " kHz",
                     juce::dontSendNotification);
}

juce::AudioProcessorEditor* PitchDelayProcessor::createEditor()
{
    return new PitchDelayEditor (*this);
}
```

- [ ] **Step 2: Move `createEditor` out of the header and add the source**

In `plugin/PluginProcessor.h` replace the two editor lines with:
```cpp
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
```
In `CMakeLists.txt` add `plugin/PluginEditor.cpp` to the `target_sources(PitchDelay PRIVATE ...)` list.

In `tests/ProcessorTests.cpp` re-add (just above the first `TEST_CASE`):
```cpp
// The editor lives only in the plugin target; the headless tests never create one.
juce::AudioProcessorEditor* PitchDelayProcessor::createEditor() { return nullptr; }
```

- [ ] **Step 3: Build, test, validate**

```bash
cmake --build build && ctest --test-dir build --output-on-failure && scripts/validate.sh build
```
Expected: everything passes (pluginval at strictness 10 opens the editor). A compile error from `juce::FontOptions`/`addItemList` means the JUCE 9 name differs: read the header under `build/_deps/juce-src/modules/` and adjust.

- [ ] **Step 4: Look at it**

```bash
open build/PitchDelay_artefacts/Release/Standalone/PitchDelay.app
```
Expected: window with the four controls and a readout reading `Delay: 43200 frames = 900.000 ms @ 48.0 kHz` (rate depends on the audio device); switching Mode to 45 RPM updates it; Custom enables the unit selector and the matching slider/text box. Report what you actually see.

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "feat: minimal editor with delay readout" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
git push
```

---

### Task 6: Offset-measurement tools, Logic procedure, CI

**Files:**
- Create: `tools/make_impulse.py`, `tools/measure_offset.py`, `docs/manual-logic-test.md`, `.github/workflows/ci.yml`

**Interfaces:**
- Produces: `python3 tools/make_impulse.py out.wav` → 5 s, 48 kHz, mono, 32-bit-float WAV with a single full-scale sample at frame 48000. `python3 tools/measure_offset.py reference.wav delayed.wav [--expect N]` → prints both peak frames and the offset; exit code 0 iff offset == N (when `--expect` is given).

- [ ] **Step 1: Write `tools/make_impulse.py`**

```python
#!/usr/bin/env python3
"""Write a 5 s, 48 kHz mono 32-bit-float WAV with a single impulse at frame 48000."""
import struct
import sys

RATE, SECONDS, IMPULSE_AT = 48000, 5, 48000


def main(path):
    frames = RATE * SECONDS
    data = bytearray(4 * frames)
    struct.pack_into("<f", data, 4 * IMPULSE_AT, 1.0)
    fmt = struct.pack("<HHIIHH", 3, 1, RATE, RATE * 4, 4, 32)   # 3 = IEEE float
    body = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt \
        + b"data" + struct.pack("<I", len(data)) + bytes(data)
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", len(body)) + body)
    print(f"wrote {path}: impulse at frame {IMPULSE_AT}")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: make_impulse.py out.wav")
    main(sys.argv[1])
```

- [ ] **Step 2: Write `tools/measure_offset.py`**

```python
#!/usr/bin/env python3
"""Find the impulse (largest |sample|, channel 0) in two WAVs and print the frame offset."""
import argparse
import struct
import sys


def read_channel0(path):
    raw = open(path, "rb").read()
    if raw[:4] != b"RIFF" or raw[8:12] != b"WAVE":
        sys.exit(f"{path}: not a RIFF/WAVE file")
    pos, fmt, pcm = 12, None, None
    while pos + 8 <= len(raw):
        cid, size = raw[pos:pos + 4], struct.unpack("<I", raw[pos + 4:pos + 8])[0]
        body = raw[pos + 8:pos + 8 + size]
        if cid == b"fmt ":
            tag, ch, rate, _, align, bits = struct.unpack("<HHIIHH", body[:16])
            if tag == 0xFFFE:                       # WAVE_FORMAT_EXTENSIBLE
                tag = struct.unpack("<H", body[24:26])[0]
            fmt = (tag, ch, rate, align, bits)
        elif cid == b"data":
            pcm = body
        pos += 8 + size + (size & 1)
    if fmt is None or pcm is None:
        sys.exit(f"{path}: missing fmt or data chunk")
    tag, ch, rate, align, bits = fmt
    n = len(pcm) // align
    out = []
    for i in range(n):
        o = i * align
        if tag == 3 and bits == 32:
            out.append(struct.unpack_from("<f", pcm, o)[0])
        elif tag == 1 and bits == 16:
            out.append(struct.unpack_from("<h", pcm, o)[0] / 32768.0)
        elif tag == 1 and bits == 24:
            out.append(int.from_bytes(pcm[o:o + 3], "little", signed=True) / 8388608.0)
        elif tag == 1 and bits == 32:
            out.append(struct.unpack_from("<i", pcm, o)[0] / 2147483648.0)
        else:
            sys.exit(f"{path}: unsupported format tag={tag} bits={bits}")
    return out, rate


def peak(samples):
    best, idx = 0.0, -1
    for i, v in enumerate(samples):
        if abs(v) > best:
            best, idx = abs(v), i
    return idx, best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("reference")
    ap.add_argument("delayed")
    ap.add_argument("--expect", type=int, help="expected offset in frames")
    a = ap.parse_args()
    ref, r1 = read_channel0(a.reference)
    dly, r2 = read_channel0(a.delayed)
    (i1, p1), (i2, p2) = peak(ref), peak(dly)
    print(f"reference: peak {p1:.6f} at frame {i1} ({r1} Hz)")
    print(f"delayed:   peak {p2:.6f} at frame {i2} ({r2} Hz)")
    offset = i2 - i1
    print(f"offset: {offset} frames")
    if a.expect is not None:
        ok = offset == a.expect
        print("PASS" if ok else f"FAIL (expected {a.expect})")
        sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
```

- [ ] **Step 3: Test the tools with no DAW (identity and a synthetic delay)**

```bash
mkdir -p /tmp/pdtools && cd /tmp/pdtools
python3 /Users/ameise/src/pitchdelay/tools/make_impulse.py impulse.wav
python3 - <<'EOF'
# build a copy delayed by 43200 frames to exercise the measuring tool
import struct
raw = bytearray(open("impulse.wav","rb").read())
data_off = raw.index(b"data") + 8
shift = 43200 * 4
data = raw[data_off:]
new = bytearray(len(data)); new[shift:] = data[:len(data)-shift]
raw[data_off:] = new
open("delayed.wav","wb").write(raw)
EOF
python3 /Users/ameise/src/pitchdelay/tools/measure_offset.py impulse.wav delayed.wav --expect 43200; echo "exit=$?"
python3 /Users/ameise/src/pitchdelay/tools/measure_offset.py impulse.wav delayed.wav --expect 43201; echo "exit=$?"
```
Expected: first run prints `offset: 43200 frames` / `PASS` / `exit=0`; second prints `FAIL (expected 43201)` / `exit=1`. (The scratch files are in `/tmp/pdtools`, outside the repo.)

- [ ] **Step 4: Write `docs/manual-logic-test.md`**

```markdown
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
```

- [ ] **Step 5: Write `.github/workflows/ci.yml`**

```yaml
name: ci
on:
  push:
  pull_request:

jobs:
  macos:
    runs-on: macos-latest
    steps:
      - uses: actions/checkout@v4
      - name: Install tools
        run: brew install ninja cmake
      - name: Configure
        run: cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPITCHDELAY_INSTALL_AFTER_BUILD=OFF
      - name: Build
        run: cmake --build build
      - name: Unit tests
        run: ctest --test-dir build --output-on-failure
      - name: Install pluginval
        run: |
          curl -fsSL -o pluginval.zip https://github.com/Tracktion/pluginval/releases/latest/download/pluginval_macOS.zip
          unzip -q pluginval.zip
      - name: Validate plugins
        env:
          PLUGINVAL: ${{ github.workspace }}/pluginval.app/Contents/MacOS/pluginval
          CI: "true"
        run: scripts/validate.sh build
```
The first CI run may need tuning (pluginval download name, headless `auval`). If a step fails, investigate and fix the cause; do not delete the step.

- [ ] **Step 6: Commit and check CI**

```bash
git add -A
git commit -m "feat: offset measurement tools, Logic procedure and CI" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
git push
gh run watch --exit-status || gh run view --log-failed | tail -40
```
Expected: the `ci` run goes green, or the failing step's log tells you what to fix.

- [ ] **Step 7: Hand over the manual Logic test**

Do **not** run `docs/manual-logic-test.md` yourself. Tell the user the plugins are installed in `~/Library/Audio/Plug-Ins/` and ask them to run it and report the offsets.

---

## Self-Review

**Spec coverage:** purpose/DelayLine transparency, exact RPM frames, max 10 s, clamp, no allocation after prepare, mono/stereo, clear-on-prepare only → Tasks 2, 3, 4 (no `reset()` override). Parameters + non-automatable + sample-rate behaviour → Task 4 (layout, tests). Bypass semantics → Task 4. Editor readout → Task 5. Build/AU codes/post-build copy/universal option → Task 1. Tests 1 (core), 2 (processor), 3 (pluginval/auval), 4 (manual Logic) → Tasks 2–4, 1/4/5, 6. CI → Task 6. Licence/attribution → Task 1. Out-of-scope items are not planned. One addition beyond the spec: tail length = delay (Review Focus 1), recorded in the spec in Task 4 Step 8.

**Placeholders:** none; every code step has code.

**Type consistency:** `DelayLine::{prepare,reset,setDelayFrames,getDelayFrames,process,record}` and `computeDelayFrames(Mode, CustomUnit, double, int, double)` are used identically in Tasks 2–5; parameter ids come only from `pitchdelay::ids`; `PitchDelayProcessor::{apvts,getCurrentDelayFrames,createEditor}` match across Tasks 4–5.
