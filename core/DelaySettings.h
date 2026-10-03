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
