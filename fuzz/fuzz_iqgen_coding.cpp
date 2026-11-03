// libFuzzer harness: the skycue-iqgen frame coding helpers (pure, in
// memory). Mode byte (fuzz_util.hpp splitMode):
//   0  Reed-Solomon: the payload's first byte picks the UAT code (downlink
//      short, downlink long, uplink block: uatCode(), the parameters
//      uat_frame.cpp encodes with), the second the byte to corrupt, the rest
//      is the data, k = min(size, 255 - nroots) bytes. Data + parity has all
//      syndromes 0; with one byte changed some syndrome is not 0. A data
//      part of 18, 34 or 432 bytes also goes through uatEncode and
//      uatFrameBits (36 sync bits + 8 per encoded byte, each 0 or 1).
//   1  Mode S CRC: a 7-byte frame (payload under 14 bytes) or a 14-byte one,
//      zero-padded, with its last 3 bytes set to modesCrcResidual of the
//      frame with them zeroed; the residual is then 0.
//   2  parseHex: on success, toHex(out, lowercase) is the lowercased
//      payload; on failure out is unchanged.
// Seeds: seeds/iqgen_coding/ (docs/hardening.md "Fuzzing").

#include "bits.hpp"
#include "fuzz_util.hpp"
#include "modes_crc.hpp"
#include "reed_solomon.hpp"
#include "uat_frame.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace {

bool allZero(const std::vector<uint8_t>& v)
{
    return std::all_of(v.begin(), v.end(), [](uint8_t b) { return b == 0; });
}

void reedSolomon(const std::string& payload)
{
    if (payload.size() < 2)
        return;
    static const iqgen::UatKind kinds[] = {iqgen::UatKind::downlink_short, iqgen::UatKind::downlink_long,
                                           iqgen::UatKind::uplink};
    const iqgen::ReedSolomon& rs = iqgen::uatCode(kinds[static_cast<uint8_t>(payload[0]) % 3]);
    const auto flip_at = static_cast<uint8_t>(payload[1]);
    const std::string data_text = payload.substr(2);

    const std::size_t k = std::min<std::size_t>(data_text.size(), 255 - rs.nroots());
    std::vector<uint8_t> word(data_text.begin(), data_text.begin() + static_cast<std::ptrdiff_t>(k));
    const auto parity = rs.parity(word.data(), k);
    FUZZ_CHECK(parity.size() == rs.nroots());
    word.insert(word.end(), parity.begin(), parity.end());
    FUZZ_CHECK(allZero(rs.syndromes(word.data(), word.size())));

    // One changed byte is within the code's distance (nroots + 1 >= 2).
    word[flip_at % word.size()] ^= static_cast<uint8_t>(flip_at | 1u);
    FUZZ_CHECK(!allZero(rs.syndromes(word.data(), word.size())));

    const std::vector<uint8_t> data(data_text.begin(), data_text.end());
    if (const auto kind = iqgen::uatKindForPayload(data.size())) {
        const auto encoded = iqgen::uatEncode(data);
        const auto bits = iqgen::uatFrameBits(data);
        FUZZ_CHECK(bits.size() == 36 + 8 * encoded.size());
        FUZZ_CHECK(std::all_of(bits.begin(), bits.end(), [](uint8_t b) { return b <= 1; }));
        FUZZ_CHECK(iqgen::uatFrameSamples(*kind) == 2 * bits.size());
    }
}

void modesCrc(const std::string& payload)
{
    const std::size_t n = payload.size() < 14 ? 7 : 14;
    std::vector<uint8_t> frame(n, 0);
    std::copy_n(payload.begin(), std::min(n, payload.size()), frame.begin());
    frame[n - 3] = frame[n - 2] = frame[n - 1] = 0;
    const uint32_t crc = iqgen::modesCrcResidual(frame);
    FUZZ_CHECK(crc <= 0xffffffu);
    frame[n - 3] = static_cast<uint8_t>(crc >> 16);
    frame[n - 2] = static_cast<uint8_t>(crc >> 8);
    frame[n - 1] = static_cast<uint8_t>(crc);
    FUZZ_CHECK(iqgen::modesCrcResidual(frame) == 0);
}

void hex(const std::string& payload)
{
    const std::vector<uint8_t> sentinel{0xde, 0xad};
    std::vector<uint8_t> out = sentinel;
    if (!iqgen::parseHex(payload, out)) {
        FUZZ_CHECK(out == sentinel);
        return;
    }
    std::string lower = payload;
    for (char& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    FUZZ_CHECK(iqgen::toHex(out, false) == lower);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, std::size_t size)
{
    skycuefuzz::canary(data, size);
    skycuefuzz::quietLogs();
    const auto in = skycuefuzz::splitMode(data, size, 3);
    if (in.mode == 0)
        reedSolomon(in.payload);
    else if (in.mode == 1)
        modesCrc(in.payload);
    else
        hex(in.payload);
    return 0;
}
