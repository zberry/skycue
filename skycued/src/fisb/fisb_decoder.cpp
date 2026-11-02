#include "fisb/fisb_decoder.hpp"

#include "fisb/apdu.hpp"
#include "fisb/fisb_time.hpp"
#include "fisb/text_products.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace skycued {
namespace fisb {

namespace {

bool pointInRange(const std::pair<double, double>& p)
{
    return std::isfinite(p.first) && std::isfinite(p.second) && std::fabs(p.first) <= 90.0 &&
           std::fabs(p.second) <= 180.0;
}

bool shapeInRange(const Shape& s)
{
    return std::all_of(s.points.begin(), s.points.end(), pointInRange);
}

}  // namespace

uint64_t stationKey(const UplinkHeader& h)
{
    const auto lat = static_cast<uint64_t>(std::llround(h.lat_deg * 1e4) + 900000);
    const auto lon = static_cast<uint64_t>(std::llround(h.lon_deg * 1e4) + 1800000);
    return (lat << 32) | lon;
}

FisbDecoder::FisbDecoder(bool experimental) : experimental_(experimental) {}

void FisbDecoder::onProduct(std::function<void(const FisbProduct&)> handler) { on_product_ = std::move(handler); }

void FisbDecoder::feed(TimePoint ref, const std::string& hex_line)
{
    ++counters_.uplinks;
    std::vector<uint8_t> bytes;  // local: a product callback may feed again
    bytes.reserve(kUplinkBytes);
    if (!hexToBytes(hex_line, bytes)) {
        ++counters_.bad_hex;
        return;
    }
    if (bytes.size() != kUplinkBytes) {
        ++counters_.bad_frames;
        return;
    }
    const auto uh = parseUplinkHeader(bytes.data(), bytes.size());
    if (!uh) {
        ++counters_.bad_frames;
        return;
    }
    if (!uh->app_data_valid) {
        ++counters_.not_app_data;
        return;
    }
    const uint64_t station = stationKey(*uh);
    const bool ok = forEachInfoFrame(bytes.data(), bytes.size(), [&](const InfoFrame& f) {
        if (f.type != 0)
            return;
        ApduStatus status = ApduStatus::ok;
        const auto h = parseApdu(f.data, f.len, experimental_, &status);
        if (!h) {
            if (status == ApduStatus::experimental_time_option)
                ++counters_.skipped_experimental;
            else
                ++counters_.bad_apdu;
            return;
        }
        ++counters_.apdus;
        apdu(*h, f.data + h->header_bytes, f.len - h->header_bytes, ref, *uh, station);
    });
    if (!ok)
        ++counters_.bad_frames;  // the frames before the overrun were still decoded
    counters_.segments_open = segments_.open();
}

void FisbDecoder::apdu(const ApduHeader& h, const uint8_t* p, std::size_t n, TimePoint ref, const UplinkHeader& uh,
                       uint64_t station)
{
    const ProductFamily family = productFamily(h.product_id);
    if (family == ProductFamily::other) {
        ++counters_.unsupported_products;
        return;
    }
    FisbProduct out;
    out.id = h.product_id;
    out.received = ref;
    out.product_time = apduTime(h, ref);
    out.station = station;
    out.station_lat = uh.lat_deg;
    out.station_lon = uh.lon_deg;

    if (family == ProductFamily::global_block) {
        const uint64_t rejected_before = block_stats_.rejected;
        auto d = parseGlobalBlockProduct(h, p, n, experimental_, &block_stats_);
        counters_.blocks_rejected += block_stats_.rejected - rejected_before;
        if (!d)
            return;
        const auto before = d->blocks.size();
        d->blocks.erase(std::remove_if(d->blocks.begin(), d->blocks.end(),
                                       [](const RadarBlock& b) {
                                           return !pointInRange({b.lat0, b.lon0});
                                       }),
                        d->blocks.end());
        counters_.blocks_rejected += before - d->blocks.size();
        if (d->blocks.empty() && d->empty_blocks.empty())
            return;
        out.body = std::move(*d);
        emit(std::move(out));
        return;
    }

    // Text and TWGO may be segmented (8 and 14 in practice).
    std::vector<uint8_t> joined;
    ApduHeader hdr = h;
    if (h.segmented) {
        auto done = segments_.add(h, p, n, ref, station);
        counters_.segments_open = segments_.open();
        if (!done)
            return;
        hdr = done->hdr;
        joined = std::move(done->payload);
        p = joined.data();
        n = joined.size();
        out.product_time = apduTime(hdr, ref);
    }

    if (family == ProductFamily::generic_text) {
        auto records = parseTextProduct(hdr, p, n, ref);
        if (records.empty()) {
            ++counters_.empty_text;
            return;
        }
        out.body = std::move(records);
        emit(std::move(out));
        return;
    }

    auto t = parseTwgo(hdr, p, n, ref);
    if (!t) {
        ++counters_.bad_products;
        return;
    }
    const auto before = t->shapes.size();
    t->shapes.erase(std::remove_if(t->shapes.begin(), t->shapes.end(),
                                   [](const Shape& s) { return !shapeInRange(s); }),
                    t->shapes.end());
    counters_.rejected_shapes += before - t->shapes.size();
    if (t->has_graphics && before > 0 && t->shapes.empty()) {
        // Nothing drawable is left: do not pair it as a graphics half.
        t->has_graphics = false;
        ++counters_.graphics_rejected;
        if (!t->text && !t->cancel)
            return;
    }
    out.body = std::move(*t);
    emit(std::move(out));
}

void FisbDecoder::emit(FisbProduct&& product)
{
    ++counters_.products;
    if (on_product_)
        on_product_(product);
}

void FisbDecoder::sweep(TimePoint now)
{
    counters_.segments_timed_out += segments_.sweep(now, std::chrono::minutes(60));
    counters_.segments_open = segments_.open();
}

}  // namespace fisb
}  // namespace skycued
