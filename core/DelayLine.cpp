#include "DelayLine.h"

#include <algorithm>

namespace pitchdelay
{
void DelayLine::prepare (int numChannels, int maxDelayFrames)
{
    numChannels_ = std::max (0, numChannels);
    maxDelay_ = std::max (0, maxDelayFrames);
    size_ = maxDelay_ + 1;
    buffer_.assign (static_cast<size_t> (numChannels_) * static_cast<size_t> (size_), 0.0f);
    delay_ = 0;
    writePos_ = 0;
    dirty_ = false;
}

void DelayLine::reset() noexcept
{
    if (dirty_)   // a stopped transport resets every block; do not memset 10 s of audio each time
    {
        std::fill (buffer_.begin(), buffer_.end(), 0.0f);
        dirty_ = false;
    }
    writePos_ = 0;
}

void DelayLine::setDelayFrames (int frames) noexcept
{
    delay_ = std::clamp (frames, 0, maxDelay_);
}

void DelayLine::process (float* const* channels, int numChannels, int numFrames) noexcept
{
    const int n = std::min (numChannels, numChannels_);
    if (n > 0 && numFrames > 0)
        dirty_ = true;
    for (int c = 0; c < n; ++c)
    {
        float* ring = buffer_.data() + static_cast<size_t> (c) * static_cast<size_t> (size_);
        float* io = channels[c];
        int w = writePos_;
        int r = w - delay_;
        if (r < 0) r += size_;
        for (int i = 0; i < numFrames; ++i)
        {
            ring[w] = io[i];     // write first, so delay 0 reads the sample just written
            io[i] = ring[r];
            if (++w == size_) w = 0;
            if (++r == size_) r = 0;
        }
    }
    writePos_ = static_cast<int> ((static_cast<long long> (writePos_) + numFrames) % size_);
}

void DelayLine::record (const float* const* channels, int numChannels, int numFrames) noexcept
{
    const int n = std::min (numChannels, numChannels_);
    if (n > 0 && numFrames > 0)
        dirty_ = true;
    for (int c = 0; c < n; ++c)
    {
        float* ring = buffer_.data() + static_cast<size_t> (c) * static_cast<size_t> (size_);
        const float* in = channels[c];
        int w = writePos_;
        for (int i = 0; i < numFrames; ++i)
        {
            ring[w] = in[i];
            if (++w == size_) w = 0;
        }
    }
    writePos_ = static_cast<int> ((static_cast<long long> (writePos_) + numFrames) % size_);
}
}
