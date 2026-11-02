#include "modes_crc.hpp"

namespace iqgen {

namespace {

// The generator without its x^24 term.
constexpr uint32_t kModesGenerator = 0xFFF409;

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
    const bool shape = (df == 11 && frame.size() == 7) || ((df == 17 || df == 18) && frame.size() == 14);
    return shape && modesCrcResidual(frame) == 0;
}

}  // namespace iqgen
