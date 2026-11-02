#pragma once

// Rational resampling of a CU8 stream: up / down = out_rate / in_rate
// reduced by their gcd, a polyphase windowed-sinc filter (32 taps per phase,
// Blackman window, cutoff min(0.5 * in_rate, 0.45 * out_rate): the input's
// Nyquist frequency when upsampling, an anti-alias margin when downsampling;
// unity gain at DC)
// and the output rounded and clipped as IqWriter does without noise.
// Output sample m is the input interpolated at m * down / up input samples;
// samples before the start and after the end of the input count as zero.

#include <cstdint>
#include <iosfwd>

namespace iqgen {

constexpr uint64_t kResampleMaxFactor = 1000;
constexpr unsigned kResampleTapsPerPhase = 32;

struct ResampleCounts {
    uint64_t in_samples, out_samples;  // out_samples = floor(in_samples * up / down)
    uint64_t clipped;                  // output I and Q values clipped to 0..255, counted separately
};

// Throws std::invalid_argument for a zero rate or when up or down exceeds
// kResampleMaxFactor, before reading anything; std::runtime_error when out fails.
ResampleCounts resampleCu8(std::istream& in, uint64_t in_rate, uint64_t out_rate, std::ostream& out);

}  // namespace iqgen
