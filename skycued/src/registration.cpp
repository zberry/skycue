#include "registration.hpp"

#include <cstddef>

namespace skycued {
namespace registration {

namespace {

constexpr uint32_t kFirst = 0xA00001;
constexpr char kLetters[] = "ABCDEFGHJKLMNPQRSTUVWXYZ"; // no I, no O
constexpr uint32_t kLetterCount = 24;
constexpr size_t kMaxDigits = 5;

// Size of the subtree under a prefix of (index + 1) digits.
constexpr uint32_t kSubtree[kMaxDigits] = {101711, 10111, 951, 35, 1};
constexpr uint32_t kTotal = 9 * kSubtree[0]; // N1..N9 subtrees

// Number of codes a prefix of `digits` digits owns itself (bare + suffixes).
constexpr uint32_t ownCodes(size_t digits)
{
    return digits <= 3 ? 1 + kLetterCount + kLetterCount * kLetterCount : digits == 4 ? 1 + kLetterCount : 1;
}

int letterIndex(char c)
{
    for (uint32_t i = 0; i < kLetterCount; ++i)
        if (kLetters[i] == c)
            return static_cast<int>(i);
    return -1;
}

char toUpper(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }

} // namespace

std::optional<std::string> nNumberFromIcao(uint32_t icao)
{
    if (icao < kFirst || icao - kFirst >= kTotal)
        return std::nullopt;
    uint32_t rem = icao - kFirst;
    std::string out = "N";
    out += static_cast<char>('1' + rem / kSubtree[0]);
    rem %= kSubtree[0];
    for (size_t digits = 1;; ++digits) {
        const uint32_t own = ownCodes(digits);
        if (rem < own) {
            if (rem == 0)
                return out;
            const uint32_t c = rem - 1;
            if (digits == 4) {
                out += kLetters[c];
                return out;
            }
            out += kLetters[c / (kLetterCount + 1)];
            if (c % (kLetterCount + 1) != 0)
                out += kLetters[c % (kLetterCount + 1) - 1];
            return out;
        }
        // digits < 5 here: a 5-digit prefix owns its whole (size 1) subtree.
        rem -= own;
        out += static_cast<char>('0' + rem / kSubtree[digits]);
        rem %= kSubtree[digits];
    }
}

std::optional<uint32_t> icaoFromNNumber(const std::string& n)
{
    size_t pos = 0;
    if (pos < n.size() && toUpper(n[pos]) == 'N')
        ++pos;

    std::string digits;
    while (pos < n.size() && n[pos] >= '0' && n[pos] <= '9')
        digits += n[pos++];
    if (digits.empty() || digits.size() > kMaxDigits || digits[0] == '0')
        return std::nullopt;

    int letters[2] = {-1, -1};
    size_t nletters = 0;
    while (pos < n.size()) {
        const int li = letterIndex(toUpper(n[pos++]));
        if (li < 0 || nletters == 2)
            return std::nullopt;
        letters[nletters++] = li;
    }
    if ((digits.size() == 4 && nletters > 1) || (digits.size() == 5 && nletters > 0))
        return std::nullopt;

    uint32_t offset = static_cast<uint32_t>(digits[0] - '1') * kSubtree[0];
    for (size_t i = 1; i < digits.size(); ++i)
        offset += ownCodes(i) + static_cast<uint32_t>(digits[i] - '0') * kSubtree[i];

    if (nletters == 1 && digits.size() == 4) {
        offset += 1 + static_cast<uint32_t>(letters[0]);
    } else if (nletters >= 1) {
        offset += 1 + static_cast<uint32_t>(letters[0]) * (kLetterCount + 1);
        if (nletters == 2)
            offset += 1 + static_cast<uint32_t>(letters[1]);
    }
    return kFirst + offset;
}

} // namespace registration
} // namespace skycued
