#include "bits.hpp"

#include <stdexcept>

namespace iqgen {

namespace {

int nibble(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

}  // namespace

bool parseHex(const std::string& hex, std::vector<uint8_t>& out)
{
    if (hex.size() % 2 != 0)
        return false;
    std::vector<uint8_t> bytes;
    bytes.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const int hi = nibble(hex[i]), lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        bytes.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    out.swap(bytes);
    return true;
}

std::string toHex(const std::vector<uint8_t>& bytes, bool upper)
{
    const char* d = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    std::string s;
    s.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) {
        s += d[b >> 4];
        s += d[b & 15];
    }
    return s;
}

void appendBits(std::vector<uint8_t>& bits, uint64_t value, unsigned nbits)
{
    if (nbits > 64)
        throw std::invalid_argument("appendBits: more than 64 bits");
    for (unsigned i = nbits; i-- > 0;)
        bits.push_back(static_cast<uint8_t>((value >> i) & 1u));
}

void appendBytes(std::vector<uint8_t>& bits, const uint8_t* p, std::size_t n)
{
    bits.reserve(bits.size() + 8 * n);
    for (std::size_t i = 0; i < n; ++i)
        appendBits(bits, p[i], 8);
}

}  // namespace iqgen
