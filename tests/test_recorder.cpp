#define BOOST_TEST_MODULE skycued_recorder
#include <boost/test/unit_test.hpp>

#include "json_lite.hpp"
#include "readsb_json_source.hpp"
#include "recorder.hpp"
#include "geom_altitude.hpp"
#include "replay_source.hpp"
#include "track_table.hpp"
#include "time_util.hpp"

#include <skycue/types.hpp>

#include <boost/asio/io_context.hpp>

#include <dirent.h>
#include <ftw.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using skycued::parseReadsbLine;
using skycued::pathExists;
using skycued::Recorder;
using skycued::RecorderConfig;
using skycued::ReplaySource;
using skycued::TrackObservation;
using sys_clock = std::chrono::system_clock;
using steady = std::chrono::steady_clock;

namespace {

const std::string kReceivedHeader =
    "time,type,hex,addr_type,source,pos_time,lat,lon,on_ground,alt_baro_ft,alt_geom_ft,gs_kt,track_deg,"
    "baro_rate_fpm,geom_rate_fpm,callsign,squawk,registration,nic,nac_p,rssi,uplink_hex,input";
const std::string kSentHeader =
    "time,event,hex,addr_type,source,pos_time,lat,lon,on_ground,alt_is_geom,alt_baro_ft,alt_geom_ft,gs_kt,"
    "track_deg,vrate_fpm,callsign,squawk,registration,nic,nac_p,keyframe,drop_reason,note";

int removeEntry(const char* path, const struct stat*, int, struct FTW*) { return ::remove(path); }

// A fresh directory under $TMPDIR (or /tmp), removed with its contents.
struct TempDir {
    std::string path;
    TempDir()
    {
        const char* base = std::getenv("TMPDIR");
        std::string tmpl = std::string(base && *base ? base : "/tmp") + "/skycue-recorder-XXXXXX";
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        if (!::mkdtemp(buf.data()))
            throw std::runtime_error("mkdtemp failed");
        path = buf.data();
    }
    ~TempDir() { ::nftw(path.c_str(), removeEntry, 16, FTW_DEPTH | FTW_PHYS); }
};

std::vector<std::string> filesWithSuffix(const std::string& dir, const std::string& suffix)
{
    std::vector<std::string> out;
    DIR* d = ::opendir(dir.c_str());
    if (!d)
        return out;
    while (dirent* e = ::readdir(d)) {
        std::string name = e->d_name;
        if (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
            out.push_back(dir + "/" + name);
    }
    ::closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> readLines(const std::string& path)
{
    std::ifstream in(path);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line))
        lines.push_back(line);
    return lines;
}

// RFC 4180 split of one CSV line (no embedded newlines in these tests).
std::vector<std::string> splitCsv(const std::string& line)
{
    std::vector<std::string> out(1);
    bool quoted = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (quoted) {
            if (c == '"' && i + 1 < line.size() && line[i + 1] == '"') {
                out.back() += '"';
                ++i;
            } else if (c == '"') {
                quoted = false;
            } else {
                out.back() += c;
            }
        } else if (c == '"') {
            quoted = true;
        } else if (c == ',') {
            out.emplace_back();
        } else {
            out.back() += c;
        }
    }
    return out;
}

std::size_t column(const std::string& header, const std::string& name)
{
    auto cols = splitCsv(header);
    auto it = std::find(cols.begin(), cols.end(), name);
    if (it == cols.end())
        throw std::runtime_error("no column " + name);
    return static_cast<std::size_t>(it - cols.begin());
}

std::vector<TrackObservation> fixtureObservations(std::size_t n)
{
    std::ifstream in(std::string(SKYCUE_FIXTURE_DIR) + "/readsb-sample.jsonl");
    std::vector<TrackObservation> out;
    std::string line;
    while (out.size() < n && std::getline(in, line))
        if (auto obs = parseReadsbLine(line))
            out.push_back(*obs);
    return out;
}

TrackObservation minimalObservation(sys_clock::time_point when)
{
    TrackObservation obs;
    obs.id = {0xabc123, skycue::AddressType::icao};
    obs.source = skycue::Source::adsb_1090;
    obs.received_time = when;
    return obs;
}

sys_clock::time_point roundMs(sys_clock::time_point t)
{
    return sys_clock::time_point(std::chrono::round<std::chrono::milliseconds>(t.time_since_epoch()));
}

// Records `observations` into a fresh received-stream JSONL file; returns its path.
std::string recordObservations(const std::string& dir, const std::vector<TrackObservation>& observations)
{
    RecorderConfig cfg;
    cfg.dir = dir;
    cfg.csv = false;
    Recorder rec(cfg, "received");
    for (const auto& o : observations)
        rec.writeObservation(o);
    rec.flush();
    return rec.currentPath("jsonl");
}

struct Arrival {
    TrackObservation obs;
    steady::time_point at;
};

}  // namespace

BOOST_AUTO_TEST_CASE(time_and_hex_helpers)
{
    auto t = skycued::parseIso8601Ms("2026-11-01T12:34:56.789Z");
    BOOST_REQUIRE(t);
    BOOST_TEST(skycued::formatIso8601Ms(*t) == "2026-11-01T12:34:56.789Z");
    BOOST_TEST(std::chrono::duration_cast<std::chrono::milliseconds>(t->time_since_epoch()).count() ==
               1793536496789LL);
    BOOST_TEST(skycued::formatIso8601Ms(*t + 400us) == "2026-11-01T12:34:56.789Z");
    BOOST_TEST(skycued::formatIso8601Ms(*t + 600us) == "2026-11-01T12:34:56.790Z");
    BOOST_TEST(skycued::formatCompactUtc(*t) == "20261101-123456");
    BOOST_TEST(skycued::parseIso8601Ms("2026-11-01T12:34:56Z").has_value());
    BOOST_TEST(!skycued::parseIso8601Ms("2026-11-01 12:34:56.789Z").has_value());
    BOOST_TEST(!skycued::parseIso8601Ms("2026-02-31T00:00:00.000Z").has_value());
    BOOST_TEST(!skycued::parseIso8601Ms("2026-11-01T12:34:56.7890Z").has_value());
    BOOST_TEST(!skycued::parseIso8601Ms("2026-11-01T12:34:56.789").has_value());
    BOOST_TEST(skycued::hexAddress(0xa1b2c3) == "a1b2c3");
    BOOST_TEST(skycued::hexAddress(0x00000f) == "00000f");
    BOOST_TEST(*skycued::parseHexAddress("A1b2C3") == 0xa1b2c3u);
    BOOST_TEST(!skycued::parseHexAddress("a1b2c").has_value());
    BOOST_TEST(!skycued::parseHexAddress("a1b2cg").has_value());
}

// Found by fuzz_recording (mode 1, a capture line with year 1013):
// parseIso8601Ms took any 4-digit year, but system_clock counts int64
// nanoseconds, which cover 1677-09-21 to 2262-04-11 only; the milliseconds
// to nanoseconds conversion overflowed (undefined behaviour) for years
// outside that range.
BOOST_AUTO_TEST_CASE(fuzz_recording_iso8601_out_of_range)
{
    using skycued::parseIso8601Ms;
    for (const char* bad : {"1013-11-02T10:15:00.123Z", "0000-01-01T00:00:00Z", "9999-12-31T23:59:59.999Z",
                            "2262-04-11T23:47:16.855Z", "1677-09-21T00:12:43.145Z"})
        BOOST_TEST(!parseIso8601Ms(bad).has_value(), "accepted: " << bad);
    // The ends of the range still parse, exactly.
    const auto last = parseIso8601Ms("2262-04-11T23:47:16.854Z");
    BOOST_REQUIRE(last);
    BOOST_TEST(std::chrono::duration_cast<std::chrono::milliseconds>(last->time_since_epoch()).count() ==
               9223372036854LL);
    const auto first = parseIso8601Ms("1677-09-21T00:12:43.146Z");
    BOOST_REQUIRE(first);
    BOOST_TEST(std::chrono::duration_cast<std::chrono::milliseconds>(first->time_since_epoch()).count() ==
               -9223372036854LL);
    // A recording line with such a time is not a record.
    skycued::RecordingRecord rec;
    BOOST_TEST((skycued::parseRecordingLine(R"({"time":"9999-12-31T23:59:59.999Z","type":"uat_uplink","uplink_hex":"00"})",
                                            rec) == skycued::LineKind::ignore));
}

