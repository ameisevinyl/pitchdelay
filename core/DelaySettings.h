// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 ameisevinyl

#pragma once

namespace pitchdelay
{

inline constexpr double kMaxDelaySeconds = 10.0;

int maxDelayFrames (double sampleRate);
int framesForMilliseconds (double ms, double sampleRate);

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
}
