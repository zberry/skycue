// libFuzzer harness: the FIS-B decoder fed one uplink line per input,
// in memory (no recorder, no socket). Input: mode byte (fuzz_util.hpp
// splitMode), then a byte whose low bit picks the decoder (0 normal,
// 1 experimental; one static decoder each, so segment reassembly builds up
// across inputs), then the payload:
//   0  the payload as raw uplink bytes, fed as "+" + lowercase hex + ";"
//   1  the payload fed as the line itself
// Every product's text and TWGO records go through the fisb_json.hpp writers
// the web uses; the JSON must be UTF-8 and parse as an object. TWGO shapes
// and global blocks must be inside the lat/lon range the decoder enforces
// (fisb_decoder.hpp). Seeds: seeds/fisb_decoder/ (docs/hardening.md
// "Fuzzing").

#include "fisb/fisb_decoder.hpp"
#include "fisb/fisb_json.hpp"
#include "fisb/fisb_store.hpp"
#include "fuzz_util.hpp"
#include "json_lite.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>

namespace {

using skycued::fisb::TimePoint;

// 2020-06-01T00:00:00Z.
const TimePoint kRef{std::chrono::seconds(1590969600)};

void checkJson(const std::string& json)
{
    FUZZ_CHECK(skycuefuzz::validUtf8(json));
    FUZZ_CHECK(skycued::JsonObject::parse(json, 1u << 24).has_value());
}

bool inRange(double lat, double lon)
{
    return lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180;
}

void onProduct(const skycued::fisb::FisbProduct& p)
{
    using namespace skycued::fisb;
    if (const auto* text = std::get_if<std::vector<TextRecord>>(&p.body)) {
        for (const TextRecord& r : *text)
            checkJson(textRecordJson(r));
    } else if (const auto* t = std::get_if<TwgoParsed>(&p.body)) {
        // The record the store would build from this half (fisb_store.cpp
        // applyTwgo), written as the web writes it.
        TwgoRecord r;
        r.key = t->key;
        r.text = t->text;
        r.shapes = t->shapes;
        r.has_graphics = t->has_graphics;
        r.complete = t->text.has_value();
        r.cancelled = t->cancel;
        r.start = t->start;
        r.graphics_end = t->end;
        r.location = t->location;
        r.label = t->label;
        r.received = r.last_received = r.expires = p.received;
        checkJson(twgoRecordJson(r));
        for (const Shape& s : t->shapes) {
            for (const auto& pt : s.points)
                FUZZ_CHECK(inRange(pt.first, pt.second));
            FUZZ_CHECK(skycuefuzz::validUtf8(shapeJson(s)));
        }
    } else if (const auto* b = std::get_if<DecodedBlocks>(&p.body)) {
        for (const RadarBlock& rb : b->blocks)
            FUZZ_CHECK(inRange(rb.lat0, rb.lon0));
    }
}

skycued::fisb::FisbDecoder& decoder(bool experimental)
{
    static skycued::fisb::FisbDecoder normal(false);
    static skycued::fisb::FisbDecoder exp(true);
    static const bool wired = [] {
        normal.onProduct(onProduct);
        exp.onProduct(onProduct);
        return true;
    }();
    (void)wired;
    return experimental ? exp : normal;
}

std::string hexLine(const std::string& bytes)
{
    static const char kDigits[] = "0123456789abcdef";
    std::string line = "+";
    for (unsigned char c : bytes) {
        line += kDigits[c >> 4];
        line += kDigits[c & 0x0f];
    }
    line += ';';
    return line;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, std::size_t size)
{
    skycuefuzz::canary(data, size);
    skycuefuzz::quietLogs();
    const auto in = skycuefuzz::splitMode(data, size, 2);
    const bool experimental = !in.payload.empty() && (static_cast<uint8_t>(in.payload[0]) & 1u) != 0;
    const std::string payload = in.payload.empty() ? std::string() : in.payload.substr(1);

    auto& d = decoder(experimental);
    d.feed(kRef, in.mode == 0 ? hexLine(payload) : payload);

    // Every 256 inputs, drop the segment sets (older than 60 min at ref + 61
    // min: all of them), so memory stays bounded.
    static unsigned count = 0;
    if (++count % 256 == 0) {
        decoder(false).sweep(kRef + std::chrono::minutes(61));
        decoder(true).sweep(kRef + std::chrono::minutes(61));
    }
    return 0;
}
