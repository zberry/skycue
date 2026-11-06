// skycue-stats: summarises recordings for checking receiver coverage and the
// filter / rate-limit chain.
//
//   skycue-stats [--lat L --lon L | --config skycue.conf] [--json] [--top N]
//                [--range-bin NM] [--alt-bin FT] [--gap-s S] FILE...
//
// FILE: a received-stream or sent-stream JSONL recording, or a capture of
// readsb's JSON port, any of them optionally gzip-compressed; each line is
// recognised on its own. Several files are merged in time order. The report
// (text, or one JSON object with --json) goes to stdout; docs/operating.md
// describes it.
//
// Exit status: 0 success, 1 a file that cannot be read or a bad --config,
// 2 usage error.

#include "config.hpp"
#include "gz_line_reader.hpp"
#include "recording_reader.hpp"
#include "stats_report.hpp"

#include <boost/program_options.hpp>

#include <zlib.h>

#include <cerrno>
#include <cmath>
#include <cstring>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace po = boost::program_options;
using namespace skycued;

namespace {

// A plain or gzip file, line by line; a truncated gzip stream throws.
using Reader = GzLineReader;

const char* lineKindName(LineKind k)
{
    switch (k) {
    case LineKind::received: return "received";
    case LineKind::sent: return "sent";
    case LineKind::readsb_raw: return "readsb-raw";
    case LineKind::ignore:
    case LineKind::bad: break;
    }
    return "";
}

// Reads every record of `path` into `out`; returns what was found.
StatsInput readFile(const std::string& path, std::size_t index, std::vector<RecordingRecord>& out)
{
    StatsInput in;
    in.name = path;
    Reader reader(path);
    std::set<LineKind> kinds;
    std::string line;
    std::size_t line_no = 0, first_bad = 0;
    while (reader.getline(line)) {
        ++line_no;
        RecordingRecord rec;
        const LineKind kind = parseRecordingLine(line, rec);
        if (kind == LineKind::ignore)
            continue;
        if (kind == LineKind::bad) {
            if (in.skipped++ == 0)
                first_bad = line_no;
            continue;
        }
        kinds.insert(kind);
        rec.file = index;
        out.push_back(std::move(rec));
        ++in.records;
    }
    for (LineKind k : {LineKind::received, LineKind::sent, LineKind::readsb_raw})
        if (kinds.count(k))
            in.kind += (in.kind.empty() ? "" : "+") + std::string(lineKindName(k));
    if (in.kind.empty())
        in.kind = "empty";
    if (in.skipped)
        std::cerr << "skycue-stats: " << path << ": skipped " << in.skipped << " of " << line_no
                  << " lines that are not recording records (first: line " << first_bad << ")\n";
    return in;
}

int usage(const std::string& message, const po::options_description& opts)
{
    std::cerr << "skycue-stats: " << message << "\n\nusage: skycue-stats [options] FILE...\n" << opts;
    return 2;
}

}  // namespace

int main(int argc, char** argv)
{
    double lat = NAN, lon = NAN, range_bin = 10, gap_s = 5;
    int alt_bin = 5000;
    int top = 15;
    std::string config_path;
    std::vector<std::string> files;

    po::options_description opts("Options");
    opts.add_options()
        ("help,h", "show this help")
        ("lat", po::value(&lat), "receiver latitude, degrees (with --lon; overrides --config)")
        ("lon", po::value(&lon), "receiver longitude, degrees (with --lat)")
        ("config", po::value(&config_path), "skycue.conf to take the receiver position (and altitude) from")
        ("json", "write one JSON object instead of text")
        ("top", po::value(&top)->default_value(15), "rows in the per-aircraft tables")
        ("range-bin", po::value(&range_bin)->default_value(10), "range histogram bin, nm")
        ("alt-bin", po::value(&alt_bin)->default_value(5000), "altitude histogram bin, ft")
        ("gap-s", po::value(&gap_s)->default_value(5), "report gaps between records longer than this, s");
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
        std::cout << "usage: skycue-stats [options] FILE...\n"
                     "Summarises received/sent JSONL recordings and readsb JSON captures (.gz too).\n"
                     "The Aircraft section counts observations by source, by address type and by input\n"
                     "(\"By input\": the obs records' input id, \"(none)\" for records without one).\n"
                  << opts;
        return 0;
    }
    if (files.empty())
        return usage("no FILE given", opts);
    if (vm.count("lat") != vm.count("lon"))
        return usage("--lat and --lon go together", opts);
    if (vm.count("lat") && !(std::isfinite(lat) && std::fabs(lat) <= 90 && std::isfinite(lon) && std::fabs(lon) <= 180))
        return usage("--lat must be within +-90 and --lon within +-180", opts);
    if (top < 0)
        return usage("--top must be 0 or more", opts);
    if (!(range_bin > 0) || !std::isfinite(range_bin))
        return usage("--range-bin must be positive", opts);
    if (alt_bin <= 0)
        return usage("--alt-bin must be positive", opts);
    if (!(gap_s > 0) || !std::isfinite(gap_s))
        return usage("--gap-s must be positive", opts);

    StatsOptions options;
    options.top_n = static_cast<std::size_t>(top);
    options.range_bin_nm = range_bin;
    options.alt_bin_ft = alt_bin;
    options.gap_s = gap_s;
    if (!config_path.empty()) {
        try {
            const Config config = parseConfigFile(config_path);
            options.rx_alt_m = config.receiver_alt_m + config.antenna_height_m;
            if (std::isfinite(config.receiver_lat) && std::isfinite(config.receiver_lon)) {
                options.rx_lat = config.receiver_lat;
                options.rx_lon = config.receiver_lon;
                options.receiver_from = "--config";
            }
        } catch (const std::exception& e) {
            std::cerr << "skycue-stats: " << e.what() << "\n";
            return 1;
        }
    }
    if (vm.count("lat")) {
        options.rx_lat = lat;
        options.rx_lon = lon;
        options.receiver_from = "--lat/--lon";
    }

    std::vector<RecordingRecord> records;
    try {
        for (std::size_t i = 0; i < files.size(); ++i)
            options.inputs.push_back(readFile(files[i], i, records));
    } catch (const std::exception& e) {
        std::cerr << "skycue-stats: " << e.what() << "\n";
        return 1;
    }

    const StatsReport report = computeStats(records, options);
    if (vm.count("json"))
        std::cout << formatStatsJson(report) << "\n";
    else
        std::cout << formatStatsText(report);
    std::cout.flush();
    return std::cout ? 0 : 1;
}
