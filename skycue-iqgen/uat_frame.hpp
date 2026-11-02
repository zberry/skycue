#pragma once

// UAT frames as transmitted: the 36-bit sync word, then the payload with its
// Reed-Solomon parity (downlink), or six 72-byte blocks each with 20 parity
// bytes, interleaved byte by byte (uplink). Payloads are what dump978 prints
// between the '+'/'-' and the first ';'.

#include "reed_solomon.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace iqgen {

enum class UatKind { downlink_short, downlink_long, uplink };

// 18, 34 or 432 payload bytes; nullopt for any other length.
std::optional<UatKind> uatKindForPayload(std::size_t bytes);

// The transmitted bytes after the sync word: 30, 48 or 552 (interleaved).
// Throws std::invalid_argument for a payload of any other length.
std::vector<uint8_t> uatEncode(const std::vector<uint8_t>& payload);

// Sync (36 bits) + 8 bits per encoded byte, MSB first, one bit per element.
// Throws as uatEncode does.
std::vector<uint8_t> uatFrameBits(const std::vector<uint8_t>& payload);

// Samples in a whole frame at 2 samples per bit: 552, 840 or 8904.
std::size_t uatFrameSamples(UatKind kind);

// The Reed-Solomon code of a kind (per block for the uplink).
const ReedSolomon& uatCode(UatKind kind);

}  // namespace iqgen
