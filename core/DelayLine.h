// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 ameisevinyl

#pragma once
#include <vector>

namespace pitchdelay
{
// Exact-frame, bit-transparent multichannel delay line.
// Only copies samples: no gain, filtering, interpolation or feedback.
// All delays are in whole sample frames. Allocation happens only in prepare().
class DelayLine
{
public:
    void prepare (int numChannels, int maxDelayFrames);
    void reset() noexcept;

    void setDelayFrames (int frames) noexcept;
    int getDelayFrames() const noexcept { return delay_; }
    int getMaxDelayFrames() const noexcept { return maxDelay_; }
    int getNumChannels() const noexcept { return numChannels_; }
    // True when the buffer may hold audio; reset() skips the memory clear when it is already clean.
    bool isDirty() const noexcept { return dirty_; }

    // In place: every channel becomes itself delayed by getDelayFrames().
    void process (float* const* channels, int numChannels, int numFrames) noexcept;
    // Writes the input into the buffer and leaves the channels untouched.
    void record (const float* const* channels, int numChannels, int numFrames) noexcept;

private:
    std::vector<float> buffer_;   // numChannels_ rings of size_ frames
    int numChannels_ = 0;
    int maxDelay_ = 0;
    int size_ = 1;                // maxDelay_ + 1: write-then-read makes delay 0 valid
    int delay_ = 0;
    int writePos_ = 0;
    bool dirty_ = false;
};
}
