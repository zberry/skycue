#include "iq_writer.hpp"

#include <algorithm>
#include <cmath>
#include <istream>
#include <ostream>
#include <stdexcept>

namespace iqgen {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;
constexpr std::size_t kChunkSamples = 65536;

double wrapPhase(double phase)
{
    if (phase >= kPi)
        phase -= kTwoPi;
    else if (phase < -kPi)
        phase += kTwoPi;
    return phase;
}

}  // namespace

double amplitudeForLevel(double dbfs)
{
    return 128.0 * std::pow(10.0, dbfs / 20.0);
}

uint8_t quantiseCu8(double v, uint64_t& clipped)
{
    const double r = std::round(128.0 + v);
    if (r < 0.0) {
        ++clipped;
        return 0;
    }
    if (r > 255.0) {
        ++clipped;
        return 255;
    }
    return static_cast<uint8_t>(r);
}

IqWriter::IqWriter(std::ostream& out, double rate, double amplitude, std::optional<double> snr_db, double cfo_hz,
                   uint64_t seed)
    : out_(out),
      sigma_(snr_db ? amplitude * std::pow(10.0, -*snr_db / 20.0) / std::sqrt(2.0) : 0.0),
      phase_step_(wrapPhase(std::remainder(kTwoPi * cfo_hz / rate, kTwoPi))),
      rng_(seed)
{
    bytes_.reserve(2 * kChunkSamples);
}

double IqWriter::uniform()
{
    // The top 53 bits as k in [0, 2^53), mapped to (k + 1) / 2^53.
    return (static_cast<double>(rng_() >> 11) + 1.0) * (1.0 / 9007199254740992.0);
}

void IqWriter::write(const std::complex<float>* x, std::size_t n)
{
    std::size_t done = 0;
    while (done < n) {
        const std::size_t count = std::min(kChunkSamples, n - done);
        bytes_.clear();
        for (std::size_t k = 0; k < count; ++k) {
            double re = static_cast<double>(x[done + k].real());
            double im = static_cast<double>(x[done + k].imag());
            if (phase_step_ != 0.0) {
                const double c = std::cos(phase_), s = std::sin(phase_);
                const double r = re * c - im * s;
                im = re * s + im * c;
                re = r;
                phase_ = wrapPhase(phase_ + phase_step_);
            }
            if (sigma_ > 0.0) {
                const double radius = std::sqrt(-2.0 * std::log(uniform()));
                const double angle = kTwoPi * uniform();
                re += sigma_ * radius * std::cos(angle);
                im += sigma_ * radius * std::sin(angle);
            }
            bytes_.push_back(static_cast<char>(quantiseCu8(re, clipped_)));
            bytes_.push_back(static_cast<char>(quantiseCu8(im, clipped_)));
        }
        out_.write(bytes_.data(), static_cast<std::streamsize>(bytes_.size()));
        if (!out_)
            throw std::runtime_error("write failed");
        samples_ += count;
        done += count;
    }
}

Cu8Info cu8Info(std::istream& in, double rate)
{
    Cu8Info info{0, 0.0, 0.0, 0.0, 0.0, 0.0};
    double sum_mag = 0.0, sum_i = 0.0, sum_q = 0.0;
    std::vector<char> buf(2 * kChunkSamples + 1);
    std::size_t carry = 0;  // a byte left over from the previous read
    while (in) {
        in.read(buf.data() + carry, static_cast<std::streamsize>(buf.size() - carry));
        const std::size_t have = carry + static_cast<std::size_t>(in.gcount());
        const std::size_t whole = have / 2;
        for (std::size_t k = 0; k < whole; ++k) {
            const double i = static_cast<double>(static_cast<uint8_t>(buf[2 * k])) - 128.0;
            const double q = static_cast<double>(static_cast<uint8_t>(buf[2 * k + 1])) - 128.0;
            const double mag = std::sqrt(i * i + q * q);
            sum_mag += mag;
            sum_i += i;
            sum_q += q;
            info.peak_mag = std::max(info.peak_mag, mag);
        }
        info.samples += whole;
        carry = have - 2 * whole;
        if (carry)
            buf[0] = buf[have - 1];
    }
    if (info.samples) {
        const auto n = static_cast<double>(info.samples);
        info.seconds = n / rate;
        info.mean_mag = sum_mag / n;
        info.dc_i = sum_i / n;
        info.dc_q = sum_q / n;
    }
    return info;
}

}  // namespace iqgen
