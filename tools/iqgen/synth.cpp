#include "synth.hpp"

#include "bits.hpp"

#include <cmath>
#include <stdexcept>

namespace iqgen {

namespace {

// Mode S timing in 12 MHz ticks (12 per us).
constexpr int64_t kTicksPerUs = 12;
constexpr int64_t kPulseTicks = 6;                       // 0.5 us
constexpr int64_t kPreambleTicks[] = {0, 12, 42, 54};    // 0.0, 1.0, 3.5, 4.5 us
constexpr int64_t kDataStartTicks = 8 * kTicksPerUs;     // 8.0 us

// UAT: modulation index 0.6 at 2 samples per bit.
constexpr double kPi = 3.14159265358979323846;
constexpr double kUatStep = 0.3 * kPi;
constexpr std::size_t kUatSamplesPerBit = 2;

}  // namespace

std::size_t modesFrameTicks(std::size_t frame_bytes)
{
    return static_cast<std::size_t>(kDataStartTicks) + frame_bytes * 8 * static_cast<std::size_t>(kTicksPerUs);
}

ModesSynth::ModesSynth(double amplitude)
    : amplitude_(static_cast<float>(amplitude)), ring_(kModesRingTicks, 0.0f)
{
}

void ModesSynth::addPulse(int64_t tick)
{
    for (int64_t t = tick; t < tick + kPulseTicks; ++t)
        ring_[static_cast<std::size_t>(t) % kModesRingTicks] += amplitude_;
}

void ModesSynth::addFrame(int64_t start_tick, const std::vector<uint8_t>& frame)
{
    const int64_t emitted = emittedTicks();
    const auto span = static_cast<int64_t>(modesFrameTicks(frame.size()));
    if (start_tick < emitted)
        throw std::invalid_argument("Mode S frame starts before the ticks already emitted");
    if (start_tick + span > emitted + static_cast<int64_t>(kModesRingTicks))
        throw std::invalid_argument("Mode S frame starts too far ahead of the ticks emitted");

    for (int64_t p : kPreambleTicks)
        addPulse(start_tick + p);
    std::vector<uint8_t> bits;
    appendBytes(bits, frame.data(), frame.size());
    for (std::size_t b = 0; b < bits.size(); ++b) {
        const int64_t chip = start_tick + kDataStartTicks + static_cast<int64_t>(b) * kTicksPerUs;
        addPulse(bits[b] ? chip : chip + kPulseTicks);
    }
}

void ModesSynth::emitUntil(int64_t tick, std::vector<std::complex<float>>& out)
{
    while ((next_sample_ + 1) * kModesTicksPerSample <= tick) {
        float sum = 0.0f;
        for (int64_t t = next_sample_ * kModesTicksPerSample; t < (next_sample_ + 1) * kModesTicksPerSample; ++t) {
            float& slot = ring_[static_cast<std::size_t>(t) % kModesRingTicks];
            sum += slot;
            slot = 0.0f;
        }
        out.emplace_back(sum / static_cast<float>(kModesTicksPerSample), 0.0f);
        ++next_sample_;
    }
}

UatSynth::UatSynth(double amplitude) : amplitude_(amplitude), ring_(kUatRingSamples) {}

void UatSynth::addFrame(int64_t start_sample, const std::vector<uint8_t>& bits)
{
    const auto span = static_cast<int64_t>(bits.size() * kUatSamplesPerBit);
    if (start_sample < next_sample_)
        throw std::invalid_argument("UAT frame starts before the samples already emitted");
    if (start_sample + span > next_sample_ + static_cast<int64_t>(kUatRingSamples))
        throw std::invalid_argument("UAT frame starts too far ahead of the samples emitted");

    double phase = 0.0;
    for (int64_t n = 0; n < span; ++n) {
        const auto slot = static_cast<std::size_t>(start_sample + n) % kUatRingSamples;
        ring_[slot] += std::complex<float>(static_cast<float>(amplitude_ * std::cos(phase)),
                                           static_cast<float>(amplitude_ * std::sin(phase)));
        phase += bits[static_cast<std::size_t>(n) / kUatSamplesPerBit] ? kUatStep : -kUatStep;
        if (phase >= kPi)
            phase -= 2.0 * kPi;
        else if (phase < -kPi)
            phase += 2.0 * kPi;
    }
}

void UatSynth::emitUntil(int64_t sample, std::vector<std::complex<float>>& out)
{
    for (; next_sample_ < sample; ++next_sample_) {
        auto& slot = ring_[static_cast<std::size_t>(next_sample_) % kUatRingSamples];
        out.push_back(slot);
        slot = std::complex<float>();
    }
}

}  // namespace iqgen
