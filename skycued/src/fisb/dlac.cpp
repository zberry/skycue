#include "fisb/dlac.hpp"

#include "fisb/bit_reader.hpp"

namespace skycued {
namespace fisb {

namespace {

constexpr unsigned kEtx = 0, kNullChar = 27, kTab = 28, kRecordSep = 29, kNewline = 30;

}  // namespace

std::string decodeDlac(const uint8_t* p, std::size_t n)
{
    std::string out;
    BitReader r(p, n);
    bool tab = false;
    while (r.remaining() >= 6) {
        const unsigned c = *r.u(6);
        if (tab) {
            out.append(c, ' ');
            tab = false;
            continue;
        }
        if (c == kEtx)
            break;
        if (c == kTab)
            tab = true;
        else if (c == kNullChar)
            continue;
        else if (c == kRecordSep)
            out += '\x1e';
        else if (c == kNewline)
            out += '\n';
        else if (c <= 26)
            out += static_cast<char>('A' + c - 1);
        else  // 31..63: '|', then ' ' (32) .. '?' (63), which is ASCII order
            out += c == 31 ? '|' : static_cast<char>(' ' + (c - 32));
    }
    return out;
}

}  // namespace fisb
}  // namespace skycued
