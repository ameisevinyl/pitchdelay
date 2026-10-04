#pragma once
#include <vector>

namespace pitchdelay
{
// Periodic tone-burst generator for calibration: one Hann-windowed 1 kHz burst of 5 ms
// every `period` frames. The burst is precomputed in prepare().
class PulseGenerator
{
public:
    static constexpr double kBurstSeconds = 0.005;
    static constexpr double kToneHz = 1000.0;

    void prepare (double sampleRate);
    void reset() noexcept { phase_ = 0; }
    void setPeriodFrames (int frames) noexcept;
    void setLevelDb (float db) noexcept;
    int getBurstFrames() const noexcept { return static_cast<int> (burst_.size()); }

    // Writes numFrames samples of one channel and advances the phase.
    void generate (float* out, int numFrames) noexcept;

private:
    std::vector<float> burst_;
    int period_ = 0;
    int phase_ = 0;
    float gain_ = 0.1f;   // -20 dB
};
}
