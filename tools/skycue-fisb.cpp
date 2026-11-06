// skycue-fisb: decodes FIS-B ground uplinks to product JSONL or a summary,
// for testing the decoder and for the operator's curiosity.
//
//   skycue-fisb [--ref-date YYYY-MM-DD] [--json | --summary] [--experimental] FILE...
//
// FILE, plain or gzip-compressed (zlib reads both), holds any mix of, line by
// line:
//   - dump978 lines: "+<hex>;" uplinks, optionally with ";rs=..;rssi=..;t=<unix s.ms>;"
//     ("-<hex>;" downlinks and "#" comments are counted and skipped);
//   - skycue-rawrec captures (uat-*.txt): "<epoch_ms> <dump978 line>";
//   - received-stream JSONL: "uat_uplink" records (other records are skipped).
//
// Time. Each uplink is decoded against a reference time (partial APDU times
// resolve to the candidate nearest it): the rawrec prefix, else t=, else the
// record's "time". Lines with none of these (the 2015 dump978 sample) are
// dated by --ref-date (default: today, with a warning): a first pass decodes
// them against 12:00 UTC that day, and every undated line is then decoded
// against one estimated reception time, the newest global-block (radar)
// product time found, else the median product-413 time, else that noon.
// (TWGO times are report times, hours from reception, so they are not used.)
// The store's clock is the newest reference time so far. Several files share
// one decoder and store, in the order given.
//
// --json (one JSON object per line): every FisbStore event, in the order the
// store emits it (fisb_text, fisb_twgo, fisb_radar / fisb_raster with every
// block, fisb_expired), through the fisb_json writers, so field names match
// the recording stream and the web API. The store is swept after each uplink
// and once more 10 s after the last, which publishes mosaics still pending.
// Every time in the output derives from the input: two runs are identical.
//
// --summary (the default): inputs, products by ID, text and TWGO records,
// global blocks, mosaics, ground stations, store events and every counter.
//
// Exit status: 0 success, 1 a file that cannot be read, 2 usage error,
// 3 no line of the input was an uplink.

#include "fisb/fisb_decoder.hpp"
#include "fisb/fisb_json.hpp"
#include "fisb/fisb_store.hpp"
#include "fisb/uplink_frame.hpp"
#include "gz_line_reader.hpp"
#include "recording_reader.hpp"
#include "time_util.hpp"

#include <boost/program_options.hpp>

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace po = boost::program_options;
using namespace skycued;
using namespace skycued::fisb;

