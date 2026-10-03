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
