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

// Minimal host transport for the tests.
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
}

// The editor lives only in the plugin target; the headless tests never create one.
juce::AudioProcessorEditor* PitchDelayProcessor::createEditor() { return nullptr; }

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

TEST_CASE ("delay readout and tail follow parameters and restored state before any audio is processed")
{
    Fixture f (48000.0);
    f.set (pitchdelay::ids::mode, 1);                    // 45 RPM, no processBlock call
    REQUIRE (f.proc.getCurrentDelayFrames() == 32000);
    REQUIRE (f.proc.getTailLengthSeconds() == Catch::Approx (2.0 / 3.0).margin (1e-9));

    Fixture saved (48000.0);
    saved.set (pitchdelay::ids::mode, 2);
    saved.set (pitchdelay::ids::customUnit, 1);
    saved.set (pitchdelay::ids::customSamples, 4800.0f);
    juce::MemoryBlock state;
    saved.proc.getStateInformation (state);

    Fixture loaded (48000.0);                            // host restores a project, then asks for the tail
    loaded.proc.setStateInformation (state.getData(), (int) state.getSize());
    REQUIRE (loaded.proc.getCurrentDelayFrames() == 4800);
    REQUIRE (loaded.proc.getTailLengthSeconds() == Catch::Approx (0.1).margin (1e-9));
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

TEST_CASE ("stopping the transport wipes the buffer so a restart begins with a full delay of zeros")
{
    Fixture f (48000.0);
    FakePlayHead host;
    f.proc.setPlayHead (&host);
    f.set (pitchdelay::ids::mode, 2);
    f.set (pitchdelay::ids::customUnit, 1);
    f.set (pitchdelay::ids::customSamples, 100.0f);

    host.playing = true;
    f.run (ramp (1000));                       // fills the buffer with old audio

    host.playing = false;
    const auto whileStopped = f.run (ramp (300, 5000.0f));   // input keeps arriving while stopped
    for (float v : whileStopped) REQUIRE (v == 0.0f);        // drive output is silent while stopped

    host.playing = true;                       // start again from the top
    const auto restarted = f.run (std::vector<float> (300, 0.0f));
    for (float v : restarted) REQUIRE (v == 0.0f);           // nothing old comes out

    f.proc.setPlayHead (nullptr);
}

TEST_CASE ("after a stop, playback delays by exactly N frames again")
{
    Fixture f (48000.0);
    FakePlayHead host;
    f.proc.setPlayHead (&host);
    f.set (pitchdelay::ids::mode, 2);
    f.set (pitchdelay::ids::customUnit, 1);
    f.set (pitchdelay::ids::customSamples, 100.0f);

    host.playing = false;
    f.run (ramp (200));
    host.playing = true;
    std::vector<float> in (400, 0.0f);
    in[0] = 1.0f;
    REQUIRE (firstNonZero (f.run (in)) == 100);

    f.proc.setPlayHead (nullptr);
}

TEST_CASE ("bypass while the transport is stopped still passes audio through, and wipes the buffer")
{
    Fixture f (48000.0);
    FakePlayHead host;
    f.proc.setPlayHead (&host);
    f.set (pitchdelay::ids::mode, 2);
    f.set (pitchdelay::ids::customUnit, 1);
    f.set (pitchdelay::ids::customSamples, 100.0f);

    host.playing = true;
    f.run (ramp (500));
    f.set (pitchdelay::ids::bypass, 1.0f);
    host.playing = false;
    const auto r = ramp (200, 9000.0f);
    REQUIRE (f.run (r) == r);                  // bypass: undelayed pass-through

    f.set (pitchdelay::ids::bypass, 0.0f);
    host.playing = true;
    for (float v : f.run (std::vector<float> (300, 0.0f))) REQUIRE (v == 0.0f);

    f.proc.setPlayHead (nullptr);
}

TEST_CASE ("reset() clears the buffer")
{
    Fixture f (48000.0);
    f.set (pitchdelay::ids::mode, 2);
    f.set (pitchdelay::ids::customUnit, 1);
    f.set (pitchdelay::ids::customSamples, 100.0f);

    f.run (ramp (1000));
    f.proc.reset();
    for (float v : f.run (std::vector<float> (300, 0.0f))) REQUIRE (v == 0.0f);
}

TEST_CASE ("a host that reports the transport as playing delays normally")
{
    Fixture f (48000.0);
    FakePlayHead host;
    f.proc.setPlayHead (&host);
    f.set (pitchdelay::ids::mode, 2);
    f.set (pitchdelay::ids::customUnit, 1);
    f.set (pitchdelay::ids::customSamples, 100.0f);
    std::vector<float> in (400, 0.0f);
    in[0] = 1.0f;
    REQUIRE (firstNonZero (f.run (in)) == 100);
    f.proc.setPlayHead (nullptr);
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
