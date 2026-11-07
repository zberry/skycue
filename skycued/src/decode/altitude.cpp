#include "decode/altitude.hpp"

// Field layouts: ICAO Annex 10 Vol IV (Mode S surveillance, AC and ID fields)
// as summarised in The 1090 MHz Riddle (Junzi Sun), "Downlink formats" and
// "Airborne position" chapters. The Gillham (Mode C) code is the
// 11-bit reflected-binary altitude code of the same Annex, 500 ft part in
// D2 D4 A1 A2 A4 B1 B2 B4 (D2 most significant) and a five-state 100 ft part
// in C1 C2 C4.

namespace skycued::decode {

namespace {

// AC13 bit positions (value bit index), first wire bit is bit 12.
constexpr unsigned kC1 = 12, kA1 = 11, kC2 = 10, kA2 = 9, kC4 = 8, kA4 = 7;
constexpr unsigned kM = 6, kB1 = 5, kQ = 4, kB2 = 3, kD2 = 2, kB4 = 1, kD4 = 0;

unsigned bit(uint32_t v, unsigned i) { return (v >> i) & 1u; }

std::optional<int32_t> gillham(uint32_t v)
{
    // 100 ft part: C1 C2 C4 as a 3-bit value; 1,3,2,6,4 are the five legal
    // states in order, 0, 5 and 7 are illegal.
    const unsigned c = (bit(v, kC1) << 2) | (bit(v, kC2) << 1) | bit(v, kC4);
    // The cycle runs 001, 011, 010, 110, 100 for the 100 ft steps 1..5.
    static const int step[8] = {0, 1, 3, 2, 5, 0, 4, 0};
    const int oh = step[c];
    if (oh == 0)
        return std::nullopt;

    // 500 ft part: reflected binary, D1 is not transmitted.
    unsigned g = (bit(v, kD2) << 7) | (bit(v, kD4) << 6) | (bit(v, kA1) << 5) | (bit(v, kA2) << 4)
        | (bit(v, kA4) << 3) | (bit(v, kB1) << 2) | (bit(v, kB2) << 1) | bit(v, kB4);
    unsigned n = 0;
    for (; g != 0; g >>= 1)
        n ^= g;

    int hundredsOfFt = oh;
    if (n & 1u)
        hundredsOfFt = 6 - oh;  // the 100 ft cycle runs backwards on odd 500 ft steps
    return static_cast<int32_t>(n) * 500 + hundredsOfFt * 100 - 1300;
}

}  // namespace

std::optional<int32_t> altitudeAc13(uint32_t ac13)
{
    if (ac13 == 0 || ac13 > 0x1FFFu)
        return std::nullopt;
    if (bit(ac13, kM))
        return std::nullopt;
    if (bit(ac13, kQ)) {
        // 11-bit N: drop M (bit 6) and Q (bit 4).
        const uint32_t n = ((ac13 & 0x1F80u) >> 2) | ((ac13 & 0x20u) >> 1) | (ac13 & 0xFu);
        return static_cast<int32_t>(n) * 25 - 1000;
    }
    return gillham(ac13);
}

std::optional<int32_t> altitudeAc12(uint32_t ac12)
{
    if (ac12 == 0 || ac12 > 0xFFFu)
        return std::nullopt;
    // Re-insert a zero M bit between the 6th and 7th bit.
    const uint32_t ac13 = ((ac12 & 0xFC0u) << 1) | (ac12 & 0x3Fu);
    return altitudeAc13(ac13);
}

std::optional<std::string> squawkId13(uint32_t id13)
{
    if (id13 > 0x1FFFu)
        return std::nullopt;
    // Wire order: C1 A1 C2 A2 C4 A4 X B1 D1 B2 D2 B4 D4.
    const unsigned c1 = bit(id13, 12), a1 = bit(id13, 11), c2 = bit(id13, 10), a2 = bit(id13, 9);
    const unsigned c4 = bit(id13, 8), a4 = bit(id13, 7), b1 = bit(id13, 5), d1 = bit(id13, 4);
    const unsigned b2 = bit(id13, 3), d2 = bit(id13, 2), b4 = bit(id13, 1), d4 = bit(id13, 0);
    std::string s(4, '0');
    s[0] = static_cast<char>('0' + (a4 << 2 | a2 << 1 | a1));
    s[1] = static_cast<char>('0' + (b4 << 2 | b2 << 1 | b1));
    s[2] = static_cast<char>('0' + (c4 << 2 | c2 << 1 | c1));
    s[3] = static_cast<char>('0' + (d4 << 2 | d2 << 1 | d1));
    return s;
}

}  // namespace skycued::decode
