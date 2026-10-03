#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

#include "DelayLine.h"

using pitchdelay::DelayLine;

namespace
{
uint32_t bitsOf (float f) { uint32_t u; std::memcpy (&u, &f, 4); return u; }
float fromBits (uint32_t u) { float f; std::memcpy (&f, &u, 4); return f; }

// Runs mono `input` through `d` in blocks of `blockSize`; returns the output.
std::vector<float> run (DelayLine& d, const std::vector<float>& input, int blockSize)
{
    std::vector<float> out = input;
    for (size_t pos = 0; pos < out.size(); pos += (size_t) blockSize)
    {
        const int n = (int) std::min<size_t> ((size_t) blockSize, out.size() - pos);
        float* ch[1] = { out.data() + pos };
        d.process (ch, 1, n);
    }
    return out;
}
}

TEST_CASE ("an impulse comes out exactly N frames later and nowhere else")
{
    const int N = GENERATE (0, 1, 5, 100);
    const int block = GENERATE (1, 7, 64, 512);

    DelayLine d;
    d.prepare (1, 1000);
    d.setDelayFrames (N);

    std::vector<float> in ((size_t) N + 300, 0.0f);
    in[3] = 1.0f;
    const auto out = run (d, in, block);

    for (size_t i = 0; i < out.size(); ++i)
        REQUIRE (out[i] == (i == (size_t) (3 + N) ? 1.0f : 0.0f));
}

TEST_CASE ("output is bit-identical to input, including NaN payloads, inf, denormals, -0")
{
    const int N = 37;
    DelayLine d;
    d.prepare (1, 100);
    d.setDelayFrames (N);

    std::vector<float> in;
    for (uint32_t special : { 0x7fc12345u, 0x7f800001u, 0x7f800000u, 0xff800000u,
                              0x00000001u, 0x007fffffu, 0x80000000u, 0x80000001u })
        in.push_back (fromBits (special));
    std::mt19937 rng (1234);
    for (int i = 0; i < 5000; ++i)
        in.push_back (fromBits ((uint32_t) rng()));

    const auto out = run (d, in, 64);

    for (size_t i = 0; i < in.size(); ++i)
    {
        const uint32_t expected = i < (size_t) N ? 0u : bitsOf (in[i - (size_t) N]);
        REQUIRE (bitsOf (out[i]) == expected);
    }
}

TEST_CASE ("block much larger than the ring wraps correctly")
{
    DelayLine d;
    d.prepare (1, 10);             // ring of 11 frames
    d.setDelayFrames (3);

    std::vector<float> in (500);
    for (size_t i = 0; i < in.size(); ++i) in[i] = (float) (i + 1);
    const auto out = run (d, in, 500);   // one block of 500 frames

    for (size_t i = 0; i < in.size(); ++i)
        REQUIRE (out[i] == (i < 3 ? 0.0f : in[i - 3]));
}

TEST_CASE ("maximum delay works and larger requests clamp")
{
    DelayLine d;
    d.prepare (1, 1000);
    d.setDelayFrames (5000);
    REQUIRE (d.getDelayFrames() == 1000);
    d.setDelayFrames (-4);
    REQUIRE (d.getDelayFrames() == 0);
    d.setDelayFrames (1000);

    std::vector<float> in (1200, 0.0f);
    in[0] = 1.0f;
    const auto out = run (d, in, 128);
    for (size_t i = 0; i < out.size(); ++i)
        REQUIRE (out[i] == (i == 1000 ? 1.0f : 0.0f));
}

TEST_CASE ("channels are independent and share one delay")
{
    DelayLine d;
    d.prepare (2, 100);
    d.setDelayFrames (10);

    std::vector<float> l (50), r (50);
    for (int i = 0; i < 50; ++i) { l[(size_t) i] = (float) (i + 1); r[(size_t) i] = -(float) (i + 1); }
    float* ch[2] = { l.data(), r.data() };
    d.process (ch, 2, 50);

    for (int i = 0; i < 50; ++i)
    {
        REQUIRE (l[(size_t) i] == (i < 10 ? 0.0f : (float) (i - 10 + 1)));
        REQUIRE (r[(size_t) i] == (i < 10 ? 0.0f : -(float) (i - 10 + 1)));
    }
}

TEST_CASE ("changing the delay is a hard jump of the read position")
{
    DelayLine d;
    d.prepare (1, 100);
    d.setDelayFrames (10);

    std::vector<float> a (20);
    for (size_t i = 0; i < 20; ++i) a[i] = (float) (i + 1);     // frames 0..19
    float* pa[1] = { a.data() };
    d.process (pa, 1, 20);

    d.setDelayFrames (4);
    std::vector<float> b (5);
    for (size_t i = 0; i < 5; ++i) b[i] = (float) (21 + i);     // frames 20..24
    float* pb[1] = { b.data() };
    d.process (pb, 1, 5);

    // output at frame 20+k is the input from frame 20+k-4
    for (size_t k = 0; k < 5; ++k)
        REQUIRE (b[k] == (float) (20 + k - 4 + 1));
}

TEST_CASE ("record() keeps the input and fills the buffer for a later process()")
{
    DelayLine d;
    d.prepare (1, 100);
    d.setDelayFrames (10);

    std::vector<float> a (20);
    for (size_t i = 0; i < 20; ++i) a[i] = (float) (i + 1);
    const std::vector<float> copy = a;
    const float* ra[1] = { a.data() };
    d.record (ra, 1, 20);
    REQUIRE (a == copy);

    std::vector<float> zeros (10, 0.0f);
    float* pz[1] = { zeros.data() };
    d.process (pz, 1, 10);
    for (size_t k = 0; k < 10; ++k)
        REQUIRE (zeros[k] == copy[10 + k]);   // frames 10..19 recorded earlier
}

TEST_CASE ("reset() clears audio but keeps the delay")
{
    DelayLine d;
    d.prepare (1, 100);
    d.setDelayFrames (10);
    std::vector<float> a (30, 1.0f);
    run (d, a, 30);
    d.reset();
    REQUIRE (d.getDelayFrames() == 10);
    const auto out = run (d, std::vector<float> (10, 0.0f), 10);
    for (float v : out) REQUIRE (v == 0.0f);
}

TEST_CASE ("channel count mismatches are safe")
{
    DelayLine d;
    d.prepare (2, 100);
    d.setDelayFrames (5);

    // more channels than prepared: the extra channel is left untouched
    std::vector<float> a (10, 1.0f), b (10, 2.0f), c (10, 3.0f);
    float* three[3] = { a.data(), b.data(), c.data() };
    d.process (three, 3, 10);
    for (float v : c) REQUIRE (v == 3.0f);

    // fewer channels than prepared: processes without touching memory it does not own
    std::vector<float> m (10, 1.0f);
    float* one[1] = { m.data() };
    d.process (one, 1, 10);
}