BOOST_AUTO_TEST_CASE(jsonl_and_csv_written_with_headers)
{
    TempDir tmp;
    const std::string dir = tmp.path + "/nested/rec";  // created on demand
    auto obs = fixtureObservations(1).at(0);
    std::string received_jsonl, received_csv;
    {
        Recorder rec(RecorderConfig{dir, true, true, 1ull << 20, 3600s}, "received");
        rec.writeObservation(obs);
        rec.flush();
        received_jsonl = rec.currentPath("jsonl");
        received_csv = rec.currentPath(".csv");
    }
    BOOST_TEST(received_jsonl.compare(0, dir.size() + 10, dir + "/received-") == 0);
    BOOST_TEST(received_jsonl.size() == dir.size() + std::string("/received-YYYYMMDD-HHMMSS.jsonl").size());
    BOOST_TEST(received_csv.substr(0, received_csv.size() - 4) == received_jsonl.substr(0, received_jsonl.size() - 6));

    auto csv = readLines(received_csv);
    BOOST_REQUIRE(csv.size() == 2u);
    BOOST_TEST(csv[0] == kReceivedHeader);
    BOOST_TEST(skycued::recording::receivedCsvHeader() == kReceivedHeader);
    auto row = splitCsv(csv[1]);
    BOOST_REQUIRE(row.size() == splitCsv(kReceivedHeader).size());
    BOOST_TEST(row[column(kReceivedHeader, "time")] == skycued::formatIso8601Ms(obs.received_time));
    BOOST_TEST(row[column(kReceivedHeader, "type")] == "obs");
    BOOST_TEST(row[column(kReceivedHeader, "hex")] == "ac5e81");
    BOOST_TEST(row[column(kReceivedHeader, "addr_type")] == "icao");
    BOOST_TEST(row[column(kReceivedHeader, "source")] == "adsb_1090");
    BOOST_TEST(row[column(kReceivedHeader, "lat")] == "37.887976");
    BOOST_TEST(row[column(kReceivedHeader, "lon")] == "-76.261396");
    BOOST_TEST(row[column(kReceivedHeader, "on_ground")] == "0");
    BOOST_TEST(row[column(kReceivedHeader, "alt_baro_ft")] == "18000");
    BOOST_TEST(row[column(kReceivedHeader, "track_deg")] == "329.67");
    BOOST_TEST(row[column(kReceivedHeader, "callsign")] == "RPA4521");
    BOOST_TEST(row[column(kReceivedHeader, "rssi")] == "-15.8");
    BOOST_TEST(row[column(kReceivedHeader, "uplink_hex")] == "");

    auto jsonl = readLines(received_jsonl);
    BOOST_REQUIRE(jsonl.size() == 1u);
    const std::string prefix = "{\"time\":\"" + skycued::formatIso8601Ms(obs.received_time) + "\",\"type\":\"obs\",";
    BOOST_TEST(jsonl[0].compare(0, prefix.size(), prefix) == 0);
    auto parsed = skycued::JsonObject::parse(jsonl[0]);
    BOOST_REQUIRE(parsed);
    BOOST_TEST(*parsed->getString("raw") == obs.raw_line);
    BOOST_TEST(*parsed->getString("emergency") == "none");
    BOOST_TEST(*parsed->getString("category") == "A3");

    // Sent stream.
    skycue::Track track;
    track.id = {0xa5b06d, skycue::AddressType::non_icao};
    track.source = skycue::Source::uat_978;
    track.position_time = obs.received_time;
    track.lat_deg = 38.233783;
    track.lon_deg = -77.0;
    track.keyframe = true;
    track.alt_is_geom = true;
    track.alt_geom_ft = 3100;
    track.vertical_rate_fpm = -64.0f;
    track.callsign = std::string("N512RG");
    Recorder sent(RecorderConfig{dir, true, true, 1ull << 20, 3600s}, "sent");
    sent.writeTrack(track, "keyframe");
    sent.flush();
    auto scsv = readLines(sent.currentPath("csv"));
    BOOST_REQUIRE(scsv.size() == 2u);
    BOOST_TEST(scsv[0] == kSentHeader);
    BOOST_TEST(skycued::recording::sentCsvHeader() == kSentHeader);
    auto srow = splitCsv(scsv[1]);
    BOOST_REQUIRE(srow.size() == splitCsv(kSentHeader).size());
    BOOST_TEST(srow[column(kSentHeader, "event")] == "keyframe");
    BOOST_TEST(srow[column(kSentHeader, "hex")] == "a5b06d");
    BOOST_TEST(srow[column(kSentHeader, "addr_type")] == "non_icao");
    BOOST_TEST(srow[column(kSentHeader, "source")] == "uat_978");
    BOOST_TEST(srow[column(kSentHeader, "pos_time")] == skycued::formatIso8601Ms(obs.received_time));
    BOOST_TEST(srow[column(kSentHeader, "lon")] == "-77");
    BOOST_TEST(srow[column(kSentHeader, "alt_is_geom")] == "1");
    BOOST_TEST(srow[column(kSentHeader, "alt_baro_ft")] == "");
    BOOST_TEST(srow[column(kSentHeader, "alt_geom_ft")] == "3100");
    BOOST_TEST(srow[column(kSentHeader, "vrate_fpm")] == "-64");
    BOOST_TEST(srow[column(kSentHeader, "keyframe")] == "1");
    BOOST_TEST(srow[column(kSentHeader, "drop_reason")] == "");
    auto sjsonl = readLines(sent.currentPath("jsonl"));
    BOOST_REQUIRE(sjsonl.size() == 1u);
    BOOST_TEST(sjsonl[0].compare(0, 9, "{\"time\":\"") == 0);
    BOOST_TEST(sjsonl[0].find("\",\"event\":\"keyframe\",\"hex\":\"a5b06d\"") == 33u);
}

BOOST_AUTO_TEST_CASE(sent_drop_and_filter_change_records)
{
    TempDir tmp;
    Recorder sent(RecorderConfig{tmp.path, true, true, 1ull << 20, 3600s}, "sent");
    sent.writeDrop({0x123456, skycue::AddressType::tisb_trackfile}, skycue::DropReason::source_lost);
    sent.writeFilterChange("allow list \"ops\", 3 entries\nreloaded");
    sent.flush();
    auto csv = readLines(sent.currentPath("csv"));
    // The note's newline is quoted, so it spans two physical lines.
    BOOST_REQUIRE(csv.size() == 4u);
    auto drop = splitCsv(csv[1]);
    BOOST_TEST(drop[column(kSentHeader, "event")] == "drop");
    BOOST_TEST(drop[column(kSentHeader, "hex")] == "123456");
    BOOST_TEST(drop[column(kSentHeader, "addr_type")] == "tisb_trackfile");
    BOOST_TEST(drop[column(kSentHeader, "drop_reason")] == "source_lost");
    BOOST_TEST(drop[column(kSentHeader, "on_ground")] == "");
    BOOST_TEST(drop[column(kSentHeader, "keyframe")] == "");
    auto note = splitCsv(csv[2] + "\n" + csv[3]);
    BOOST_TEST(note[column(kSentHeader, "event")] == "filter_change");
    BOOST_TEST(note[column(kSentHeader, "note")] == "allow list \"ops\", 3 entries\nreloaded");
    BOOST_TEST(csv[2].find(",\"allow list \"\"ops\"\", 3 entries") != std::string::npos);

    auto jsonl = readLines(sent.currentPath("jsonl"));
    BOOST_REQUIRE(jsonl.size() == 2u);
    auto d = skycued::JsonObject::parse(jsonl[0]);
    BOOST_REQUIRE(d);
    BOOST_TEST(*d->getString("event") == "drop");
    BOOST_TEST(*d->getString("drop_reason") == "source_lost");
    BOOST_TEST(!d->has("lat"));
    auto f = skycued::JsonObject::parse(jsonl[1]);
    BOOST_REQUIRE(f);
    BOOST_TEST(*f->getString("note") == "allow list \"ops\", 3 entries\nreloaded");
}

