// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 ameisevinyl

#include "DelaySettings.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pitchdelay
{
int maxDelayFrames (double sampleRate)
{
    return static_cast<int> (std::ceil (kMaxDelaySeconds * sampleRate));
}

int framesForMilliseconds (double ms, double sampleRate)
{
    return static_cast<int> (std::llround (ms * sampleRate / 1000.0));
}


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
    if (toRate <= 0.0)
        return frames;
    if (fromRate <= 0.0 || fromRate == toRate)
        return clampFrames (frames, toRate);

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
}
