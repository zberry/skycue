#include "coding/modes_crc.hpp"

#include <array>

namespace skycued::coding {

namespace {

// The generator without its x^24 term.
constexpr uint32_t kModesGenerator = 0xFFF409;

// The residual of the 112-bit frame with only bit i set, for each i.
// The residual is linear, so this is the syndrome of a flip at bit i.
const std::array<uint32_t, 112>& singleBitSyndromes()
{
    static const std::array<uint32_t, 112> table = [] {
        std::array<uint32_t, 112> t{};
        for (std::size_t bit = 0; bit < t.size(); ++bit) {
            std::vector<uint8_t> frame(14, 0);
            frame[bit / 8] = static_cast<uint8_t>(0x80u >> (bit % 8));
            t[bit] = modesCrcResidual(frame);
        }
        return t;
    }();
    return table;
}

}  // namespace

uint32_t modesCrcResidual(const std::vector<uint8_t>& frame)
{
    // Bitwise long division: shift each bit into a 24-bit remainder; a bit
    // shifted out at x^24 is replaced by the rest of the generator.
    uint32_t rem = 0;
    for (uint8_t byte : frame) {
        for (int i = 7; i >= 0; --i) {
            const bool top = (rem & 0x800000u) != 0;
            rem = ((rem << 1) | ((byte >> i) & 1u)) & 0xFFFFFFu;
            if (top)
                rem ^= kModesGenerator;
        }
    }
    return rem;
}

int modesDf(const std::vector<uint8_t>& frame)
{
    if (frame.empty())
        return -1;
    return frame[0] >> 3;
}

bool modesClean(const std::vector<uint8_t>& frame)
{
    const int df = modesDf(frame);
    // Other DFs carry an address or data in the parity field and are never clean, so only these need a length check.
    const bool shape = (df == 11 && frame.size() == 7) || ((df == 17 || df == 18) && frame.size() == 14);
    return shape && modesCrcResidual(frame) == 0;
}

bool syndromeFix1(std::vector<uint8_t>& frame)
{
    if (frame.size() != 14)
        return false;
    const uint32_t residual = modesCrcResidual(frame);
    if (residual == 0)
        return false;
    const auto& table = singleBitSyndromes();
    // Bits 0..4 are the DF field: flipping one would change the frame's kind.
    for (std::size_t bit = 5; bit < table.size(); ++bit) {
        if (table[bit] == residual) {
            frame[bit / 8] ^= static_cast<uint8_t>(0x80u >> (bit % 8));
            return true;
        }
    }
    return false;
}

}  // namespace skycued::coding
