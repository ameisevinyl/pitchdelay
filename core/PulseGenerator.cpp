#include "PulseGenerator.h"

#include <algorithm>
#include <cmath>

namespace pitchdelay
{
void PulseGenerator::prepare (double sampleRate)
{
    const int length = std::max (1, static_cast<int> (std::llround (kBurstSeconds * sampleRate)));
    burst_.assign (static_cast<size_t> (length), 0.0f);
    const double twoPi = 6.283185307179586;
    for (int i = 0; i < length; ++i)
    {
        const double window = 0.5 * (1.0 - std::cos (twoPi * i / length));
        burst_[static_cast<size_t> (i)] = static_cast<float> (window * std::sin (twoPi * kToneHz * i / sampleRate));
    }
    phase_ = 0;
}

void PulseGenerator::setPeriodFrames (int frames) noexcept
{
    frames = std::max (0, frames);
    if (frames != period_)
    {
        period_ = frames;
        phase_ = 0;
    }
}

void PulseGenerator::setLevelDb (float db) noexcept
{
    gain_ = static_cast<float> (std::pow (10.0, static_cast<double> (db) / 20.0));
}

void PulseGenerator::generate (float* out, int numFrames) noexcept
{
    if (period_ <= 0 || burst_.empty())
    {
        std::fill (out, out + numFrames, 0.0f);
        return;
    }
    const int length = static_cast<int> (burst_.size());
    int phase = phase_;
    for (int i = 0; i < numFrames; ++i)
    {
        out[i] = phase < length ? gain_ * burst_[static_cast<size_t> (phase)] : 0.0f;
        if (++phase >= period_)
            phase = 0;
    }
    phase_ = phase;
}
}
