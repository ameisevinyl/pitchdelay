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
    generate (g, 100);                       // phase is now 100, inside the burst

    g.setPeriodFrames (1000);                // same value: keeps running
    const auto cont = generate (g, 5);
    for (int k = 0; k < 5; ++k)
        REQUIRE (cont[(size_t) k] == Catch::Approx (gainFor (-20.0f) * referenceBurst (100 + k, sr)).margin (1e-7));

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
