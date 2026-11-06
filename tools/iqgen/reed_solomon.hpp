#pragma once

// Systematic Reed-Solomon encoder and syndrome check over GF(2^8), written
// from the definitions:
//   - the field is GF(2)[x] / gfpoly with primitive element alpha = x (2);
//   - generator g(x) = prod_{i=0}^{nroots-1} (x - alpha^((fcr + i) * prim));
//   - codeword byte 0 is the coefficient of x^(n-1); the parity is
//     (d(x) * x^nroots) mod g(x), appended after the data;
//   - syndrome S_i = c(alpha^((fcr + i) * prim)), all zero for a codeword.
// A shortened code needs no padding: leading zero bytes change nothing.

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace iqgen {

class ReedSolomon {
public:
    // gfpoly: a primitive degree-8 polynomial (0x100..0x1ff); 1 <= nroots < 255;
    // prim coprime to 255. Throws std::invalid_argument otherwise.
    ReedSolomon(unsigned gfpoly, unsigned fcr, unsigned prim, unsigned nroots);

    // The nroots parity bytes of the k data bytes at data.
    // Throws std::invalid_argument when k + nroots > 255.
    std::vector<uint8_t> parity(const uint8_t* data, std::size_t k) const;

    // The nroots syndromes of the n-byte codeword at codeword (data then
    // parity). Throws std::invalid_argument when n > 255.
    std::vector<uint8_t> syndromes(const uint8_t* codeword, std::size_t n) const;

    unsigned nroots() const { return nroots_; }

private:
    uint8_t mul(uint8_t a, uint8_t b) const;

    unsigned nroots_;
    std::array<uint8_t, 256> log_{};  // log_[0] unused
    std::array<uint8_t, 255> exp_{};  // alpha^i, i in [0, 255)
    std::vector<uint8_t> roots_;      // alpha^((fcr + i) * prim)
    std::vector<uint8_t> gen_;        // g(x), x^nroots first: gen_[0] == 1
};

}  // namespace iqgen
