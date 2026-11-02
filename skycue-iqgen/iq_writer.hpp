#pragma once

// CU8 output (interleaved unsigned 8-bit I and Q, 128 = zero) and its
// statistics. A sample v becomes the byte pair round(128 + Re v),
// round(128 + Im v), each clipped to 0..255.

#include <complex>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <random>
#include <vector>

namespace iqgen {

// The CU8 amplitude of a level in dB full scale: 128 * 10^(dbfs / 20).
double amplitudeForLevel(double dbfs);

// round(128 + v) clipped to 0..255; a clipped value increments clipped.
uint8_t quantiseCu8(double v, uint64_t& clipped);

class IqWriter {
public:
    // Each written sample is rotated by cfo_hz (the phase starting at 0 for
    // the first sample), then, unless snr_db is nullopt, gets Gaussian noise
    // of sigma = amplitude * 10^(-snr_db / 20) / sqrt(2) on I and on Q,
    // from std::mt19937_64(seed) through Box-Muller; then it is quantised.
    IqWriter(std::ostream& out, double rate, double amplitude, std::optional<double> snr_db, double cfo_hz,
             uint64_t seed);

    // Throws std::runtime_error when the stream fails.
    void write(const std::complex<float>* x, std::size_t n);

    uint64_t samples() const { return samples_; }
    uint64_t clipped() const { return clipped_; }  // I and Q values counted separately

private:
    double uniform();  // (0, 1]

    std::ostream& out_;
    double sigma_;
    double phase_step_;
    double phase_ = 0.0;
    std::mt19937_64 rng_;
    uint64_t samples_ = 0;
    uint64_t clipped_ = 0;
    std::vector<char> bytes_;
};

struct Cu8Info {
    uint64_t samples;
    double seconds, mean_mag, peak_mag, dc_i, dc_q;
};

// Over every whole sample of in: the count, its duration at rate, the mean
// and peak of |(I - 128, Q - 128)|, and the means of I - 128 and Q - 128.
// All zero for an empty stream; a trailing odd byte is ignored.
Cu8Info cu8Info(std::istream& in, double rate);

}  // namespace iqgen
