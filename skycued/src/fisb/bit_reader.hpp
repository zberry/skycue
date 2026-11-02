#pragma once

// MSB-first bit reader over a byte buffer. Every read is bounds-checked: a
// read past the end returns nullopt and latches overrun(), so a parser can
// read a run of fields and test once.

#include <cstddef>
#include <cstdint>
#include <optional>

namespace skycued {
namespace fisb {

class BitReader {
public:
    BitReader(const uint8_t* data, std::size_t nbytes) : data_(data), nbits_(data ? nbytes * 8 : 0) {}

    // The next n bits (0..32) as an unsigned value, first bit most significant.
    std::optional<uint32_t> u(unsigned n)
    {
        if (overrun_ || n > 32 || n > nbits_ - pos_) {
            overrun_ = true;
            return std::nullopt;
        }
        uint32_t v = 0;
        for (unsigned i = 0; i < n; ++i, ++pos_)
            v = (v << 1) | ((data_[pos_ >> 3] >> (7 - (pos_ & 7))) & 1u);
        return v;
    }
    // Like u(), narrowed to 8 bits (n <= 8).
    std::optional<uint8_t> u8(unsigned n)
    {
        auto v = n <= 8 ? u(n) : std::nullopt;
        if (!v) {
            overrun_ = true;
            return std::nullopt;
        }
        return static_cast<uint8_t>(*v);
    }

    bool overrun() const { return overrun_; }
    std::size_t pos() const { return pos_; }  // bits consumed
    std::size_t remaining() const { return nbits_ - pos_; }
    std::size_t bytesConsumed() const { return (pos_ + 7) / 8; }  // ceil(pos / 8)

private:
    const uint8_t* data_;
    std::size_t nbits_;
    std::size_t pos_ = 0;
    bool overrun_ = false;
};

}  // namespace fisb
}  // namespace skycued