BOOST_AUTO_TEST_CASE(csv_empty_for_absent_optionals)
{
    TempDir tmp;
    Recorder rec(RecorderConfig{tmp.path, true, true, 1ull << 20, 3600s}, "received");
    auto obs = minimalObservation(sys_clock::now());
    obs.on_ground = false;
    obs.callsign = std::string("A,\"B\"");
    rec.writeObservation(obs);
    rec.flush();
    auto csv = readLines(rec.currentPath("csv"));
    BOOST_REQUIRE(csv.size() == 2u);
    auto row = splitCsv(csv[1]);
    auto cols = splitCsv(kReceivedHeader);
    BOOST_REQUIRE(row.size() == cols.size());
    for (std::size_t i = 0; i < cols.size(); ++i) {
        const std::string& c = cols[i];
        if (c == "time" || c == "type" || c == "hex" || c == "addr_type" || c == "source" || c == "on_ground" ||
            c == "callsign")
            BOOST_TEST(!row[i].empty(), c);
        else
            BOOST_TEST(row[i].empty(), c);
    }
    BOOST_TEST(row[column(kReceivedHeader, "callsign")] == "A,\"B\"");
    BOOST_TEST(csv[1].find(",\"A,\"\"B\"\"\",") != std::string::npos);

    auto jsonl = readLines(rec.currentPath("jsonl"));
    BOOST_REQUIRE(jsonl.size() == 1u);
    auto j = skycued::JsonObject::parse(jsonl[0]);
    BOOST_REQUIRE(j);
    BOOST_TEST(!j->has("lat"));
    BOOST_TEST(!j->has("pos_time"));
    BOOST_TEST(!j->has("alt_baro_ft"));
    BOOST_TEST(!j->has("rssi"));
    BOOST_TEST(*j->getString("on_ground") == "false");
}

BOOST_AUTO_TEST_CASE(obs_input_round_trips)
{
    TempDir tmp;
    Recorder rec(RecorderConfig{tmp.path, true, true, 1ull << 20, 3600s}, "received");
    auto obs = minimalObservation(sys_clock::now());
    obs.input_id = "ps-north";
    rec.writeObservation(obs);
    rec.flush();

    auto csv = readLines(rec.currentPath("csv"));
    BOOST_REQUIRE(csv.size() == 2u);
    auto row = splitCsv(csv[1]);
    BOOST_REQUIRE(row.size() == splitCsv(kReceivedHeader).size());
    BOOST_TEST(column(kReceivedHeader, "input") == row.size() - 1);  // the last column
    BOOST_TEST(row[column(kReceivedHeader, "input")] == "ps-north");

    auto jsonl = readLines(rec.currentPath("jsonl"));
    BOOST_REQUIRE(jsonl.size() == 1u);
    auto j = skycued::JsonObject::parse(jsonl[0]);
    BOOST_REQUIRE(j);
    BOOST_TEST(j->getString("input").value_or("") == "ps-north");
    auto back = skycued::recording::observationFromRecord(*j);
    BOOST_REQUIRE(back);
    BOOST_TEST(back->input_id == "ps-north");

    // A record without the key reads as no input.
    auto none = skycued::JsonObject::parse(R"({"time":"2026-11-01T00:00:00.000Z","type":"obs","hex":"abc123"})");
    BOOST_REQUIRE(none);
    auto plain = skycued::recording::observationFromRecord(*none);
    BOOST_REQUIRE(plain);
    BOOST_TEST(plain->input_id.empty());
}

BOOST_AUTO_TEST_CASE(rebroadcast_flag_round_trips)
{
    TempDir tmp;
    auto obs = minimalObservation(sys_clock::now());
    obs.rebroadcast = true;
    auto plain = minimalObservation(sys_clock::now());
    plain.id.address = 0xabc124;
    const auto lines = readLines(recordObservations(tmp.path, {obs, plain}));
    BOOST_REQUIRE(lines.size() == 2u);

    auto j = skycued::JsonObject::parse(lines[0]);
    BOOST_REQUIRE(j);
    auto back = skycued::recording::observationFromRecord(*j);
    BOOST_REQUIRE(back);
    BOOST_TEST(back->rebroadcast);

    auto k = skycued::JsonObject::parse(lines[1]);
    BOOST_REQUIRE(k);
    BOOST_TEST(!k->has("rebroadcast"));  // false is not written
    auto back_plain = skycued::recording::observationFromRecord(*k);
    BOOST_REQUIRE(back_plain);
    BOOST_TEST(!back_plain->rebroadcast);

    // A record without the key reads false.
    auto none = skycued::JsonObject::parse(R"({"time":"2026-11-01T00:00:00.000Z","type":"obs","hex":"abc123"})");
    BOOST_REQUIRE(none);
    auto absent = skycued::recording::observationFromRecord(*none);
    BOOST_REQUIRE(absent);
    BOOST_TEST(!absent->rebroadcast);
}

BOOST_AUTO_TEST_CASE(obs_without_on_ground_omits_key)
{
    TempDir tmp;
    Recorder rec(RecorderConfig{tmp.path, true, true, 1ull << 20, 3600s}, "received");
    auto obs = minimalObservation(sys_clock::now());
    BOOST_REQUIRE(!obs.on_ground.has_value());
    rec.writeObservation(obs);
    rec.flush();

    auto jsonl = readLines(rec.currentPath("jsonl"));
    BOOST_REQUIRE(jsonl.size() == 1u);
    auto j = skycued::JsonObject::parse(jsonl[0]);
    BOOST_REQUIRE(j);
    BOOST_TEST(!j->has("on_ground"));
    BOOST_TEST(!j->has("input"));  // no input id either

    auto csv = readLines(rec.currentPath("csv"));
    BOOST_REQUIRE(csv.size() == 2u);
    auto row = splitCsv(csv[1]);
    BOOST_REQUIRE(row.size() == splitCsv(kReceivedHeader).size());
    BOOST_TEST(row[column(kReceivedHeader, "on_ground")] == "");
    BOOST_TEST(row[column(kReceivedHeader, "input")] == "");
}

BOOST_AUTO_TEST_CASE(on_ground_key_absent_reads_unset)
{
    auto j = skycued::JsonObject::parse(
        R"({"time":"2026-11-01T00:00:00.000Z","type":"obs","hex":"abc123","alt_baro_ft":12000})");
    BOOST_REQUIRE(j);
    auto obs = skycued::recording::observationFromRecord(*j);
    BOOST_REQUIRE(obs);
    BOOST_TEST(!obs->on_ground.has_value());
}

BOOST_AUTO_TEST_CASE(on_ground_present_reads_as_written)
{
    // No heuristic: a present key is the value, whatever else the record has
    // (old recordings always carry the key, so their meaning is unchanged).
    auto f = skycued::JsonObject::parse(
        R"({"time":"2026-11-01T00:00:00.000Z","type":"obs","hex":"abc123","on_ground":false})");
    BOOST_REQUIRE(f);
    auto airborne = skycued::recording::observationFromRecord(*f);
    BOOST_REQUIRE(airborne);
    BOOST_TEST((airborne->on_ground == false));

    auto t = skycued::JsonObject::parse(
        R"({"time":"2026-11-01T00:00:00.000Z","type":"obs","hex":"abc123","on_ground":true})");
    BOOST_REQUIRE(t);
    auto ground = skycued::recording::observationFromRecord(*t);
    BOOST_REQUIRE(ground);
    BOOST_TEST((ground->on_ground == true));
}

BOOST_AUTO_TEST_CASE(rotation_by_size)
{
    TempDir tmp;
    Recorder rec(RecorderConfig{tmp.path, true, true, 2048, 3600s}, "received");
    const std::string first_jsonl = rec.currentPath("jsonl");
    for (const auto& o : fixtureObservations(20))
        rec.writeObservation(o);
    rec.flush();
    auto jsonl = filesWithSuffix(tmp.path, ".jsonl");
    auto csv = filesWithSuffix(tmp.path, ".csv");
    BOOST_TEST(jsonl.size() >= 2u);
    BOOST_TEST(csv.size() >= 2u);
    BOOST_TEST(rec.currentPath("jsonl") != first_jsonl);
    std::size_t records = 0;
    for (const auto& path : jsonl)
        records += readLines(path).size();
    BOOST_TEST(records == 20u);
    for (const auto& path : csv) {
        auto lines = readLines(path);
        BOOST_REQUIRE(!lines.empty());
        BOOST_TEST(lines[0] == kReceivedHeader, path);
    }
    // Rotation within one second appends -1, -2, ... to the name.
    bool suffixed = false;
    for (const auto& path : jsonl)
        suffixed = suffixed || path.find("-1.jsonl") != std::string::npos;
    BOOST_TEST(suffixed);
}

