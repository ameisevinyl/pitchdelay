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
