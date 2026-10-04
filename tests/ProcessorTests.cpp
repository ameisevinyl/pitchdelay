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

    // Sets a parameter to a plain (denormalised) value. APVTS listeners run synchronously.
    void set (const char* id, float plainValue)
    {
        auto* p = proc.apvts.getParameter (id);
        p->setValueNotifyingHost (p->convertTo0to1 (plainValue));
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
    REQUIRE (f.get (ids::calibrationLevel) == Catch::Approx (-20.0f).margin (1e-4));   // raw value is denormalised from 0.6667
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
