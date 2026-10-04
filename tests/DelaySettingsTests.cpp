#include <catch2/catch_approx.hpp>
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
