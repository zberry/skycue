#include "uat_frame.hpp"

#include "coding/bits.hpp"

#include <stdexcept>

namespace iqgen {

namespace {

// Frame constants and Reed-Solomon parameters (fcr 120, prim 1) of the UAT
// physical layer.
constexpr unsigned kSyncBits = 36;
constexpr uint64_t kDownlinkSyncWord = 0xEACDDA4E2ULL;
constexpr uint64_t kUplinkSyncWord = 0x153225B1DULL;

constexpr std::size_t kDownlinkShortDataBytes = 18;  // 144 bits
constexpr std::size_t kDownlinkShortBytes = 30;      // + 96 parity bits
constexpr std::size_t kDownlinkLongDataBytes = 34;   // 272 bits
constexpr std::size_t kDownlinkLongBytes = 48;       // + 112 parity bits
constexpr std::size_t kUplinkBlockDataBytes = 72;    // 576 bits
constexpr std::size_t kUplinkBlockBytes = 92;        // + 160 parity bits
constexpr std::size_t kUplinkBlocks = 6;
constexpr std::size_t kUplinkDataBytes = kUplinkBlockDataBytes * kUplinkBlocks;  // 432
constexpr std::size_t kUplinkBytes = kUplinkBlockBytes * kUplinkBlocks;          // 552

constexpr unsigned kPoly = 0x187;
constexpr unsigned kFcr = 120;
constexpr unsigned kPrim = 1;
constexpr unsigned kDownlinkShortRoots = 12;
constexpr unsigned kDownlinkLongRoots = 14;
constexpr unsigned kUplinkBlockRoots = 20;

constexpr std::size_t kSamplesPerBit = 2;

std::size_t encodedBytes(UatKind kind)
{
    switch (kind) {
    case UatKind::downlink_short:
        return kDownlinkShortBytes;
    case UatKind::downlink_long:
        return kDownlinkLongBytes;
    case UatKind::uplink:
        break;
    }
    return kUplinkBytes;
}

}  // namespace

std::optional<UatKind> uatKindForPayload(std::size_t bytes)
{
    switch (bytes) {
    case kDownlinkShortDataBytes:
        return UatKind::downlink_short;
    case kDownlinkLongDataBytes:
        return UatKind::downlink_long;
    case kUplinkDataBytes:
        return UatKind::uplink;
    default:
        return std::nullopt;
    }
}

const ReedSolomon& uatCode(UatKind kind)
{
    static const ReedSolomon down_short(kPoly, kFcr, kPrim, kDownlinkShortRoots);
    static const ReedSolomon down_long(kPoly, kFcr, kPrim, kDownlinkLongRoots);
    static const ReedSolomon up(kPoly, kFcr, kPrim, kUplinkBlockRoots);
    switch (kind) {
    case UatKind::downlink_short:
        return down_short;
    case UatKind::downlink_long:
        return down_long;
    case UatKind::uplink:
        break;
    }
    return up;
}

std::vector<uint8_t> uatEncode(const std::vector<uint8_t>& payload)
{
    const auto kind = uatKindForPayload(payload.size());
    if (!kind)
        throw std::invalid_argument("uatEncode: payload is not 18, 34 or 432 bytes");
    const ReedSolomon& rs = uatCode(*kind);

    if (*kind != UatKind::uplink) {
        std::vector<uint8_t> out = payload;
        const auto par = rs.parity(payload.data(), payload.size());
        out.insert(out.end(), par.begin(), par.end());
        return out;
    }

    // Block j is payload bytes [72j, 72j + 72) and its 20 parity bytes;
    // byte i of block j is transmitted at 6i + j.
    std::vector<uint8_t> out(kUplinkBytes);
    for (std::size_t j = 0; j < kUplinkBlocks; ++j) {
        const uint8_t* data = payload.data() + kUplinkBlockDataBytes * j;
        const auto par = rs.parity(data, kUplinkBlockDataBytes);
        for (std::size_t i = 0; i < kUplinkBlockBytes; ++i)
            out[kUplinkBlocks * i + j] = i < kUplinkBlockDataBytes ? data[i] : par[i - kUplinkBlockDataBytes];
    }
    return out;
}

std::vector<uint8_t> uatFrameBits(const std::vector<uint8_t>& payload)
{
    const auto encoded = uatEncode(payload);  // throws on a bad length
    const bool uplink = uatKindForPayload(payload.size()) == UatKind::uplink;
    std::vector<uint8_t> bits;
    bits.reserve(kSyncBits + 8 * encoded.size());
    skycued::coding::appendBits(bits, uplink ? kUplinkSyncWord : kDownlinkSyncWord, kSyncBits);
    skycued::coding::appendBytes(bits, encoded.data(), encoded.size());
    return bits;
}

std::size_t uatFrameSamples(UatKind kind)
{
    return kSamplesPerBit * (kSyncBits + 8 * encodedBytes(kind));
}

}  // namespace iqgen
