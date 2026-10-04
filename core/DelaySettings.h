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