BOOST_AUTO_TEST_CASE(retention_cap_on_start_and_rotation)
{
    TempDir tmp;
    ::mkdir((tmp.path + "/raw").c_str(), 0755);
    // Files a day old, so that retention's recent-file protection does not apply.
    auto age = [](const std::string& p) {
        struct timeval tv[2] {};
        ::gettimeofday(&tv[0], nullptr);
        tv[0].tv_sec -= 86400;
        tv[1] = tv[0];
        ::utimes(p.c_str(), tv);
    };
    auto put = [&](const std::string& rel, std::size_t n) {
        std::ofstream(tmp.path + "/" + rel) << std::string(n, 'x');
        age(tmp.path + "/" + rel);
    };
    put("received-20200101-000000.jsonl", 4000);
    put("raw/beast-20200101-000000.bin", 4000);
    put("raw/beast-20200102-000000.bin", 100);  // the newest beast file: kept (rawrec may be writing it)
    put("notes.txt", 50000);                     // not ours: never counted or deleted

    // max_bytes 0: nothing is deleted.
    std::string sent_jsonl;
    {
        RecorderConfig off{tmp.path, true, true, 1ull << 30, 3600s};
        Recorder rec(off, "sent");
        sent_jsonl = rec.currentPath("jsonl");
    }
    for (const auto& suffix : {".jsonl", ".csv"})
        for (const auto& p : filesWithSuffix(tmp.path, suffix))
            age(p);
    BOOST_TEST(pathExists(tmp.path + "/received-20200101-000000.jsonl"));

    // A cap: on start the oldest files go first, then on every rotation.
    RecorderConfig cfg{tmp.path, true, true, 2048, 3600s};
    cfg.max_bytes = 4000;
    Recorder rec(cfg, "received");
    BOOST_TEST(!pathExists(tmp.path + "/received-20200101-000000.jsonl"));
    BOOST_TEST(!pathExists(tmp.path + "/raw/beast-20200101-000000.bin"));
    BOOST_TEST(pathExists(tmp.path + "/raw/beast-20200102-000000.bin"));
    BOOST_TEST(pathExists(tmp.path + "/notes.txt"));
    for (const auto& o : fixtureObservations(60)) {
        rec.writeObservation(o);
        // Retention never removes the files being written.
        BOOST_REQUIRE(pathExists(rec.currentPath("jsonl")));
        BOOST_REQUIRE(pathExists(rec.currentPath("csv")));
        // Let the closed files look old, as they would after hourly rotations.
        for (const auto& suffix : {".jsonl", ".csv"})
            for (const auto& p : filesWithSuffix(tmp.path, suffix))
                if (p != rec.currentPath("jsonl") && p != rec.currentPath("csv"))
                    age(p);
    }
    rec.flush();
    uint64_t total = 0;
    for (const auto& suffix : {".jsonl", ".csv"})
        for (const auto& p : filesWithSuffix(tmp.path, suffix)) {
            struct stat st {};
            ::stat(p.c_str(), &st);
            total += static_cast<uint64_t>(st.st_size);
        }
    // Within the cap after the last rotation, give or take the current pair's growth since.
    BOOST_TEST(total <= 4000u + 2 * 2048u + 1024u);
    BOOST_TEST(filesWithSuffix(tmp.path, ".jsonl").size() >= 1u);
    BOOST_TEST(pathExists(tmp.path + "/notes.txt"));
    // The newest sent pair (another Recorder's, maybe still open) is kept.
    BOOST_TEST(pathExists(sent_jsonl));
}

BOOST_AUTO_TEST_CASE(rotation_by_age)
{
    TempDir tmp;
    Recorder rec(RecorderConfig{tmp.path, true, false, 1ull << 30, 1s}, "received");
    rec.writeObservation(minimalObservation(sys_clock::now()));
    const std::string first = rec.currentPath("jsonl");
    rec.writeObservation(minimalObservation(sys_clock::now()));
    BOOST_TEST(rec.currentPath("jsonl") == first);
    std::this_thread::sleep_for(1100ms);
    rec.writeObservation(minimalObservation(sys_clock::now()));
    rec.flush();
    BOOST_TEST(rec.currentPath("jsonl") != first);
    BOOST_TEST(rec.currentPath("csv") == "");
    BOOST_TEST(filesWithSuffix(tmp.path, ".jsonl").size() == 2u);
    BOOST_TEST(filesWithSuffix(tmp.path, ".csv").empty());
    BOOST_TEST(readLines(first).size() == 2u);
    BOOST_TEST(readLines(rec.currentPath("jsonl")).size() == 1u);
}

BOOST_AUTO_TEST_CASE(uplink_record)
{
    TempDir tmp;
    const auto when = sys_clock::now();
    std::string path;
    {
        Recorder rec(RecorderConfig{tmp.path, true, true, 1ull << 20, 3600s}, "received");
        rec.writeUplink(when, "0a1b2c3d");
        rec.writeObservation(minimalObservation(when + 1s));
        rec.flush();
        path = rec.currentPath("jsonl");
        auto csv = readLines(rec.currentPath("csv"));
        BOOST_REQUIRE(csv.size() == 3u);
        auto row = splitCsv(csv[1]);
        auto cols = splitCsv(kReceivedHeader);
        BOOST_REQUIRE(row.size() == cols.size());
        for (std::size_t i = 0; i < cols.size(); ++i) {
            if (cols[i] == "time")
                BOOST_TEST(row[i] == skycued::formatIso8601Ms(when));
            else if (cols[i] == "type")
                BOOST_TEST(row[i] == "uat_uplink");
            else if (cols[i] == "uplink_hex")
                BOOST_TEST(row[i] == "0a1b2c3d");
            else
                BOOST_TEST(row[i].empty(), cols[i]);
        }
    }
    auto jsonl = readLines(path);
    BOOST_REQUIRE(jsonl.size() == 2u);
    BOOST_TEST(jsonl[0] == "{\"time\":\"" + skycued::formatIso8601Ms(when) +
                               "\",\"type\":\"uat_uplink\",\"uplink_hex\":\"0a1b2c3d\"}");

    // Replay skips uplinks without counting them as bad lines.
    boost::asio::io_context io;
    ReplaySource replay(io, path, 0);
    std::size_t n = 0;
    replay.onObservation([&](const TrackObservation&) { ++n; });
    replay.start();
    io.run();
    BOOST_TEST(n == 1u);
    BOOST_TEST(replay.skippedLines() == 0u);
}

BOOST_AUTO_TEST_CASE(replay_roundtrip)
{
    TempDir tmp;
    auto original = fixtureObservations(50);
    BOOST_REQUIRE(original.size() == 50u);
    const std::string path = recordObservations(tmp.path, original);

    boost::asio::io_context io;
    ReplaySource replay(io, path, 0);
    std::vector<TrackObservation> got;
    bool ended = false;
    replay.onObservation([&](const TrackObservation& o) { got.push_back(o); });
    replay.onEnd([&] { ended = true; });
    replay.start();
    BOOST_TEST(got.empty());  // start() only schedules
    io.run();
    BOOST_TEST(ended);
    BOOST_TEST(replay.skippedLines() == 0u);
    BOOST_REQUIRE(got.size() == original.size());
    std::size_t with_position = 0;
    for (std::size_t i = 0; i < got.size(); ++i) {
        BOOST_TEST_CONTEXT("observation " << i)
        {
            const auto& a = original[i];
            const auto& b = got[i];
            BOOST_TEST(b.id.address == a.id.address);
            BOOST_TEST(static_cast<int>(b.id.type) == static_cast<int>(a.id.type));
            BOOST_TEST(static_cast<int>(b.source) == static_cast<int>(a.source));
            BOOST_TEST((b.received_time == roundMs(a.received_time)));
            BOOST_TEST(b.position_time.has_value() == a.position_time.has_value());
            if (a.position_time && b.position_time) {
                BOOST_TEST((*b.position_time == roundMs(*a.position_time)));
                ++with_position;
            }
            BOOST_TEST((b.lat == a.lat));
            BOOST_TEST((b.lon == a.lon));
            BOOST_TEST((b.on_ground == a.on_ground));
            BOOST_TEST((b.alt_baro_ft == a.alt_baro_ft));
            BOOST_TEST((b.alt_geom_ft == a.alt_geom_ft));
            BOOST_TEST((b.gs_kt == a.gs_kt));
            BOOST_TEST((b.track_deg == a.track_deg));
            BOOST_TEST((b.baro_rate_fpm == a.baro_rate_fpm));
            BOOST_TEST((b.geom_rate_fpm == a.geom_rate_fpm));
            BOOST_TEST((b.callsign == a.callsign));
            BOOST_TEST((b.squawk == a.squawk));
            BOOST_TEST((b.registration == a.registration));
            BOOST_TEST((b.emergency == a.emergency));
            BOOST_TEST((b.category == a.category));
            BOOST_TEST((b.nic == a.nic));
            BOOST_TEST((b.nac_p == a.nac_p));
            BOOST_TEST((b.rssi == a.rssi));
            BOOST_TEST(b.raw_line == a.raw_line);
        }
    }
    BOOST_TEST(with_position > 40u);  // the fixture's positions were exercised
}

