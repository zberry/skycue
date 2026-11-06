#pragma once

// Baseband waveform synthesis for the IQ generators, streaming: frames are
// added in start order and the output is taken off the front with
// emitUntil(), so only one frame's worth of samples is ever held.
//
// Mode S (1090 MHz) is pulse-position modulation built at a 12 MS/s
// intermediate rate: preamble pulses 0.5 us long at 0.0, 1.0, 3.5 and
// 4.5 us, data from 8.0 us at 1 bit/us, a 1 having its pulse in the first
// half-us. Pulses are rectangular, amplitude A on I with Q = 0; a box filter
// of 5 ticks (the mean of each 5) decimates to 2 400 000 S/s.
//
// UAT (978 MHz) is phase-continuous CPFSK at 2 083 333 S/s, 2 samples per
// bit, rectangular frequency pulse, deviation +-312.5 kHz (+-0.3 pi rad per
// sample), a 1 bit at +312.5 kHz: x = A exp(j phi).

#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace iqgen {

constexpr double kModesRate = 2400000.0;
constexpr double kModesTickRate = 12e6;
constexpr double kUatRate = 2083333.0;

constexpr int64_t kModesTicksPerSample = 5;
constexpr std::size_t kModesRingTicks = 2048;   // >= 1440 + 5
constexpr std::size_t kUatRingSamples = 16384;  // >= 8904

// Ticks (12 MHz) from the first preamble pulse to the end of the last data
// bit: (8 + 8 * frame_bytes) us; 7 -> 768, 14 -> 1440.
std::size_t modesFrameTicks(std::size_t frame_bytes);

class ModesSynth {
public:
    explicit ModesSynth(double amplitude);

    // Adds the PPM pulses of frame (any whole number of bytes) from
    // start_tick on; overlapping frames add. start_tick must not be before
    // the ticks already emitted, and the frame must end within
    // kModesRingTicks of them (call emitUntil(start_tick) first); throws
    // std::invalid_argument otherwise.
    void addFrame(int64_t start_tick, const std::vector<uint8_t>& frame);

    // Appends output sample m (the mean of ticks [5m, 5m + 5)) for every m
    // not yet emitted with 5m + 5 <= tick.
    void emitUntil(int64_t tick, std::vector<std::complex<float>>& out);

    // Ticks covered by the samples emitted so far (a multiple of 5).
    int64_t emittedTicks() const { return next_sample_ * kModesTicksPerSample; }

private:
    void addPulse(int64_t tick);

    float amplitude_;
    int64_t next_sample_ = 0;
    std::vector<float> ring_;  // tick t at ring_[t % kModesRingTicks]
};

class UatSynth {
public:
    explicit UatSynth(double amplitude);

    // Adds the CPFSK waveform of bits (one bit per element, 0 or 1) from
    // start_sample on: 2 * bits.size() samples, phase 0 at the first sample,
    // advancing +0.3 pi per sample for a 1 bit and -0.3 pi for a 0 bit.
    // Overlapping frames add (complex). start_sample must not be before the
    // samples already emitted, and the frame must end within kUatRingSamples
    // of them (call emitUntil(start_sample) first); throws
    // std::invalid_argument otherwise.
    void addFrame(int64_t start_sample, const std::vector<uint8_t>& bits);

    // Appends every sample not yet emitted before sample.
    void emitUntil(int64_t sample, std::vector<std::complex<float>>& out);

    int64_t emittedSamples() const { return next_sample_; }

private:
    double amplitude_;
    int64_t next_sample_ = 0;
    std::vector<std::complex<float>> ring_;  // sample n at ring_[n % kUatRingSamples]
};

}  // namespace iqgen
