#include "fisb/fisb_store.hpp"

#include "fisb/fisb_time.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <iterator>
#include <ratio>
#include <utility>

namespace skycued {
namespace fisb {

namespace {

bool isNexrad(uint16_t pid) { return pid == 63 || pid == 64; }

// §10.3 with the reference §7 table: NEXRAD and lightning 75 min, the other
// rasters (icing, turbulence, cloud tops) 105 min.
std::chrono::minutes rasterLifetime(uint16_t pid)
{
    return isNexrad(pid) || pid == 103 ? expiry::kNexrad : expiry::kOtherRaster;
}

uint32_t fnv1a(const std::string& s)
{
    uint32_t h = 2166136261u;
    for (unsigned char c : s) {
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

const char* kindName(TextRecord::Kind k)
{
    switch (k) {
    case TextRecord::metar: return "metar";
    case TextRecord::taf: return "taf";
    case TextRecord::pirep: return "pirep";
    case TextRecord::winds: return "winds";
    case TextRecord::other: break;
    }
    return "other";
}

bool sameShapes(const std::vector<Shape>& a, const std::vector<Shape>& b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const Shape& x = a[i];
        const Shape& y = b[i];
        if (x.geom != y.geom || x.points != y.points || x.radius_nm != y.radius_nm || x.alt_low_ft != y.alt_low_ft ||
            x.alt_high_ft != y.alt_high_ft || x.agl != y.agl)
            return false;
    }
    return true;
}

bool isDigit(char c) { return c >= '0' && c <= '9'; }

// The first "ddhh/ddhh" group (digits not touching other digits): the end
// day and hour.
std::optional<std::pair<unsigned, unsigned>> tafValidityEnd(const std::string& t)
{
    for (std::size_t slash = t.find('/'); slash != std::string::npos; slash = t.find('/', slash + 1)) {
        if (slash < 4 || slash + 5 > t.size())
            continue;
        bool ok = true;
        for (std::size_t i = slash - 4; i < slash + 5; ++i)
            if (i != slash && !isDigit(t[i]))
                ok = false;
        if (!ok || (slash > 4 && isDigit(t[slash - 5])) || (slash + 5 < t.size() && isDigit(t[slash + 5])))
            continue;
        const unsigned day = static_cast<unsigned>((t[slash + 1] - '0') * 10 + (t[slash + 2] - '0'));
        const unsigned hour = static_cast<unsigned>((t[slash + 3] - '0') * 10 + (t[slash + 4] - '0'));
        return std::make_pair(day, hour);
    }
    return std::nullopt;
}

int64_t epochSeconds(TimePoint t)
{
    return std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count();
}

bool isNotam(uint16_t pid) { return pid == 8 || pid == 16 || pid == 17; }

// The TWGO report year against the data clock's (reference §6.5, §7): the
// clock's year, or the year before in January, the year after in December.
// The key holds the year's last two digits, or for some NOTAMs its last one.
bool plausibleYear(const TwgoKey& k, TimePoint now)
{
    using Days = std::chrono::duration<int64_t, std::ratio<86400>>;
    const detail::Civil c = detail::civilFromDays(std::chrono::floor<Days>(now.time_since_epoch()).count());
    for (int64_t dy = -1; dy <= 1; ++dy) {
        if ((dy == -1 && c.m != 1) || (dy == 1 && c.m != 12))
            continue;
        const int64_t y = c.y + dy;
        if (k.year == y % 100 || (isNotam(k.pid) && k.year < 10 && k.year == y % 10))
            return true;
    }
    return false;
}

// §10.3 (amended at the final review): these products are re-sent every
// cycle, so a report not heard for 70 min is gone whatever end it carries.
bool cappedByReception(uint16_t pid) { return pid != 13; }

}  // namespace

std::string Mosaic::key() const
{
    std::string k = std::to_string(product);
    if (altitude_code)
        k += "-" + std::to_string(*altitude_code);
    return k;
}

std::string textKey(const TextRecord& r)
{
    const std::string kind = kindName(r.kind);
    switch (r.kind) {
    case TextRecord::metar:
    case TextRecord::taf: return kind + ":" + r.station;
    case TextRecord::winds: return kind + ":" + r.station + ":" + std::to_string(r.period_h ? *r.period_h : 0);
    case TextRecord::pirep:
    case TextRecord::other: break;
    }
    char hash[16];
    std::snprintf(hash, sizeof hash, "%08x", fnv1a(r.text));
    return kind + ":" + r.station + ":" + std::to_string(epochSeconds(r.issued)) + ":" + hash;
}

TimePoint textExpiry(const TextRecord& r)
{
    switch (r.kind) {
    case TextRecord::metar: return r.issued + expiry::kMetar;
    case TextRecord::taf:
        if (const auto end = tafValidityEnd(r.text))
            if (const auto t = resolveUtc(r.issued, std::nullopt, end->first, end->second, 0))
                if (*t > r.issued)
                    return *t;
        return r.issued + expiry::kTafFallback;
    case TextRecord::winds: return r.valid.value_or(r.issued) + expiry::kWindsFallback;
    case TextRecord::pirep:
    case TextRecord::other: break;
    }
    return r.issued + expiry::kPirep;  // PIREP; unclassified text likewise
}

// ---- apply ----

std::vector<FisbEvent> FisbStore::apply(const FisbProduct& p, TimePoint now)
{
    std::vector<FisbEvent> out;
    ++stats_.applied;
    StationHeard& st = stations_[p.station];
    st.key = p.station;
    st.lat = p.station_lat;
    st.lon = p.station_lon;
    st.last_heard = now;
    ++st.products;

    if (const auto* text = std::get_if<std::vector<TextRecord>>(&p.body))
        applyText(p, *text, now, out);
    else if (const auto* twgo = std::get_if<TwgoParsed>(&p.body))
        applyTwgo(p, *twgo, now, out);
    else if (const auto* blocks = std::get_if<DecodedBlocks>(&p.body))
        applyBlocks(p, *blocks, now, out);
    return out;
}

void FisbStore::applyText(const FisbProduct& p, const std::vector<TextRecord>& records, TimePoint now,
                          std::vector<FisbEvent>& out)
{
    for (TextRecord r : records) {
        r.expires = textExpiry(r);
        if (r.expires <= now) {
            ++stats_.stale;
            continue;
        }
        const std::string key = textKey(r);
        auto it = text_.find(key);
        if (it != text_.end()) {
            // METAR/TAF/winds: only a strictly newer issue replaces. PIREP and
            // other: the key includes issued and text, so this is a repeat.
            if (r.kind == TextRecord::pirep || r.kind == TextRecord::other || r.issued <= it->second.issued)
                continue;
            it->second = r;
        } else {
            text_.emplace(key, r);
        }
        FisbEvent e;
        e.kind = FisbEvent::Kind::text;
        e.product = p.id;
        e.key = key;
        e.text = r;
        e.expires = r.expires;
        out.push_back(std::move(e));
    }
}

void FisbStore::applyTwgo(const FisbProduct& p, const TwgoParsed& t, TimePoint now, std::vector<FisbEvent>& out)
{
    if (!plausibleYear(t.key, now)) {
        ++stats_.wrong_year;
        return;
    }
    auto it = twgo_.find(t.key);
    if (it == twgo_.end()) {
        // A keep-alive or a cancel for a report not held creates nothing.
        if (t.cancel || (t.text && t.text->empty() && !t.has_graphics))
            return;
        TwgoState s;
        s.rec.key = t.key;
        s.rec.received = now;
        it = twgo_.emplace(t.key, std::move(s)).first;
    }
    TwgoState& s = it->second;
    TwgoRecord& r = s.rec;

    auto event = [&](FisbEvent::Kind kind, const char* reason) {
        FisbEvent e;
        e.kind = kind;
        e.expired_kind = FisbEvent::Kind::twgo;
        e.product = p.id;
        e.key = r.key.str();
        e.twgo = r;
        e.reason = reason;
        e.expires = r.expires;
        out.push_back(std::move(e));
    };

    if (r.cancelled) {
        // Within its 30 min only the cancellation is kept (reference §6.5): a
        // repeated cancel does not extend it, and a keep-alive or a graphics
        // half still on the air does not bring the report back.
        ++stats_.ignored_after_cancel;
        return;
    }
    r.last_received = now;
    if (t.cancel) {
        r.cancelled = true;
        r.complete = false;
        r.text.reset();  // nothing stale can resurface
        r.shapes.clear();
        r.has_graphics = false;
        r.start.reset();
        r.graphics_end.reset();
        r.text_end.reset();
        r.pending = false;
        r.expires = now + expiry::kCancelled;
        ++stats_.cancelled;
        s.published = true;  // listed (flagged cancelled) until it expires
        event(FisbEvent::Kind::expired, "cancelled");
        return;
    }

    bool changed = false;
    if (t.text && !t.text->empty() && (!r.text || *r.text != *t.text)) {
        if (r.text) {
            // The text changed: the old graphics no longer belong to it.
            r.shapes.clear();
            r.has_graphics = false;
            r.start.reset();
            r.graphics_end.reset();
        }
        r.text = t.text;
        r.text_end.reset();
        if (p.id == 8 || p.id == 16 || p.id == 17)
            r.text_end = notamTextEnd(*r.text);
        changed = true;
    }
    if (t.has_graphics &&
        (!r.has_graphics || !sameShapes(r.shapes, t.shapes) || r.start != t.start || r.graphics_end != t.end)) {
        r.shapes = t.shapes;
        r.has_graphics = true;
        r.start = t.start;
        r.graphics_end = t.end;
        changed = true;
    }
    if (!t.location.empty())
        r.location = t.location;
    if (!t.label.empty())
        r.label = t.label;

    const bool was_complete = r.complete;
    r.complete = p.id == 14 ? r.has_graphics : (r.text && !r.text->empty());
    if (r.complete) {
        // The latest end time the record carries (fisb-decode uses the NOTAM
        // text's; a graphics end can be earlier); all but SUA also end 70 min
        // after the last reception (a keep-alive renews).
        std::optional<TimePoint> end = r.text_end;
        if (r.graphics_end && (!end || *r.graphics_end > *end))
            end = r.graphics_end;
        const TimePoint live = r.last_received + expiry::kTwgo;
        r.expires = !end ? live : cappedByReception(p.id) ? std::min(*end, live) : *end;
    } else {
        r.expires = now + expiry::kTwgoUnmatched;
        if (r.graphics_end && *r.graphics_end < r.expires)
            r.expires = *r.graphics_end;
    }
    const bool was_pending = r.pending;
    r.pending = r.start && *r.start > now;
    if (r.expires <= now) {
        ++stats_.stale;
        if (s.published) {
            FisbEvent e;
            e.kind = FisbEvent::Kind::expired;
            e.expired_kind = FisbEvent::Kind::twgo;
            e.product = p.id;
            e.key = r.key.str();
            e.reason = "expired";
            e.expires = r.expires;
            out.push_back(std::move(e));
        }
        twgo_.erase(it);
        return;
    }
    if (r.complete && (changed || !was_complete || !s.published || r.pending != was_pending)) {
        s.published = true;
        event(FisbEvent::Kind::twgo, "");
    }
}

void FisbStore::applyBlocks(const FisbProduct& p, const DecodedBlocks& b, TimePoint now, std::vector<FisbEvent>& out)
{
    if (!p.product_time) {
        ++stats_.no_product_time;
        return;
    }
    const TimePoint pt = *p.product_time;
    Mosaic head;
    head.product = p.id;
    head.altitude_code = b.altitude_code;
    const std::string key = head.key();
    auto it = mosaics_.find(key);
    if (it == mosaics_.end()) {
        MosaicState m;
        m.head = head;
        m.head.product_time = pt;
        it = mosaics_.emplace(key, std::move(m)).first;
    }
    MosaicState& m = it->second;
    if (pt > m.head.product_time) {
        m.head.product_time = pt;
        // Blocks of an old cycle go when the mosaic moves on.
        for (auto bi = m.blocks.begin(); bi != m.blocks.end();) {
            if (bi->second.time + kMosaicMergeWindow < pt) {
                bi = m.blocks.erase(bi);
                ++stats_.blocks_pruned;
            } else {
                ++bi;
            }
        }
    }
    const std::size_t n_blocks = b.blocks.size() + b.empty_blocks.size();
    if (pt + kMosaicMergeWindow < m.head.product_time) {
        stats_.blocks_too_old += n_blocks;
        return;
    }
    m.head.expires = m.head.product_time + rasterLifetime(p.id);
    if (m.head.expires <= now) {
        ++stats_.stale;
        if (m.blocks.empty())
            mosaics_.erase(it);
        return;
    }

    bool merged = false;
    for (const RadarBlock& rb : b.blocks)
        merged |= merge(m, rb, b.scale, pt);
    const uint8_t empty_level = p.id == 64 ? 1 : 0;  // §10.6: empty CONUS blocks are level 1
    for (uint32_t bn : b.empty_blocks) {
        const auto g = blockGeometry(bn, b.scale, true);  // the decoder already filtered latitude
        if (!g)
            continue;
        RadarBlock rb;
        rb.block = bn;
        rb.lat0 = g->lat0;
        rb.lon0 = g->lon0;
        rb.dlat = g->dlat;
        rb.dlon = g->dlon;
        rb.bins.fill(empty_level);
        merged |= merge(m, rb, b.scale, pt);
    }
    if (!merged)
        return;
    m.head.updated = now;
    m.head.scale = b.scale;
    m.last_block = now;
    if (!m.dirty) {
        m.dirty = true;
        m.dirty_since = now;
    }
    if (now - m.dirty_since >= kMosaicMaxInterval)
        publish(m, now, out);
}

bool FisbStore::merge(MosaicState& m, const RadarBlock& block, uint8_t scale, TimePoint time)
{
    const uint32_t k = (static_cast<uint32_t>(scale) << 24) | block.block;
    auto it = m.blocks.find(k);
    if (it != m.blocks.end()) {
        if (it->second.time > time) {
            ++stats_.blocks_superseded;
            return false;
        }
        if (it->second.time == time && it->second.block.bins == block.bins) {
            ++stats_.blocks_unchanged;
            return false;
        }
        it->second = HeldBlock{block, time, ++seq_};
        ++stats_.blocks_merged;
        return true;
    }
    if (m.blocks.size() >= kMosaicMaxBlocks) {
        auto oldest = m.blocks.begin();
        for (auto bi = m.blocks.begin(); bi != m.blocks.end(); ++bi)
            if (bi->second.time < oldest->second.time ||
                (bi->second.time == oldest->second.time && bi->second.seq < oldest->second.seq))
                oldest = bi;
        m.blocks.erase(oldest);
        ++m.head.evicted;
        ++stats_.blocks_evicted;
    }
    m.blocks.emplace(k, HeldBlock{block, time, ++seq_});
    ++stats_.blocks_merged;
    return true;
}

Mosaic FisbStore::view(const MosaicState& m) const
{
    Mosaic v = m.head;
    v.blocks.reserve(m.blocks.size());
    for (const auto& b : m.blocks)
        v.blocks.push_back(b.second.block);
    return v;
}

void FisbStore::publish(MosaicState& m, TimePoint now, std::vector<FisbEvent>& out)
{
    m.dirty = false;
    m.head.published = now;
    ++stats_.mosaics_published;
    FisbEvent e;
    e.kind = isNexrad(m.head.product) ? FisbEvent::Kind::radar : FisbEvent::Kind::raster;
    e.product = m.head.product;
    e.key = m.head.key();
    e.mosaic = view(m);
    e.expires = m.head.expires;
    out.push_back(std::move(e));
}

// ---- sweep ----

std::vector<FisbEvent> FisbStore::sweep(TimePoint now)
{
    std::vector<FisbEvent> out;
    auto expired = [&](FisbEvent::Kind of, uint16_t product, const std::string& key, TimePoint due,
                       const TextRecord* text = nullptr) {
        ++stats_.expired;
        FisbEvent e;
        e.kind = FisbEvent::Kind::expired;
        e.expired_kind = of;
        if (text)
            e.text = *text;
        e.product = product;
        e.key = key;
        e.reason = "expired";
        e.expires = due;
        out.push_back(std::move(e));
    };

    for (auto it = mosaics_.begin(); it != mosaics_.end();) {
        MosaicState& m = it->second;
        if (m.head.expires <= now) {
            if (m.head.published)  // never published: nobody was told of it
                expired(isNexrad(m.head.product) ? FisbEvent::Kind::radar : FisbEvent::Kind::raster,
                        m.head.product, it->first, m.head.expires);
            it = mosaics_.erase(it);
            continue;
        }
        if (m.dirty && (now - m.last_block >= kMosaicQuiet || now - m.dirty_since >= kMosaicMaxInterval))
            publish(m, now, out);
        ++it;
    }
    for (auto it = text_.begin(); it != text_.end();) {
        if (it->second.expires <= now) {
            expired(FisbEvent::Kind::text, 413, it->first, it->second.expires, &it->second);
            it = text_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = twgo_.begin(); it != twgo_.end();) {
        TwgoState& s = it->second;
        if (s.rec.expires <= now) {
            if (s.published)
                expired(FisbEvent::Kind::twgo, s.rec.key.pid, s.rec.key.str(), s.rec.expires);
            it = twgo_.erase(it);
            continue;
        }
        if (s.rec.pending && s.rec.start && *s.rec.start <= now) {
            // In force now: published again, no longer pending.
            s.rec.pending = false;
            if (s.published && s.rec.complete && !s.rec.cancelled) {
                FisbEvent e;
                e.kind = FisbEvent::Kind::twgo;
                e.expired_kind = FisbEvent::Kind::twgo;
                e.product = s.rec.key.pid;
                e.key = s.rec.key.str();
                e.twgo = s.rec;
                e.expires = s.rec.expires;
                out.push_back(std::move(e));
            }
        }
        ++it;
    }
    for (auto it = stations_.begin(); it != stations_.end();)
        it = now - it->second.last_heard > kStationForget ? stations_.erase(it) : std::next(it);
    return out;
}

// ---- views ----

FisbSnapshot FisbStore::snapshot() const
{
    FisbSnapshot s;
    for (const auto& m : mosaics_) {
        (isNexrad(m.second.head.product) ? s.radar : s.rasters).push_back(view(m.second));
        blocks_copied_ += m.second.blocks.size();
    }
    for (const auto& t : text_) {
        switch (t.second.kind) {
        case TextRecord::metar: s.metar.push_back(t.second); break;
        case TextRecord::taf: s.taf.push_back(t.second); break;
        case TextRecord::pirep: s.pirep.push_back(t.second); break;
        case TextRecord::winds: s.winds.push_back(t.second); break;
        case TextRecord::other: s.other.push_back(t.second); break;
        }
    }
    for (const auto& t : twgo_)
        if (t.second.published)
            s.twgo.push_back(t.second.rec);
    for (const auto& st : stations_)
        s.stations.push_back(st.second);
    s.stats = stats();
    return s;
}

std::optional<Mosaic> FisbStore::mosaic(uint16_t product, std::optional<uint8_t> altitude_code) const
{
    const MosaicState* best = nullptr;
    for (const auto& m : mosaics_) {
        const Mosaic& h = m.second.head;
        if (h.product != product || (altitude_code && h.altitude_code != altitude_code))
            continue;
        if (!best || (h.altitude_code && best->head.altitude_code && *h.altitude_code < *best->head.altitude_code))
            best = &m.second;
    }
    if (!best)
        return std::nullopt;
    blocks_copied_ += best->blocks.size();
    return view(*best);
}

FisbStoreStats FisbStore::stats() const
{
    FisbStoreStats s = stats_;
    s.twgo_pending = 0;
    for (const auto& t : twgo_)
        s.twgo_pending += t.second.published ? 0 : 1;
    return s;
}

std::size_t FisbStore::mosaicBlocks() const
{
    std::size_t n = 0;
    for (const auto& m : mosaics_)
        n += m.second.blocks.size();
    return n;
}

FisbSummary FisbStore::summary(TimePoint now) const
{
    FisbSummary s;
    s.enabled = true;
    s.text_records = text_.size();
    for (const auto& t : twgo_)
        s.twgo_records += t.second.published ? 1 : 0;
    s.mosaics = mosaics_.size();
    s.mosaic_blocks = mosaicBlocks();
    s.stations = stations_.size();
    for (const auto& t : text_) {
        switch (t.second.kind) {
        case TextRecord::metar: ++s.metar; break;
        case TextRecord::taf: ++s.taf; break;
        case TextRecord::pirep: ++s.pirep; break;
        case TextRecord::winds: ++s.winds; break;
        case TextRecord::other: ++s.other; break;
        }
    }
    for (const auto& t : twgo_)
        s.twgo_cancelled += t.second.published && t.second.rec.cancelled ? 1 : 0;
    for (const auto& st : stations_)
        s.heard.push_back(st.second);
    for (const auto& m : mosaics_) {
        const Mosaic& h = m.second.head;
        MosaicHead mh;
        mh.key = m.first;
        mh.product = h.product;
        mh.altitude_code = h.altitude_code;
        mh.scale = h.scale;
        mh.product_time = h.product_time;
        mh.updated = h.updated;
        mh.expires = h.expires;
        mh.published = h.published;
        mh.block_count = m.second.blocks.size();
        mh.evicted = h.evicted;
        (isNexrad(h.product) ? s.radar : s.rasters).push_back(std::move(mh));
    }
    for (const auto& m : mosaics_) {
        if (!isNexrad(m.second.head.product))
            continue;
        const double age = std::chrono::duration<double>(now - m.second.head.product_time).count();
        if (!s.radar_age_s || age < *s.radar_age_s)
            s.radar_age_s = age;
    }
    return s;
}

}  // namespace fisb
}  // namespace skycued