namespace {

// A plain or gzip file, line by line; a truncated gzip stream throws.
using Reader = GzLineReader;

std::string format(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// NOLINTNEXTLINE(cert-dcl50-cpp): a printf-style wrapper: the format attribute has the compiler check every call
std::string format(const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return n < 0 ? std::string() : std::string(buf, std::min<std::size_t>(static_cast<std::size_t>(n), sizeof buf - 1));
}

unsigned long long ull(uint64_t v) { return static_cast<unsigned long long>(v); }

// ---- line classification ----

enum class InputKind { dump978, dump978_t, rawrec, received };
constexpr InputKind kInputKinds[] = {InputKind::dump978, InputKind::dump978_t, InputKind::rawrec, InputKind::received};

const char* inputKindName(InputKind k)
{
    switch (k) {
    case InputKind::dump978: return "dump978";
    case InputKind::dump978_t: return "dump978-t";
    case InputKind::rawrec: return "rawrec";
    case InputKind::received: return "received";
    }
    return "";
}

struct Line {
    enum What { uplink, downlink, other } what = other;
    InputKind kind = InputKind::dump978;
    std::optional<TimePoint> time;  // the line's own timestamp
    std::string json_hex;           // received JSONL: the record's uplink_hex (else the line is fed as is)
};

bool isDigit(char c) { return c >= '0' && c <= '9'; }

// "<seconds>[.<fraction>]" at s[pos], ending at ';', whitespace or the end.
std::optional<TimePoint> parseEpochSeconds(const std::string& s, std::size_t pos)
{
    std::size_t i = pos;
    int64_t sec = 0;
    while (i < s.size() && isDigit(s[i]) && i - pos < 12)
        sec = sec * 10 + (s[i++] - '0');
    if (i == pos)
        return std::nullopt;
    int64_t ms = 0;
    if (i < s.size() && s[i] == '.') {
        ++i;
        int n = 0;
        for (; i < s.size() && isDigit(s[i]); ++i)
            if (n < 3) {
                ms = ms * 10 + (s[i] - '0');
                ++n;
            }
        for (; n < 3; ++n)
            ms *= 10;
    }
    if (i < s.size() && s[i] != ';' && !std::isspace(static_cast<unsigned char>(s[i])))
        return std::nullopt;
    return TimePoint(std::chrono::milliseconds(sec * 1000 + ms));
}

Line classify(const std::string& raw)
{
    Line l;
    std::size_t i = raw.find_first_not_of(" \t\r");
    if (i == std::string::npos)
        return l;
    if (raw[i] == '{') {
        RecordingRecord rec;
        if (parseRecordingLine(raw, rec) == LineKind::received && rec.kind == RecordKind::uat_uplink) {
            l.what = Line::uplink;
            l.kind = InputKind::received;
            l.time = rec.time;
            l.json_hex = rec.uplink_hex;
        }
        return l;
    }
    std::size_t j = i;
    while (j < raw.size() && isDigit(raw[j]))
        ++j;
    bool rawrec = false;
    if (j > i && j - i <= 15 && j < raw.size() && raw[j] == ' ') {
        int64_t ms = 0;
        for (std::size_t k = i; k < j; ++k)
            ms = ms * 10 + (raw[k] - '0');
        l.time = TimePoint(std::chrono::milliseconds(ms));
        rawrec = true;
        i = raw.find_first_not_of(' ', j);
        if (i == std::string::npos)
            return Line{};
    }
    if (raw[i] == '+')
        l.what = Line::uplink;
    else if (raw[i] == '-')
        l.what = Line::downlink;
    else
        return Line{};
    if (rawrec) {
        l.kind = InputKind::rawrec;
    } else {
        const std::size_t t = raw.find(";t=", i);
        if (t != std::string::npos)
            l.time = parseEpochSeconds(raw, t + 3);
        l.kind = l.time ? InputKind::dump978_t : InputKind::dump978;
    }
    return l;
}

// "YYYY-MM-DD" -> 12:00 UTC that day.
std::optional<TimePoint> parseRefDate(const std::string& s)
{
    if (s.size() != 10 || s[4] != '-' || s[7] != '-')
        return std::nullopt;
    for (std::size_t i : {0, 1, 2, 3, 5, 6, 8, 9})
        if (!isDigit(s[i]))
            return std::nullopt;
    return parseIso8601Ms(s + "T12:00:00Z");
}

// ---- what the summary gathers ----

struct FileInfo {
    std::string name;
    std::set<InputKind> kinds;
    uint64_t lines = 0, uplinks = 0, downlinks = 0, other = 0;
};

struct TextKindStats {
    uint64_t decoded = 0;
    std::set<std::string> stations;
};

struct TwgoHalfStats {
    uint64_t text = 0, keepalive = 0, graphics = 0, cancels = 0;
};

struct BlockProductStats {
    uint64_t messages = 0, rle_blocks = 0, empty_lists = 0, empty_blocks = 0;
};

struct GroundStation {
    double lat = 0, lon = 0;
    std::set<unsigned> site_ids;
    uint64_t uplinks = 0, app_data = 0;
};

// The reference time of the undated uplinks (see the file comment).
struct UndatedTime {
    TimePoint time{};
    const char* how = "";
};

UndatedTime estimateUndated(const std::vector<std::string>& files, TimePoint noon, bool experimental)
{
    FisbDecoder d(experimental);
    std::optional<TimePoint> newest_block;
    std::vector<TimePoint> text_times;
    d.onProduct([&](const FisbProduct& p) {
        if (!p.product_time)
            return;
        const ProductFamily f = productFamily(p.id);
        if (f == ProductFamily::global_block && (!newest_block || *p.product_time > *newest_block))
            newest_block = p.product_time;
        else if (f == ProductFamily::generic_text)
            text_times.push_back(*p.product_time);
    });
    for (const auto& path : files) {
        Reader reader(path);
        std::string raw;
        while (reader.getline(raw)) {
            const Line l = classify(raw);
            if (l.what == Line::uplink && !l.time)
                d.feed(noon, raw);
        }
    }
    if (newest_block)
        return {*newest_block, "the newest global-block product time"};
    if (!text_times.empty()) {
        auto mid = text_times.begin() + static_cast<std::ptrdiff_t>(text_times.size() / 2);
        std::nth_element(text_times.begin(), mid, text_times.end());
        return {*mid, "the median product-413 time"};
    }
    return {noon, "12:00 UTC"};
}

struct Gathered {
    std::vector<FileInfo> files;
    UndatedTime undated;
    uint64_t uplinks_timed = 0, uplinks_undated = 0;
    std::optional<TimePoint> first_time, last_time;  // of the timed uplinks
    std::map<uint16_t, uint64_t> products;
    std::map<int, TextKindStats> text;  // by TextRecord::Kind
    std::map<uint16_t, TwgoHalfStats> twgo;
    std::map<uint16_t, BlockProductStats> blocks;
    std::map<uint64_t, GroundStation> stations;
    std::map<FisbEvent::Kind, uint64_t> events;
    uint64_t events_cancelled = 0;
    std::map<std::string, uint64_t> publishes;  // mosaic key -> times published
};

// ---- the summary ----

std::string pad(const std::string& s, std::size_t w) { return s.size() >= w ? s : s + std::string(w - s.size(), ' '); }

std::string formatSummary(const Gathered& g, const FisbDecoder& decoder, const FisbStore& store,
                          const std::optional<std::string>& ref_date, bool experimental)
{
    std::string o;
    const FisbSnapshot snap = store.snapshot();
    const FisbDecoderCounters& c = decoder.counters();

    o += "== Inputs\n";
    std::size_t w = 0;
    for (const auto& f : g.files)
        w = std::max(w, f.name.size());
    for (const auto& f : g.files) {
        std::string kinds;
        for (InputKind k : kInputKinds)
            if (f.kinds.count(k))
                kinds += (kinds.empty() ? "" : "+") + std::string(inputKindName(k));
        if (kinds.empty())
            kinds = "none";
        o += format("  %s  %-9s  %llu lines: %llu uplinks, %llu downlinks, %llu other\n", pad(f.name, w).c_str(),
                    kinds.c_str(), ull(f.lines), ull(f.uplinks), ull(f.downlinks), ull(f.other));
    }

    o += "\n== Uplinks\n";
    o += format("  uplinks            %llu (%llu timestamped, %llu undated)\n", ull(c.uplinks), ull(g.uplinks_timed),
                ull(g.uplinks_undated));
    if (g.uplinks_undated)
        o += format("  undated taken as   %s (%s on %s)\n", formatIso8601Ms(g.undated.time).c_str(), g.undated.how,
                    ref_date ? ("--ref-date " + *ref_date).c_str() : "today, no --ref-date");
    if (g.first_time)
        o += format("  time span          %s .. %s\n", formatIso8601Ms(*g.first_time).c_str(),
                    formatIso8601Ms(*g.last_time).c_str());
    o += format("  APDUs              %llu\n", ull(c.apdus));
    o += format("  products decoded   %llu\n", ull(c.products));
    o += format("  experimental       %s\n", experimental ? "on" : "off");

    o += "\n== Products by ID (decoded products; a segmented set counts once)\n";
    for (const auto& p : g.products)
        o += format("%5u  %-16s %8llu\n", static_cast<unsigned>(p.first), productName(p.first), ull(p.second));

    o += "\n== Text records (product 413)\n";
    std::map<int, std::size_t> stored = {{TextRecord::metar, snap.metar.size()},
                                         {TextRecord::taf, snap.taf.size()},
                                         {TextRecord::pirep, snap.pirep.size()},
                                         {TextRecord::winds, snap.winds.size()},
                                         {TextRecord::other, snap.other.size()}};
    for (int k : {TextRecord::metar, TextRecord::taf, TextRecord::pirep, TextRecord::winds, TextRecord::other}) {
        const auto it = g.text.find(k);
        const uint64_t decoded = it == g.text.end() ? 0 : it->second.decoded;
        const std::size_t stations = it == g.text.end() ? 0 : it->second.stations.size();
        if (decoded == 0 && stored[k] == 0)
            continue;
        o += format("  %-6s decoded %6llu  stored %6zu  stations %6zu\n",
                    textKindName(static_cast<TextRecord::Kind>(k)), ull(decoded), stored[k], stations);
    }

    o += "\n== TWGO records (decoded halves; records stored at the end)\n";
    std::set<uint16_t> twgo_pids;
    for (const auto& t : g.twgo)
        twgo_pids.insert(t.first);
    for (const auto& r : snap.twgo)
        twgo_pids.insert(r.key.pid);
    for (uint16_t pid : twgo_pids) {
        const auto it = g.twgo.find(pid);
        const TwgoHalfStats h = it == g.twgo.end() ? TwgoHalfStats{} : it->second;
        uint64_t records = 0, cancelled = 0, graphics = 0;
        std::map<std::string, uint64_t> geoms;
        for (const auto& r : snap.twgo) {
            if (r.key.pid != pid)
                continue;
            ++records;
            cancelled += r.cancelled;
            graphics += r.has_graphics;
            for (const auto& s : r.shapes)
                ++geoms[shapeGeomName(s.geom)];
        }
        std::string shapes;
        for (const auto& gm : geoms)
            shapes += format("%s%s %llu", shapes.empty() ? "" : ", ", gm.first.c_str(), ull(gm.second));
        o += format("%5u  %s\n", static_cast<unsigned>(pid), productName(pid));
        o += format("         decoded  %llu text, %llu keep-alive, %llu graphics, %llu cancel\n", ull(h.text),
                    ull(h.keepalive), ull(h.graphics), ull(h.cancels));
        o += format("         stored   %llu records (%llu cancelled, %llu with graphics)\n", ull(records),
                    ull(cancelled), ull(graphics));
        o += format("         shapes   %s\n", shapes.empty() ? "none" : shapes.c_str());
    }

    o += "\n== Global blocks (decoded messages)\n";
    for (const auto& b : g.blocks)
        o += format("%5u  %s: %llu messages, %llu non-empty (RLE) blocks, %llu empty-block lists with %llu empty blocks\n",
                    static_cast<unsigned>(b.first), productName(b.first), ull(b.second.messages),
                    ull(b.second.rle_blocks), ull(b.second.empty_lists), ull(b.second.empty_blocks));

    o += "\n== Mosaics (held at the end; empty: every bin at the no-data level, 1 for CONUS)\n";
    std::vector<const Mosaic*> mosaics;
    mosaics.reserve(snap.radar.size() + snap.rasters.size());
    for (const auto& m : snap.radar)
        mosaics.push_back(&m);
    for (const auto& m : snap.rasters)
        mosaics.push_back(&m);
    for (const Mosaic* m : mosaics) {
        const uint8_t none = m->product == 64 ? 1 : 0;
        std::size_t empty = 0;
        double s = 90, n = -90, west = 180, east = -180;
        for (const auto& b : m->blocks) {
            empty += std::all_of(b.bins.begin(), b.bins.end(), [none](uint8_t v) { return v == none; });
            n = std::max(n, b.lat0);
            s = std::min(s, b.lat0 - 4 * b.dlat);
            west = std::min(west, b.lon0);
            east = std::max(east, b.lon0 + 32 * b.dlon);
        }
        const std::string key = m->key();
        const auto pub = g.publishes.find(key);
        o += format("%5s  %-16s %s  %zu blocks, %zu empty  lat %.4f..%.4f lon %.4f..%.4f  published %llu\n",
                    key.c_str(), productName(m->product), formatIso8601Ms(m->product_time).c_str(), m->blocks.size(),
                    empty, m->blocks.empty() ? 0.0 : s, m->blocks.empty() ? 0.0 : n, m->blocks.empty() ? 0.0 : west,
                    m->blocks.empty() ? 0.0 : east, ull(pub == g.publishes.end() ? 0 : pub->second));
    }

    o += "\n== Ground stations (lat lon from the uplink header)\n";
    for (const auto& st : g.stations) {
        std::string sites;
        for (unsigned id : st.second.site_ids)
            sites += (sites.empty() ? "" : ",") + std::to_string(id);
        o += format("  %9.4f %10.4f  site %s  %llu uplinks (%llu with application data)\n", st.second.lat,
                    st.second.lon, sites.c_str(), ull(st.second.uplinks), ull(st.second.app_data));
    }

    auto ev = [&](FisbEvent::Kind k) {
        const auto it = g.events.find(k);
        return ull(it == g.events.end() ? 0 : it->second);
    };
    o += "\n== Store events\n";
    o += format("  text     %llu\n  twgo     %llu\n  radar    %llu\n  raster   %llu\n  expired  %llu (%llu cancelled)\n",
                ev(FisbEvent::Kind::text), ev(FisbEvent::Kind::twgo), ev(FisbEvent::Kind::radar),
                ev(FisbEvent::Kind::raster), ev(FisbEvent::Kind::expired), ull(g.events_cancelled));

    const GlobalBlockStats& bs = decoder.blockStats();
    const FisbStoreStats ss = store.stats();
    const std::pair<const char*, uint64_t> counters[] = {
        {"bad_hex", c.bad_hex},
        {"bad_frames", c.bad_frames},
        {"not_app_data", c.not_app_data},
        {"bad_apdu", c.bad_apdu},
        {"unsupported_products", c.unsupported_products},
        {"skipped_experimental", c.skipped_experimental},
        {"segments_open", decoder.segmentsOpen()},
        {"segments_timed_out", c.segments_timed_out},
        {"bad_products", c.bad_products},
        {"rejected_shapes", c.rejected_shapes},
        {"graphics_rejected", c.graphics_rejected},
        {"empty_text", c.empty_text},
        {"blocks_rejected", c.blocks_rejected},
        {"blocks.rle_blocks", bs.rle_blocks},
        {"blocks.empty_lists", bs.empty_lists},
        {"blocks.empty_lists_l0", bs.empty_lists_l0},
        {"blocks.empty_blocks", bs.empty_blocks},
        {"blocks.high_lat_skipped", bs.high_lat_skipped},
        {"store.applied", ss.applied},
        {"store.stale", ss.stale},
        {"store.blocks_merged", ss.blocks_merged},
        {"store.blocks_too_old", ss.blocks_too_old},
        {"store.blocks_superseded", ss.blocks_superseded},
        {"store.blocks_unchanged", ss.blocks_unchanged},
        {"store.blocks_evicted", ss.blocks_evicted},
        {"store.blocks_pruned", ss.blocks_pruned},
        {"store.mosaics_published", ss.mosaics_published},
        {"store.expired", ss.expired},
        {"store.cancelled", ss.cancelled},
        {"store.ignored_after_cancel", ss.ignored_after_cancel},
        {"store.no_product_time", ss.no_product_time},
        {"store.twgo_pending", ss.twgo_pending},
        {"store.wrong_year", ss.wrong_year},
        {"store.stale_start", ss.stale_start},
    };
    o += "\n== Decoder counters\n";
    for (const auto& kv : counters)
        o += format("  %-28s %llu\n", kv.first, ull(kv.second));
    return o;
}

int usage(const std::string& message, const po::options_description& opts)
{
    std::cerr << "skycue-fisb: " << message << "\n\nusage: skycue-fisb [options] FILE...\n" << opts;
    return 2;
}

}  // namespace

int main(int argc, char** argv)
{
    std::string ref_date_arg;
    std::vector<std::string> files;

    po::options_description opts("Options");
    opts.add_options()
        ("help,h", "show this help")
        ("ref-date", po::value(&ref_date_arg),
         "YYYY-MM-DD (UTC): the date of uplinks without a timestamp (default: today, with a warning)")
        ("json", "write one JSON object per FIS-B store event")
        ("summary", "write a summary (the default)")
        ("experimental", "decode the unverified layouts too (APDU time options 1/3, blocks at or above 60 N)");
    po::options_description hidden;
    hidden.add_options()("file", po::value(&files));
    po::options_description all;
    all.add(opts).add(hidden);
    po::positional_options_description pos;
    pos.add("file", -1);

    po::variables_map vm;
    try {
        po::store(po::command_line_parser(argc, argv).options(all).positional(pos).run(), vm);
        po::notify(vm);
    } catch (const std::exception& e) {
        return usage(e.what(), opts);
    }
    if (vm.count("help")) {
        std::cout << "usage: skycue-fisb [options] FILE...\n"
                     "Decodes FIS-B uplinks (dump978 lines, rawrec captures, received JSONL; .gz too).\n"
                  << opts
                  << "\nTime: an uplink is decoded against its own timestamp (rawrec prefix, t=, or the JSONL\n"
                     "\"time\"). Uplinks without one are all decoded against a single reception time\n"
                     "estimated on --ref-date: the newest radar/raster (global-block) product time found,\n"
                     "else the median product-413 time, else 12:00 UTC. --summary prints the estimate.\n"
                     "\nExit status: 0 ok, 1 unreadable or truncated file, 2 usage error, 3 no uplink lines.\n";
        return 0;
    }
    if (files.empty())
        return usage("no FILE given", opts);
    if (vm.count("json") && vm.count("summary"))
        return usage("--json and --summary exclude each other", opts);
    const bool json = vm.count("json") > 0;
    const bool experimental = vm.count("experimental") > 0;

    std::optional<std::string> ref_date;
    TimePoint ref_noon;
    if (vm.count("ref-date")) {
        const auto t = parseRefDate(ref_date_arg);
        if (!t)
            return usage("--ref-date must be a date YYYY-MM-DD, got '" + ref_date_arg + "'", opts);
        ref_noon = *t;
        ref_date = ref_date_arg;
    } else {
        const auto day = std::chrono::duration_cast<std::chrono::hours>(
                             std::chrono::system_clock::now().time_since_epoch()).count() / 24;
        ref_noon = TimePoint(std::chrono::hours(day * 24 + 12));
    }
    for (const auto& f : files) {  // fail before any output
        gzFile probe = gzopen(f.c_str(), "rb");
        if (!probe) {
            std::cerr << "skycue-fisb: cannot open " << f << ": " << std::strerror(errno ? errno : ENOENT) << "\n";
            return 1;
        }
        gzclose(probe);
    }

    FisbDecoder decoder(experimental);
    FisbStore store;
    Gathered g;
    std::optional<TimePoint> clock;  // the store's clock
    auto advance = [&](TimePoint t) {
        if (!clock || t > *clock)
            clock = t;
    };
    auto emit = [&](const std::vector<FisbEvent>& events, TimePoint time) {
        for (const auto& e : events) {
            ++g.events[e.kind];
            if (e.kind == FisbEvent::Kind::expired && e.reason == "cancelled")
                ++g.events_cancelled;
            if ((e.kind == FisbEvent::Kind::radar || e.kind == FisbEvent::Kind::raster) && e.mosaic)
                ++g.publishes[e.mosaic->key()];
            if (json)
                std::cout << eventRecordJson(e, time) << '\n';
        }
    };

    TimePoint line_ref{};
    decoder.onProduct([&](const FisbProduct& p) {
        ++g.products[p.id];
        if (const auto* text = std::get_if<std::vector<TextRecord>>(&p.body)) {
            for (const auto& r : *text) {
                TextKindStats& k = g.text[r.kind];
                ++k.decoded;
                if (!r.station.empty())
                    k.stations.insert(r.station);
            }
        } else if (const auto* t = std::get_if<TwgoParsed>(&p.body)) {
            TwgoHalfStats& h = g.twgo[p.id];
            if (t->cancel)
                ++h.cancels;
            if (t->text)
                ++(t->text->empty() ? h.keepalive : h.text);
            if (t->has_graphics)
                ++h.graphics;
        } else if (const auto* b = std::get_if<DecodedBlocks>(&p.body)) {
            BlockProductStats& s = g.blocks[p.id];
            ++s.messages;
            s.rle_blocks += b->blocks.size();
            if (!b->empty_blocks.empty()) {
                ++s.empty_lists;
                s.empty_blocks += b->empty_blocks.size();
            }
        }
        const TimePoint now = clock.value_or(line_ref);
        emit(store.apply(p, now), now);
    });

    bool warned = false;
    try {
        g.undated = estimateUndated(files, ref_noon, experimental);
        for (const auto& path : files) {
            FileInfo info;
            info.name = path;
            Reader reader(path);
            std::string raw;
            std::vector<uint8_t> bytes;
            while (reader.getline(raw)) {
                ++info.lines;
                const Line l = classify(raw);
                if (l.what == Line::other) {
                    ++info.other;
                    continue;
                }
                info.kinds.insert(l.kind);
                if (l.what == Line::downlink) {
                    ++info.downlinks;
                    continue;
                }
                ++info.uplinks;
                const std::string& hex = l.kind == InputKind::received ? l.json_hex : raw;
                if (hexToBytes(hex, bytes)) {
                    if (const auto h = parseUplinkHeader(bytes.data(), bytes.size())) {
                        GroundStation& st = g.stations[stationKey(*h)];
                        st.lat = h->lat_deg;
                        st.lon = h->lon_deg;
                        st.site_ids.insert(h->tisb_site_id);
                        ++st.uplinks;
                        st.app_data += h->app_data_valid;
                    }
                }
                if (l.time) {
                    ++g.uplinks_timed;
                    line_ref = *l.time;
                    if (!g.first_time || line_ref < *g.first_time)
                        g.first_time = line_ref;
                    if (!g.last_time || line_ref > *g.last_time)
                        g.last_time = line_ref;
                } else {
                    ++g.uplinks_undated;
                    line_ref = g.undated.time;
                    if (!ref_date && !warned) {
                        warned = true;
                        std::cerr << "skycue-fisb: WARN: " << path << ":" << info.lines
                                  << ": uplinks without a timestamp; dating them by today ("
                                  << formatIso8601Ms(ref_noon).substr(0, 10) << " UTC); give --ref-date\n";
                    }
                }
                advance(line_ref);
                decoder.feed(line_ref, hex);
                decoder.sweep(*clock);
                emit(store.sweep(*clock), *clock);
            }
            g.files.push_back(std::move(info));
        }
    } catch (const std::exception& e) {
        std::cerr << "skycue-fisb: " << e.what() << "\n";
        return 1;
    }
    if (clock) {  // publish the mosaics still pending
        const TimePoint last = *clock + kMosaicQuiet;
        decoder.sweep(last);
        emit(store.sweep(last), last);
    }

    if (!json)
        std::cout << formatSummary(g, decoder, store, ref_date, experimental);
    std::cout.flush();
    if (!std::cout)
        return 1;
    if (decoder.counters().uplinks == 0) {
        std::cerr << "skycue-fisb: no uplink lines in the input\n";
        return 3;
    }
    return 0;
}