BOOST_AUTO_TEST_CASE(replay_reads_hand_edited_records_and_skips_bad_lines)
{
    TempDir tmp;
    const std::string path = tmp.path + "/edited.jsonl";
    {
        std::ofstream out(path);
        out << "{\"time\":\"2026-11-01T00:00:00.000Z\",\"type\":\"obs\",\"hex\":\"ABCDEF\",\"addr_type\":\"non_icao\","
               "\"source\":\"tisb\",\"lat\":1.5,\"lon\":2.5,\"pos_time\":\"2026-11-01T00:00:00.000Z\","
               "\"on_ground\":true,\"callsign\":\"X\",\"raw\":\"{\\\"hex\\\":\\\"000000\\\"}\"}\n"
            << "not json\n"
            << "\n"
            << "{\"time\":\"2026-11-01T00:00:00.100Z\",\"type\":\"obs\",\"hex\":\"12345\"}\n"
            << "{\"time\":\"2026-11-01T00:00:00.200Z\",\"type\":\"obs\",\"hex\":\"123456\",\"nic\":300}\n"
            << "{\"time\":\"2026-11-01T00:00:00.300Z\",\"type\":\"mystery\"}\n"
            << "{\"time\":\"2026-11-01T00:00:00.400Z\",\"type\":\"obs\",\"hex\":\"123456\"}\n";
    }
    boost::asio::io_context io;
    ReplaySource replay(io, path, 0);
    BOOST_TEST(replay.recordCount() == 2u);
    BOOST_TEST(replay.skippedLines() == 4u);
    std::vector<TrackObservation> got;
    replay.onObservation([&](const TrackObservation& o) { got.push_back(o); });
    replay.start();
    io.run();
    BOOST_REQUIRE(got.size() == 2u);
    BOOST_TEST(got[0].id.address == 0xabcdefu);
    BOOST_TEST(static_cast<int>(got[0].id.type) == static_cast<int>(skycue::AddressType::non_icao));
    BOOST_TEST(static_cast<int>(got[0].source) == static_cast<int>(skycue::Source::tisb));
    BOOST_TEST((got[0].on_ground == true));
    BOOST_TEST(*got[0].lat == 1.5);
    BOOST_TEST(*got[0].callsign == "X");
    BOOST_TEST(got[0].raw_line == "{\"hex\":\"000000\"}");  // raw is carried, never parsed
    BOOST_TEST(got[1].id.address == 0x123456u);
    BOOST_TEST(!got[1].lat.has_value());

    BOOST_CHECK_THROW(ReplaySource(io, tmp.path + "/missing.jsonl", 0), std::runtime_error);
    BOOST_CHECK_THROW(ReplaySource(io, path, -1), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(replay_pacing)
{
    TempDir tmp;
    const auto t0 = sys_clock::now();
    const std::string path = recordObservations(tmp.path, {minimalObservation(t0), minimalObservation(t0 + 1s)});
    boost::asio::io_context io;
    ReplaySource replay(io, path, 10);
    std::vector<steady::time_point> at;
    replay.onObservation([&](const TrackObservation&) { at.push_back(steady::now()); });
    replay.start();
    io.run();
    BOOST_REQUIRE(at.size() == 2u);
    const auto gap = std::chrono::duration_cast<std::chrono::milliseconds>(at[1] - at[0]).count();
    BOOST_TEST(gap >= 80);
    BOOST_TEST(gap < 400);
}

// Found by fuzz_recording (mode 0) on seed 5, a readsb line of
// fixtures/fisb/received-with-uplinks.jsonl (2015) against a replay started
// in 2026 at speed 1e-6: the delay was clamped above only, so a record before
// the first one (a hand-edited file need not be in order) gave -3.7e23 s,
// and the double-to-int64 nanosecond conversion overflowed (undefined
// behaviour, a garbage timer expiry).
BOOST_AUTO_TEST_CASE(fuzz_recording_replay_delay_overflow)
{
    using skycued::replayDelay;
    skycued::RecordingRecord rec;
    BOOST_REQUIRE(skycued::parseRecordingLine(R"({"now":1422072720.377,"hex":"a3b5f8"})", rec) ==
                  skycued::LineKind::readsb_raw);
    const sys_clock::time_point start{std::chrono::seconds(1793577600)};  // 2026-11-02T00:00:00Z
    BOOST_TEST((replayDelay(rec.time - start, 1e-6) == steady::duration::zero()));
    BOOST_TEST((replayDelay(-1s, 1.0) == steady::duration::zero()));  // earlier records play at once

    const auto max = std::chrono::duration_cast<steady::duration>(
        std::chrono::duration<double>(skycued::kMaxReplayDelaySeconds));
    BOOST_TEST((replayDelay(start - rec.time, 1e-6) == max));
    BOOST_TEST((replayDelay(sys_clock::duration::max(), 1e-300) == max));
    BOOST_TEST((replayDelay(2s, 4.0) == steady::duration(500ms)));
    BOOST_TEST((replayDelay(0s, 1.0) == steady::duration::zero()));
}

// Found in review of fuzz_recording: a recording may hold a 1677 and a 2026
// record (both inside the nanosecond clock's range), but their difference,
// about 349 years, is not: ReplaySource's offsets, gaps, span and loop shifts
// and the harness's offset were plain int64 subtractions that overflowed
// (undefined behaviour; the offset wrapped negative, so the 2026 record
// played at once). They saturate now.
BOOST_AUTO_TEST_CASE(replay_offset_saturates_across_the_clock_range)
{
    using skycued::parseIso8601Ms;
    using skycued::replayOffset;
    const auto old_t = parseIso8601Ms("1677-09-22T00:00:00.000Z");
    const auto new_t = parseIso8601Ms("2026-11-02T00:00:00.000Z");
    BOOST_REQUIRE(old_t);
    BOOST_REQUIRE(new_t);
    BOOST_TEST((replayOffset(*new_t, *old_t) == sys_clock::duration::max()));
    BOOST_TEST((replayOffset(*old_t, *new_t) == sys_clock::duration::min()));
    BOOST_TEST((replayOffset(*new_t, *new_t - 2s) == sys_clock::duration(2s)));
    BOOST_TEST((replayOffset(*new_t - 2s, *new_t) == sys_clock::duration(-2s)));

    TempDir tmp;
    const std::string path = tmp.path + "/span.jsonl";
    {
        std::ofstream out(path);
        out << "{\"time\":\"1677-09-22T00:00:00.000Z\",\"type\":\"obs\",\"hex\":\"123456\"}\n"
            << "{\"time\":\"2026-11-02T00:00:00.000Z\",\"type\":\"obs\",\"hex\":\"abcdef\"}\n";
    }
    // At 1e12 times real time the saturated offset (about 9.2e9 s) is a
    // 9.2 ms wait; the wrapped one was negative, a wait of 0.
    {
        boost::asio::io_context io;
        ReplaySource replay(io, path, 1e12);
        BOOST_REQUIRE(replay.recordCount() == 2u);
        std::vector<steady::time_point> at;
        replay.onObservation([&](const TrackObservation&) { at.push_back(steady::now()); });
        replay.start();
        io.run();
        BOOST_REQUIRE(at.size() == 2u);
        BOOST_TEST((at[1] - at[0] >= 5ms));
        BOOST_TEST((at[1] - at[0] < 2s));
    }
    // Loop mode: the second pass's shift saturates too, so its 2026 record
    // is stamped at the clock's end instead of a wrapped time.
    {
        boost::asio::io_context io;
        ReplaySource replay(io, path, 1e12, true);
        std::vector<sys_clock::time_point> got;
        replay.onObservation([&](const TrackObservation& o) {
            got.push_back(o.received_time);
            if (got.size() == 4u)
                io.stop();
        });
        replay.start();
        io.run();
        BOOST_REQUIRE(got.size() == 4u);
        BOOST_TEST((got[0] == *old_t));
        BOOST_TEST((got[1] == *new_t));
        BOOST_TEST((got[3] == sys_clock::time_point::max()));
    }
}

// A zero factor is a zero shift, whatever the other: the first pass of a
// loop (pass 0) and an empty span (shift 0, a one-record file) must never
// saturate, or a huge pass count with a zero span would move every record to
// the end of the clock's range.
BOOST_AUTO_TEST_CASE(replay_loop_shift_zero_factor_is_zero)
{
    using skycued::replayLoopShift;
    using D = sys_clock::duration;
    BOOST_TEST((replayLoopShift(D::zero(), UINT64_MAX) == D::zero()));
    BOOST_TEST((replayLoopShift(D::zero(), static_cast<uint64_t>(INT64_MAX) + 1) == D::zero()));
    BOOST_TEST((replayLoopShift(D::max(), 0) == D::zero()));
    BOOST_TEST((replayLoopShift(D::min(), 0) == D::zero()));
    BOOST_TEST((replayLoopShift(D(2s), 3) == D(6s)));
    BOOST_TEST((replayLoopShift(D(2s), UINT64_MAX) == D::max()));
    BOOST_TEST((replayLoopShift(D(-2s), UINT64_MAX) == D::min()));
    BOOST_TEST((replayLoopShift(D::max(), 2) == D::max()));
}

BOOST_AUTO_TEST_CASE(replay_default_speed_is_realtime)
{
    TempDir tmp;
    const auto t0 = sys_clock::now();
    const std::string path = recordObservations(tmp.path, {minimalObservation(t0), minimalObservation(t0 + 300ms)});
    boost::asio::io_context io;
    ReplaySource replay(io, path);  // default speed
    std::vector<steady::time_point> at;
    bool ended = false;
    replay.onObservation([&](const TrackObservation&) { at.push_back(steady::now()); });
    replay.onEnd([&] { ended = true; });
    replay.start();
    io.run();
    BOOST_REQUIRE(at.size() == 2u);
    BOOST_TEST(ended);
    const auto gap = std::chrono::duration_cast<std::chrono::milliseconds>(at[1] - at[0]).count();
    BOOST_TEST(gap >= 270);
    BOOST_TEST(gap < 700);
}

BOOST_AUTO_TEST_CASE(replay_loop_shifts_timestamps_monotonic)
{
    TempDir tmp;
    const auto t0 = roundMs(sys_clock::now());
    std::vector<TrackObservation> recorded;
    for (int i = 0; i < 5; ++i) {
        auto o = minimalObservation(t0 + std::chrono::seconds(i));
        o.lat = 1.0;
        o.lon = 2.0;
        o.position_time = o.received_time - 500ms;
        recorded.push_back(o);
    }
    const std::string path = recordObservations(tmp.path, recorded);
    boost::asio::io_context io;
    ReplaySource replay(io, path, 0, true);
    bool ended = false;
    std::vector<TrackObservation> got;
    replay.onObservation([&](const TrackObservation& o) {
        got.push_back(o);
        if (got.size() == 13)
            io.stop();
    });
    replay.onEnd([&] { ended = true; });
    replay.start();
    io.run();
    BOOST_TEST(!ended);
    BOOST_REQUIRE(got.size() == 13u);
    for (std::size_t i = 1; i < got.size(); ++i) {
        BOOST_TEST((got[i].received_time > got[i - 1].received_time), "received_time at " << i);
        BOOST_TEST((*got[i].position_time > *got[i - 1].position_time), "position_time at " << i);
    }
    // Span 4 s + median gap 1 s: each pass is shifted 5 s from the one before.
    for (std::size_t i = 5; i < got.size(); ++i) {
        BOOST_TEST((got[i].received_time - got[i - 5].received_time == sys_clock::duration(5s)), i);
        BOOST_TEST((*got[i].position_time - got[i].received_time == sys_clock::duration(-500ms)), i);
    }
    BOOST_TEST((got[5].received_time == t0 + 5s));
}

BOOST_AUTO_TEST_CASE(recorder_sent_jsonl_has_source_and_uncertainty)
{
    TempDir tmp;
    skycue::Track track;
    track.id = {0xa5b06d, skycue::AddressType::icao};
    track.source = skycue::Source::adsb_1090;
    track.position_time = sys_clock::now();
    track.alt_geom_ft = 35250;
    Recorder rec(RecorderConfig{tmp.path, true, true, 1ull << 20, 3600s}, "sent");
    rec.writeTrack(track, "update");  // reported
    track.alt_geom_uncertainty_ft = 150;
    rec.writeTrack(track, "update");  // estimated
    track.alt_geom_ft.reset();
    track.alt_geom_uncertainty_ft.reset();
    rec.writeTrack(track, "update");  // no geometric altitude: neither key
    rec.flush();
    const auto lines = readLines(rec.currentPath("jsonl"));
    BOOST_REQUIRE(lines.size() == 3u);
    auto a = skycued::JsonObject::parse(lines[0]);
    auto b = skycued::JsonObject::parse(lines[1]);
    auto c = skycued::JsonObject::parse(lines[2]);
    BOOST_REQUIRE(a && b && c);
    BOOST_TEST(a->getString("alt_geom_source").value_or("") == "reported");
    BOOST_TEST(!a->has("alt_geom_uncertainty_ft"));
    BOOST_TEST(b->getString("alt_geom_source").value_or("") == "estimated");
    BOOST_TEST(b->getDouble("alt_geom_uncertainty_ft").value_or(0) == 150.0);
    BOOST_TEST(!c->has("alt_geom_source"));
    BOOST_TEST(!c->has("alt_geom_uncertainty_ft"));
    // The CSV keeps its columns.
    BOOST_TEST(readLines(rec.currentPath("csv"))[0] == kSentHeader);
}

BOOST_AUTO_TEST_CASE(recorder_received_jsonl_has_diff_and_replays_identically)
{
    TempDir tmp;
    Recorder rec(RecorderConfig{tmp.path, true, true, 1ull << 20, 3600s}, "received");
    auto obs = minimalObservation(sys_clock::now());
    obs.geom_minus_baro_ft = -175;
    rec.writeObservation(obs);
    auto plain = minimalObservation(sys_clock::now());
    rec.writeObservation(plain);
    rec.flush();
    const auto lines = readLines(rec.currentPath("jsonl"));
    BOOST_REQUIRE(lines.size() == 2u);
    auto with = skycued::JsonObject::parse(lines[0]);
    auto without = skycued::JsonObject::parse(lines[1]);
    BOOST_REQUIRE(with && without);
    BOOST_TEST(with->getDouble("geom_minus_baro_ft").value_or(0) == -175.0);
    BOOST_TEST(!without->has("geom_minus_baro_ft"));  // an old file looks like this one
    const auto back = skycued::recording::observationFromRecord(*with);
    BOOST_REQUIRE(back);
    BOOST_TEST((back->geom_minus_baro_ft == std::optional<int32_t>(-175)));
    const auto old = skycued::recording::observationFromRecord(*without);
    BOOST_REQUIRE(old);
    BOOST_TEST(!old->geom_minus_baro_ft.has_value());
    BOOST_TEST((old->alt_baro_ft == plain.alt_baro_ft));
    // A file with the key replays to the same record it was written from.
    BOOST_TEST(skycued::formatIso8601Ms(back->received_time) == skycued::formatIso8601Ms(obs.received_time));
}

BOOST_AUTO_TEST_CASE(replay_reproduces_estimates)
{
    // Live: a baro carried 40 s (the decoder's measurement time) beside a fresh difference.
    // The recording keeps the ages; the replay restores the times, so resolveGeomAltitude
    // gives the same source and value as the live observation did.
    TempDir tmp;
    const auto t0 = roundMs(sys_clock::now());
    TrackObservation o = minimalObservation(t0);
    o.alt_baro_ft = 30000;
    o.alt_baro_time = t0 - 40s;
    o.geom_minus_baro_ft = 500;
    o.geom_minus_baro_time = t0 - 20s;
    o.alt_geom_ft = 30500;
    o.alt_geom_time = t0 - 20s;
    const std::string path = recordObservations(tmp.path, {o});
    {
        std::ifstream in(path);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        BOOST_TEST(text.find("\"alt_baro_age_s\":40") != std::string::npos);
        BOOST_TEST(text.find("\"geom_minus_baro_age_s\":20") != std::string::npos);
    }
    boost::asio::io_context io;
    ReplaySource replay(io, path, 0);
    std::vector<TrackObservation> got;
    replay.onObservation([&](const TrackObservation& x) { got.push_back(x); });
    replay.start();
    io.run();
    BOOST_REQUIRE_EQUAL(got.size(), 1u);
    BOOST_REQUIRE(got[0].alt_baro_time && got[0].alt_geom_time && got[0].geom_minus_baro_time);
    BOOST_TEST((*got[0].alt_baro_time == t0 - 40s));
    BOOST_TEST((*got[0].alt_geom_time == t0 - 20s));
    BOOST_TEST((*got[0].geom_minus_baro_time == t0 - 20s));

    const auto resolve = [&](const TrackObservation& x) {
        skycued::TrackTable table;
        skycued::TrackState& st = table.apply(x);
        skycued::learnGeomDiff(st.geom, x);
        return skycued::resolveGeomAltitude(st, x.received_time, std::nullopt);
    };
    const auto live = resolve(o), replayed = resolve(got[0]);
    BOOST_TEST((live.source == replayed.source));
    BOOST_TEST((live.alt_geom_ft == replayed.alt_geom_ft));
}

BOOST_AUTO_TEST_CASE(csv_headers_unchanged)
{
    BOOST_TEST(skycued::recording::receivedCsvHeader() == kReceivedHeader);
    BOOST_TEST(skycued::recording::sentCsvHeader() == kSentHeader);
    BOOST_TEST(kReceivedHeader.find("geom_minus_baro_ft") == std::string::npos);
    BOOST_TEST(kSentHeader.find("alt_geom_source") == std::string::npos);
    BOOST_TEST(kSentHeader.find("alt_geom_uncertainty_ft") == std::string::npos);
}

BOOST_AUTO_TEST_CASE(doc_lists_every_key)
{
    std::ifstream in(std::string(SKYCUE_DOCS_DIR) + "/recording-format.md");
    BOOST_REQUIRE(in);
    std::string doc((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    for (const auto& key : skycued::recording::observationKeys())
        BOOST_TEST(doc.find("| `" + key + "` |") != std::string::npos, key);
    for (const auto& key : skycued::recording::sentKeys())
        BOOST_TEST(doc.find("| `" + key + "` |") != std::string::npos, key);
    BOOST_TEST(doc.find(kReceivedHeader) != std::string::npos);
    BOOST_TEST(doc.find(kSentHeader) != std::string::npos);
}

BOOST_AUTO_TEST_CASE(write_failure_does_not_spawn_files)
{
    TempDir tmp;
    Recorder rec(RecorderConfig{tmp.path, true, true, 1ull << 30, 3600s}, "received");
    rec.writeObservation(minimalObservation(sys_clock::now()));
    rec.flush();
    const std::string first = rec.currentPath("jsonl");

    rec.setFailWritesForTest(true);
    const auto failed_at = steady::now();
    for (int i = 0; i < 500; ++i) {
        rec.writeObservation(minimalObservation(sys_clock::now()));
        if (i % 50 == 0)
            rec.flush();
        std::this_thread::sleep_for(2ms);
    }
    BOOST_TEST(rec.droppedRecords() == 500u);
    BOOST_TEST(filesWithSuffix(tmp.path, ".jsonl").size() <= 2u);
    BOOST_TEST(filesWithSuffix(tmp.path, ".csv").size() <= 2u);
    BOOST_TEST(readLines(first).size() == 1u);

    // Healthy again, but no new files before the reopen interval has passed.
    rec.setFailWritesForTest(false);
    if (steady::now() - failed_at < Recorder::kReopenInterval - 500ms) {
        rec.writeObservation(minimalObservation(sys_clock::now()));
        BOOST_TEST(rec.droppedRecords() == 501u);
    }
    std::this_thread::sleep_until(failed_at + Recorder::kReopenInterval + 100ms);
    const auto before = rec.droppedRecords();
    rec.writeObservation(minimalObservation(sys_clock::now()));
    BOOST_TEST(rec.droppedRecords() == before);
    const std::string recovered = rec.currentPath("jsonl");
    BOOST_TEST(!recovered.empty());
    BOOST_TEST(recovered != first);
    BOOST_TEST(readLines(recovered).size() == 1u);  // flushed at once while recovering
    BOOST_TEST(filesWithSuffix(tmp.path, ".jsonl").size() == 2u);
    BOOST_TEST(filesWithSuffix(tmp.path, ".csv").size() == 2u);
}

BOOST_AUTO_TEST_CASE(write_failure_backs_off_and_reuses_name)
{
    TempDir tmp;
    RecorderConfig cfg{tmp.path, true, true, 1ull << 30, 3600s};
    cfg.reopen_initial = 40ms;
    cfg.reopen_max = 200ms;
    Recorder rec(cfg, "received");
    rec.writeObservation(minimalObservation(sys_clock::now()));
    rec.flush();
    const std::string first = rec.currentPath("jsonl");
    BOOST_TEST(rec.reopenDelay().count() == 0);

    // A persistent failure: every reopen fails again. Note each change of
    // the reopen delay and when it happened (= when a reopen failed).
    rec.setFailWritesForTest(true);
    std::vector<std::pair<steady::time_point, std::chrono::milliseconds>> changes;
    const auto until = steady::now() + 1200ms;
    while (steady::now() < until) {
        rec.writeObservation(minimalObservation(sys_clock::now()));
        if (changes.empty() || rec.reopenDelay() != changes.back().second)
            changes.emplace_back(steady::now(), rec.reopenDelay());
        std::this_thread::sleep_for(1ms);
    }
    BOOST_REQUIRE(changes.size() == 4u);  // 40, 80, 160, then capped at 200
    BOOST_TEST(changes[0].second.count() == 40);
    BOOST_TEST(changes[1].second.count() == 80);
    BOOST_TEST(changes[2].second.count() == 160);
    BOOST_TEST(changes[3].second.count() == 200);
    for (std::size_t i = 1; i < changes.size(); ++i) {
        const auto gap = std::chrono::duration_cast<std::chrono::milliseconds>(changes[i].first - changes[i - 1].first);
        BOOST_CHECK_MESSAGE(gap.count() >= changes[i - 1].second.count() - 5,
                            "attempt " << i << " after " << gap.count() << " ms");
    }

    // Reopens at 40+80+160+200+200... ms: several, yet one extra pair only.
    const auto jsonl = filesWithSuffix(tmp.path, ".jsonl");
    BOOST_REQUIRE(jsonl.size() == 2u);
    BOOST_TEST(filesWithSuffix(tmp.path, ".csv").size() == 2u);
    const std::string second = jsonl[0] == first ? jsonl[1] : jsonl[0];
    BOOST_TEST(readLines(first).size() == 1u);

    // Recovery: the next due reopen appends to the streak's pair (the CSV
    // header is not repeated) and resets the backoff.
    rec.setFailWritesForTest(false);
    std::this_thread::sleep_for(250ms);
    rec.writeObservation(minimalObservation(sys_clock::now()));
    BOOST_TEST(rec.reopenDelay().count() == 0);
    BOOST_TEST(rec.currentPath("jsonl") == second);
    rec.writeObservation(minimalObservation(sys_clock::now()));
    rec.flush();
    BOOST_TEST(readLines(second).size() == 2u);
    const std::string second_csv = second.substr(0, second.size() - 6) + ".csv";
    const auto csv_lines = readLines(second_csv);
    BOOST_REQUIRE(csv_lines.size() == 3u);
    BOOST_TEST(csv_lines[0] == kReceivedHeader);
    BOOST_TEST(csv_lines[1] != kReceivedHeader);

    // A new streak starts again from the initial delay.
    rec.setFailWritesForTest(true);
    rec.writeObservation(minimalObservation(sys_clock::now()));
    BOOST_TEST(rec.reopenDelay().count() == 40);
}

BOOST_AUTO_TEST_CASE(non_finite_numbers_are_left_out)
{
    TempDir tmp;
    Recorder rec(RecorderConfig{tmp.path, true, true, 1ull << 20, 3600s}, "received");
    auto obs = minimalObservation(sys_clock::now());
    obs.rssi = std::numeric_limits<double>::quiet_NaN();
    obs.gs_kt = std::numeric_limits<float>::infinity();
    obs.track_deg = 12.5f;
    rec.writeObservation(obs);
    rec.flush();
    auto row = splitCsv(readLines(rec.currentPath("csv")).at(1));
    BOOST_TEST(row[column(kReceivedHeader, "rssi")] == "");
    BOOST_TEST(row[column(kReceivedHeader, "gs_kt")] == "");
    BOOST_TEST(row[column(kReceivedHeader, "track_deg")] == "12.5");
    auto line = readLines(rec.currentPath("jsonl")).at(0);
    BOOST_TEST(line.find("null") == std::string::npos);
    auto j = skycued::JsonObject::parse(line);
    BOOST_REQUIRE(j);
    BOOST_TEST(!j->has("rssi"));
    BOOST_TEST(!j->has("gs_kt"));
}

BOOST_AUTO_TEST_CASE(write_track_rejects_unknown_event)
{
    TempDir tmp;
    Recorder sent(RecorderConfig{tmp.path, true, true, 1ull << 20, 3600s}, "sent");
    skycue::Track t;
    BOOST_CHECK_THROW(sent.writeTrack(t, "drop"), std::invalid_argument);
    BOOST_CHECK_THROW(sent.writeTrack(t, nullptr), std::invalid_argument);
    sent.writeTrack(t, "update");
    sent.flush();
    BOOST_TEST(readLines(sent.currentPath("jsonl")).size() == 1u);
}

BOOST_AUTO_TEST_CASE(replay_loop_shift_uses_full_time_range)
{
    TempDir tmp;
    const auto t0 = roundMs(sys_clock::now());
    std::vector<TrackObservation> recorded;
    for (int s : {0, 1, 2, 10, 3})  // out of order: the last record is not the latest
        recorded.push_back(minimalObservation(t0 + std::chrono::seconds(s)));
    const std::string path = recordObservations(tmp.path, recorded);
    boost::asio::io_context io;
    ReplaySource replay(io, path, 0, true);
    std::vector<TrackObservation> got;
    replay.onObservation([&](const TrackObservation& o) {
        got.push_back(o);
        if (got.size() == 6)
            io.stop();
    });
    replay.start();
    io.run();
    BOOST_REQUIRE(got.size() == 6u);
    // Span max - min = 10 s, median gap 1 s.
    BOOST_TEST((got[5].received_time == t0 + 11s));
    BOOST_TEST((got[5].received_time > got[3].received_time));
}

namespace {
TrackObservation phase6Observation()
{
    auto o = minimalObservation(sys_clock::now());
    o.heading_mag_deg = 91.5f;
    o.heading_true_deg = 93.25f;
    o.ias_kt = 250;
    o.tas_kt = 310.5f;
    o.mach = 0.78f;
    o.roll_deg = -12.5f;
    o.track_rate_dps = 1.5f;
    o.sel_alt_ft = 35000;
    o.sel_alt_fms_ft = 34000;
    o.qnh_mb = 1013.6f;
    o.sel_heading_deg = 270.0f;
    o.nav_modes = 0x15;  // autopilot, althold, lnav
    o.acas_sl = 4;
    o.acas_ri = 3;
    skycued::AcasRaState ra;
    ra.active = true;
    ra.ara = 0x2000;
    ra.rac = 0x5;
    ra.rat = true;
    ra.mte = false;
    ra.tti = 1;
    ra.threat_address = 0xa1b2c3;
    ra.text = "climb";
    ra.at = o.received_time - 4500ms;
    o.acas_ra = ra;
    return o;
}
}  // namespace

BOOST_AUTO_TEST_CASE(recorder_jsonl_phase6_keys)
{
    TempDir tmp;
    const auto obs = phase6Observation();
    const auto lines = readLines(recordObservations(tmp.path, {obs}));
    BOOST_REQUIRE(lines.size() == 1u);
    auto j = skycued::JsonObject::parse(lines[0]);
    BOOST_REQUIRE(j);
    BOOST_TEST(*j->getDouble("heading_mag_deg") == 91.5);
    BOOST_TEST(*j->getDouble("heading_true_deg") == 93.25);
    BOOST_TEST(*j->getInt("ias_kt") == 250);
    BOOST_TEST(*j->getDouble("tas_kt") == 310.5);
    BOOST_TEST(*j->getDouble("mach") > 0.77);
    BOOST_TEST(*j->getDouble("roll_deg") == -12.5);
    BOOST_TEST(*j->getDouble("track_rate_dps") == 1.5);
    BOOST_TEST(*j->getInt("sel_alt_ft") == 35000);
    BOOST_TEST(*j->getInt("sel_alt_fms_ft") == 34000);
    BOOST_TEST(*j->getDouble("sel_heading_deg") == 270.0);
    BOOST_TEST(*j->getInt("acas_sl") == 4);
    BOOST_TEST(*j->getInt("acas_ri") == 3);
    auto modes = j->getStringArray("nav_modes");
    BOOST_REQUIRE(modes);
    BOOST_TEST((*modes == std::vector<std::string>{"autopilot", "althold", "lnav"}), "modes");
    auto ra = j->getObject("acas_ra");
    BOOST_REQUIRE(ra);
    BOOST_TEST(*ra->getBool("active") == true);
    BOOST_TEST(*ra->getInt("ara") == 0x2000);
    BOOST_TEST(*ra->getBool("rat") == true);
    BOOST_TEST(*ra->getString("text") == "climb");
    auto th = ra->getObject("threat");
    BOOST_REQUIRE(th);
    BOOST_TEST(*th->getString("type") == "address");
    BOOST_TEST(*th->getString("address") == "a1b2c3");
    BOOST_TEST(*j->getDouble("acas_ra_age_s") == 4.5);
}

BOOST_AUTO_TEST_CASE(received_jsonl_round_trips_new_fields)
{
    TempDir tmp;
    const auto obs = phase6Observation();
    const auto lines = readLines(recordObservations(tmp.path, {obs}));
    BOOST_REQUIRE(lines.size() == 1u);
    auto j = skycued::JsonObject::parse(lines[0]);
    BOOST_REQUIRE(j);
    auto back = skycued::recording::observationFromRecord(*j);
    BOOST_REQUIRE(back);
    BOOST_TEST(*back->heading_mag_deg == 91.5f);
    BOOST_TEST(*back->heading_true_deg == 93.25f);
    BOOST_TEST(*back->ias_kt == 250);
    BOOST_TEST(*back->tas_kt == 310.5f);
    BOOST_TEST(*back->mach == 0.78f);
    BOOST_TEST(*back->roll_deg == -12.5f);
    BOOST_TEST(*back->track_rate_dps == 1.5f);
    BOOST_TEST(*back->sel_alt_ft == 35000);
    BOOST_TEST(*back->sel_alt_fms_ft == 34000);
    BOOST_TEST(*back->qnh_mb == 1013.6f);
    BOOST_TEST(*back->sel_heading_deg == 270.0f);
    BOOST_TEST(*back->nav_modes == 0x15);
    BOOST_TEST(*back->acas_sl == 4);
    BOOST_TEST(*back->acas_ri == 3);
    BOOST_REQUIRE(back->acas_ra);
    BOOST_TEST(back->acas_ra->active);
    BOOST_TEST(back->acas_ra->ara == 0x2000);
    BOOST_TEST(back->acas_ra->rac == 0x5);
    BOOST_TEST(back->acas_ra->rat);
    BOOST_TEST(!back->acas_ra->mte);
    BOOST_TEST(back->acas_ra->tti == 1);
    BOOST_TEST(*back->acas_ra->threat_address == 0xa1b2c3u);
    BOOST_TEST(back->acas_ra->text == "climb");
    BOOST_TEST(std::chrono::duration<double>(obs.received_time - back->acas_ra->at).count() == 4.5, boost::test_tools::tolerance(0.01));

    // Position-type threat.
    auto o2 = phase6Observation();
    o2.acas_ra->tti = 2;
    o2.acas_ra->threat_address.reset();
    o2.acas_ra->threat_alt_ft = 12300;
    o2.acas_ra->threat_range_nm = 3.5f;
    o2.acas_ra->threat_bearing_deg = 45.0f;
    TempDir tmp2;
    auto l2 = readLines(recordObservations(tmp2.path, {o2}));
    auto j2 = skycued::JsonObject::parse(l2.at(0));
    BOOST_REQUIRE(j2);
    BOOST_TEST(*j2->getObject("acas_ra")->getObject("threat")->getString("type") == "position");
    auto b2 = skycued::recording::observationFromRecord(*j2);
    BOOST_REQUIRE(b2 && b2->acas_ra);
    BOOST_TEST(b2->acas_ra->tti == 2);
    BOOST_TEST(*b2->acas_ra->threat_alt_ft == 12300);
    BOOST_TEST(*b2->acas_ra->threat_range_nm == 3.5f);
    BOOST_TEST(*b2->acas_ra->threat_bearing_deg == 45.0f);
}
