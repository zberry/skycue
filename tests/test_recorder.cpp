#define BOOST_TEST_MODULE skycued_recorder
#include <boost/test/unit_test.hpp>

#include "json_lite.hpp"
#include "readsb_json_source.hpp"
#include "recorder.hpp"
#include "replay_source.hpp"
#include "time_util.hpp"

#include <skycue/types.hpp>

#include <boost/asio/io_context.hpp>

#include <dirent.h>
#include <ftw.h>
#include <sys/stat.h>
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
using skycued::Recorder;
using skycued::RecorderConfig;
using skycued::ReplaySource;
using skycued::TrackObservation;
using sys_clock = std::chrono::system_clock;
using steady = std::chrono::steady_clock;

namespace {

const std::string kReceivedHeader =
    "time,type,hex,addr_type,source,pos_time,lat,lon,on_ground,alt_baro_ft,alt_geom_ft,gs_kt,track_deg,"
    "baro_rate_fpm,geom_rate_fpm,callsign,squawk,registration,nic,nac_p,rssi,uplink_hex";
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
            BOOST_TEST(b.on_ground == a.on_ground);
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
    BOOST_TEST(got[0].on_ground);
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
