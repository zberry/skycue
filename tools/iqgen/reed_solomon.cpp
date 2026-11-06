#include "reed_solomon.hpp"

#include <stdexcept>

namespace iqgen {

namespace {

constexpr unsigned kFieldSize = 255;  // non-zero elements; also the longest codeword

unsigned gcd(unsigned a, unsigned b)
{
    while (b != 0) {
        const unsigned t = a % b;
        a = b;
        b = t;
    }
    return a;
}

}  // namespace

ReedSolomon::ReedSolomon(unsigned gfpoly, unsigned fcr, unsigned prim, unsigned nroots) : nroots_(nroots)
{
    if (gfpoly < 0x100 || gfpoly > 0x1ff)
        throw std::invalid_argument("ReedSolomon: gfpoly must have degree 8");
    if (nroots < 1 || nroots >= kFieldSize)
        throw std::invalid_argument("ReedSolomon: nroots must be in [1, 255)");
    if (prim % kFieldSize == 0 || gcd(prim % kFieldSize, kFieldSize) != 1)
        throw std::invalid_argument("ReedSolomon: prim must be coprime to 255");

    // Powers of alpha = x, reduced by gfpoly. The polynomial is primitive
    // when they visit every non-zero element exactly once.
    std::array<bool, 256> seen{};
    unsigned x = 1;
    for (unsigned i = 0; i < kFieldSize; ++i) {
        if (x == 0 || seen[x])
            throw std::invalid_argument("ReedSolomon: gfpoly is not primitive");
        seen[x] = true;
        exp_[i] = static_cast<uint8_t>(x);
        log_[x] = static_cast<uint8_t>(i);
        x <<= 1;
        if (x & 0x100u)
            x ^= gfpoly;
    }
    if (x != 1)
        throw std::invalid_argument("ReedSolomon: gfpoly is not primitive");

    // g(x) = prod (x - r_i) = prod (x + r_i), built one factor at a time,
    // highest power first.
    roots_.resize(nroots);
    gen_.assign(1, 1);
    for (unsigned i = 0; i < nroots; ++i) {
        const auto e = static_cast<unsigned>((static_cast<unsigned long long>(fcr) + i) * prim % kFieldSize);
        const uint8_t r = exp_[e];
        roots_[i] = r;
        std::vector<uint8_t> next(gen_.size() + 1, 0);
        for (std::size_t j = 0; j < gen_.size(); ++j) {
            next[j] = static_cast<uint8_t>(next[j] ^ gen_[j]);                  // the x term
            next[j + 1] = static_cast<uint8_t>(next[j + 1] ^ mul(gen_[j], r));  // the r term
        }
        gen_.swap(next);
    }
}

uint8_t ReedSolomon::mul(uint8_t a, uint8_t b) const
{
    if (a == 0 || b == 0)
        return 0;
    return exp_[(static_cast<unsigned>(log_[a]) + log_[b]) % kFieldSize];
}

std::vector<uint8_t> ReedSolomon::parity(const uint8_t* data, std::size_t k) const
{
    if (k + nroots_ > kFieldSize)
        throw std::invalid_argument("ReedSolomon::parity: codeword longer than 255 bytes");
    // Long division of d(x) * x^nroots by the monic g(x): rem holds the
    // running remainder, highest power first.
    std::vector<uint8_t> rem(nroots_, 0);
    for (std::size_t i = 0; i < k; ++i) {
        const auto fb = static_cast<uint8_t>(data[i] ^ rem[0]);
        for (unsigned j = 0; j + 1 < nroots_; ++j)
            rem[j] = static_cast<uint8_t>(rem[j + 1] ^ mul(fb, gen_[j + 1]));
        rem[nroots_ - 1] = mul(fb, gen_[nroots_]);
    }
    return rem;
}

std::vector<uint8_t> ReedSolomon::syndromes(const uint8_t* codeword, std::size_t n) const
{
    if (n > kFieldSize)
        throw std::invalid_argument("ReedSolomon::syndromes: codeword longer than 255 bytes");
    // c(r_i) by Horner's rule, byte 0 the highest power.
    std::vector<uint8_t> s(nroots_, 0);
    for (unsigned i = 0; i < nroots_; ++i) {
        uint8_t acc = 0;
        for (std::size_t j = 0; j < n; ++j)
            acc = static_cast<uint8_t>(mul(acc, roots_[i]) ^ codeword[j]);
        s[i] = acc;
    }
    return s;
}

}  // namespace iqgen
