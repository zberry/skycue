#include "resample.hpp"

#include "iq_writer.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <istream>
#include <numeric>
#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace iqgen {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int64_t kTaps = kResampleTapsPerPhase;
constexpr int64_t kBefore = kTaps / 2 - 1;  // taps at n0 - 15 .. n0 + 16
constexpr int64_t kAfter = kTaps / 2;
constexpr double kCutoff = 0.45;
constexpr std::size_t kChunkSamples = 65536;

double sinc(double x)
{
    return x == 0.0 ? 1.0 : std::sin(kPi * x) / (kPi * x);
}

// taps[p * kTaps + k] weighs input n0 - kBefore + k for an output at
// n0 + p / up input samples; each phase sums to 1.
std::vector<double> makeTaps(uint64_t up, double fc)
{
    std::vector<double> taps(static_cast<std::size_t>(up) * kTaps);
    const double half = static_cast<double>(kTaps) / 2.0;
    for (uint64_t p = 0; p < up; ++p) {
        double sum = 0.0;
        for (int64_t k = 0; k < kTaps; ++k) {
            const double tau = static_cast<double>(p) / static_cast<double>(up) + static_cast<double>(kBefore - k);
            const double w = 0.42 + 0.5 * std::cos(kPi * tau / half) + 0.08 * std::cos(2.0 * kPi * tau / half);
            const double h = sinc(2.0 * fc * tau) * w;
            taps[static_cast<std::size_t>(p * kTaps + static_cast<uint64_t>(k))] = h;
            sum += h;
        }
        for (int64_t k = 0; k < kTaps; ++k)
            taps[static_cast<std::size_t>(p * kTaps + static_cast<uint64_t>(k))] /= sum;
    }
    return taps;
}

}  // namespace

ResampleCounts resampleCu8(std::istream& in, uint64_t in_rate, uint64_t out_rate, std::ostream& out)
{
    if (in_rate == 0 || out_rate == 0)
        throw std::invalid_argument("sample rates must be positive");
    const uint64_t g = std::gcd(in_rate, out_rate);
    const uint64_t up = out_rate / g, down = in_rate / g;
    if (up > kResampleMaxFactor || down > kResampleMaxFactor)
        throw std::invalid_argument("rate ratio " + std::to_string(up) + "/" + std::to_string(down) +
                                    " needs a factor above " + std::to_string(kResampleMaxFactor));

    // Cutoff in cycles per input sample.
    const double fc = kCutoff * static_cast<double>(std::min(in_rate, out_rate)) / static_cast<double>(in_rate);
    const std::vector<double> taps = makeTaps(up, fc);

    std::vector<std::complex<float>> hist;  // input samples from index base on
    int64_t base = 0;
    int64_t total_in = 0;
    uint64_t m = 0;
    uint64_t clipped = 0;
    std::vector<char> bytes;
    std::vector<char> raw(2 * kChunkSamples + 1);
    std::size_t carry = 0;

    auto inputAt = [&](int64_t n) {
        return n < 0 || n >= total_in ? std::complex<float>() : hist[static_cast<std::size_t>(n - base)];
    };
    auto emit = [&](uint64_t limit, bool at_end) {
        bytes.clear();
        for (; m < limit; ++m) {
            const uint64_t pos = m * down;
            const auto n0 = static_cast<int64_t>(pos / up);
            if (!at_end && n0 + kAfter >= total_in)
                break;
            const double* h = taps.data() + (pos % up) * kTaps;
            double re = 0.0, im = 0.0;
            for (int64_t k = 0; k < kTaps; ++k) {
                const std::complex<float> x = inputAt(n0 - kBefore + k);
                re += h[k] * static_cast<double>(x.real());
                im += h[k] * static_cast<double>(x.imag());
            }
            bytes.push_back(static_cast<char>(quantiseCu8(re, clipped)));
            bytes.push_back(static_cast<char>(quantiseCu8(im, clipped)));
        }
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out)
            throw std::runtime_error("write failed");
    };

    while (in) {
        in.read(raw.data() + carry, static_cast<std::streamsize>(raw.size() - carry));
        const std::size_t have = carry + static_cast<std::size_t>(in.gcount());
        const std::size_t whole = have / 2;
        for (std::size_t k = 0; k < whole; ++k)
            hist.emplace_back(static_cast<float>(static_cast<uint8_t>(raw[2 * k])) - 128.0f,
                              static_cast<float>(static_cast<uint8_t>(raw[2 * k + 1])) - 128.0f);
        total_in += static_cast<int64_t>(whole);
        carry = have - 2 * whole;
        if (carry)
            raw[0] = raw[have - 1];

        emit(UINT64_MAX, false);

        // Drop the inputs no later output needs.
        const int64_t keep_from = static_cast<int64_t>(m * down / up) - kBefore;
        if (keep_from - base > static_cast<int64_t>(kChunkSamples)) {
            hist.erase(hist.begin(), hist.begin() + (keep_from - base));
            base = keep_from;
        }
    }
    const auto in_samples = static_cast<uint64_t>(total_in);
    emit(in_samples * up / down, true);
    return {in_samples, m};
}

}  // namespace iqgen
