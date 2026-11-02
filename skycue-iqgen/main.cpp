// skycue-iqgen: synthesises CU8 IQ recordings (what an RTL-SDR would have
// captured) from recorded Mode S and UAT frames, so the decoders can be fed
// a known input; also resamples CU8 and prints its statistics.
//
//   modes IN... -o OUT.cu8   1090 MHz PPM at 2 400 000 S/s
//   uat IN... -o OUT.cu8     978 MHz CPFSK at 2 083 333 S/s
//   resample IN IN_RATE -o OUT.cu8 [--out-rate R]
//   info IN [--rate R]
//
// Next to OUT.cu8 the generators write OUT.frames.txt: one line per frame,
// "<output sample offset> <hex> <expected|optional>[ overlap]".

#include "bits.hpp"
#include "inputs.hpp"
#include "iq_writer.hpp"
#include "modes_crc.hpp"
#include "placement.hpp"
#include "resample.hpp"
#include "synth.hpp"
#include "uat_frame.hpp"

#include <boost/program_options.hpp>

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace po = boost::program_options;

namespace {

constexpr const char* kUsage =
    "usage: skycue-iqgen modes IN... -o OUT.cu8 [--spacing-us 2000] [options]\n"
    "       skycue-iqgen uat IN... -o OUT.cu8 [--spacing-us 20000] [options]\n"
    "       skycue-iqgen resample IN.cu8|- IN_RATE -o OUT.cu8 [--out-rate 2400000]\n"
    "       skycue-iqgen info IN.cu8|- [--rate 2400000]\n";

constexpr const char* kOverview =
    "Synthesises CU8 IQ (unsigned 8-bit I/Q, 128 = zero) from recorded frames:\n"
    "  modes  1090 MHz Mode S at 2 400 000 S/s from .bin (Beast), .csv (epoch,HEX)\n"
    "         and .txt/.avr (AVR) captures\n"
    "  uat    978 MHz UAT at 2 083 333 S/s from dump978 raw lines (.txt/.log)\n"
    "  resample  converts a CU8 file between sample rates\n"
    "  info   prints a CU8 file's sample count, duration, magnitude and DC\n"
    "Inputs may be gzip-compressed (.gz after the suffix). The generators also\n"
    "write OUT.frames.txt listing every frame: '<sample> <hex> <expected|optional>[ overlap]'.\n"
    "'skycue-iqgen COMMAND --help' lists a command's options.\n";

constexpr std::size_t kChunk = 65536;  // output samples per write

int usageError(const std::string& msg)
{
    std::cerr << "skycue-iqgen: " << msg << "\n";
    return 2;
}

int ioError(const std::string& msg)
{
    std::cerr << "skycue-iqgen: " << msg << "\n";
    return 1;
}

std::string fmt3(double v)
{
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.3f", v);
    return buf;
}

// An output file; discard() removes it only when this run created it as a
// regular file (never a device, a pipe or a file that was already there).
class OutFile {
public:
    explicit OutFile(std::string path) : path_(std::move(path)) {}

    bool open()
    {
        struct stat st;
        const bool existed = ::stat(path_.c_str(), &st) == 0;
        errno = 0;
        os_.open(path_, std::ios::binary | std::ios::trunc);
        if (!os_)
            return false;
        created_ = !existed && ::stat(path_.c_str(), &st) == 0 && S_ISREG(st.st_mode);
        return true;
    }

    // Flushes and closes; false on failure.
    bool close()
    {
        errno = 0;
        os_.close();
        return !os_.fail();
    }

    void discard()
    {
        if (os_.is_open())
            os_.close();
        if (created_)
            ::unlink(path_.c_str());
        created_ = false;
    }

    std::ostream& stream() { return os_; }
    const std::string& path() const { return path_; }

private:
    std::string path_;
    std::ofstream os_;
    bool created_ = false;
};

std::string errnoText()
{
    return errno ? std::strerror(errno) : "I/O error";
}

// --snr: a number of dB or "none".
bool parseSnr(const std::string& s, std::optional<double>& out)
{
    if (s == "none") {
        out = std::nullopt;
        return true;
    }
    if (s.empty())
        return false;
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (*end != '\0' || !std::isfinite(v))
        return false;
    out = v;
    return true;
}

std::string sidecarPath(const std::string& out)
{
    std::string stem = out;
    if (stem.size() > 4 && stem.compare(stem.size() - 4, 4, ".cu8") == 0)
        stem.erase(stem.size() - 4);
    return stem + ".frames.txt";
}

void warnClipped(uint64_t clipped)
{
    if (clipped)
        std::cerr << "skycue-iqgen: warning: " << clipped
                  << " I/Q values clipped to 0..255 (lower --level or raise --snr)\n";
}

// ---- modes / uat -------------------------------------------------------------

int generate(bool uat, int argc, char** argv)
{
    std::vector<std::string> inputs;
    std::string out_path, snr_arg;
    double spacing_us = 0, level = -12, cfo = 0, pad_ms = 50, duration_s = 0;
    uint64_t seed = 1, max_mb = 2048;
    std::size_t limit = 0;
    po::options_description desc(std::string(kUsage) + (uat ? "uat: 978 MHz UAT from dump978 raw lines (.txt/.log)"
                                                             : "modes: 1090 MHz Mode S from .bin/.csv/.txt/.avr"));
    desc.add_options()("help,h", "show this help")("output,o", po::value(&out_path), "output CU8 file")(
        "spacing-us", po::value(&spacing_us)->default_value(uat ? 20000 : 2000),
        "gap between untimed frames and between input files, in us")(
        "level", po::value(&level)->default_value(-12), "signal level in dB full scale")(
        "snr", po::value(&snr_arg)->default_value("25"), "signal-to-noise ratio in dB, or 'none' for no noise")(
        "cfo", po::value(&cfo)->default_value(0), "carrier frequency offset in Hz")(
        "seed", po::value(&seed)->default_value(1), "noise generator seed")(
        "limit", po::value(&limit), "keep only the first N frames")(
        "duration", po::value(&duration_s), "keep only frames starting in the first S seconds")(
        "pad-ms", po::value(&pad_ms)->default_value(50), "silence before the first and after the last frame, in ms")(
        "max-mb", po::value(&max_mb)->default_value(2048), "refuse to write more than this many MiB");
    po::options_description hidden;
    hidden.add_options()("input", po::value(&inputs));
    po::options_description all;
    all.add(desc).add(hidden);
    po::positional_options_description pos;
    pos.add("input", -1);
    po::variables_map vm;
    try {
        po::store(po::command_line_parser(argc, argv).options(all).positional(pos).run(), vm);
        po::notify(vm);
    } catch (const std::exception& e) {
        return usageError(e.what());
    }
    if (vm.count("help")) {
        std::cout << desc << "\n";
        return 0;
    }
    std::optional<double> snr;
    if (!parseSnr(snr_arg, snr))
        return usageError("--snr must be a number of dB or 'none', got '" + snr_arg + "'");
    if (inputs.empty())
        return usageError("no input files");
    if (out_path.empty())
        return usageError("-o OUT.cu8 is required");
    const double rate = uat ? iqgen::kUatRate : iqgen::kModesRate;
    if (!std::isfinite(spacing_us) || spacing_us <= 0)
        return usageError("--spacing-us must be positive");
    if (!std::isfinite(level))
        return usageError("--level must be a number");
    if (!std::isfinite(cfo) || std::fabs(cfo) >= rate / 2)
        return usageError("--cfo must be below half the sample rate (" + fmt3(rate / 2) + " Hz)");
    if (!std::isfinite(pad_ms) || pad_ms < 0)
        return usageError("--pad-ms must be 0 or more");
    if (vm.count("limit") && limit == 0)
        return usageError("--limit must be at least 1");
    if (vm.count("duration") && (!std::isfinite(duration_s) || duration_s <= 0))
        return usageError("--duration must be positive");
    if (max_mb == 0 || max_mb > (uint64_t{1} << 40))
        return usageError("--max-mb must be 1 to 2^40");

    // Read every input.
    iqgen::InputStats stats;
    std::vector<std::vector<iqgen::TimedFrame>> files;
    for (const auto& in : inputs) {
        try {
            files.push_back(uat ? iqgen::readUatFile(in, stats) : iqgen::readModesFile(in, stats));
        } catch (const iqgen::InputError& e) {
            return usageError(e.what());
        } catch (const std::exception& e) {
            return ioError(e.what());
        }
    }

    // Place them: Mode S on the 12 MHz tick grid, UAT on output samples.
    iqgen::PlaceOptions opt;
    if (uat) {
        opt.pad = std::llround(pad_ms * 2083.333);
        opt.spacing = std::llround(spacing_us * 2.083333);
        opt.grid_per_time = 2083333e-9;
        if (vm.count("duration"))
            opt.duration = std::llround(duration_s * iqgen::kUatRate);
        opt.length = [](const std::vector<uint8_t>& p) {
            return static_cast<int64_t>(iqgen::uatFrameSamples(*iqgen::uatKindForPayload(p.size())));
        };
    } else {
        opt.pad = std::llround(pad_ms * 12000.0);
        opt.spacing = std::llround(spacing_us * 12.0);
        opt.grid_per_time = 1.0;
        if (vm.count("duration"))
            opt.duration = std::llround(duration_s * iqgen::kModesTickRate);
        opt.length = [](const std::vector<uint8_t>& p) {
            return static_cast<int64_t>(iqgen::modesFrameTicks(p.size()));
        };
    }
    if (vm.count("limit"))
        opt.limit = limit;
    const iqgen::Placement placed = iqgen::place(std::move(files), opt);
    if (placed.frames.empty())
        return usageError("no frames in the input (" + std::to_string(stats.skipped) + " lines skipped)");

    const int64_t per = uat ? 1 : iqgen::kModesTicksPerSample;
    const auto out_samples = static_cast<uint64_t>((placed.end + per - 1) / per);
    const uint64_t bytes = 2 * out_samples;
    if (bytes > (max_mb << 20))
        return usageError(out_path + " would be " + std::to_string((bytes + (1u << 20) - 1) >> 20) +
                          " MB (> --max-mb " + std::to_string(max_mb) + "); use --duration or --max-mb");

    // Synthesise, streaming.
    const double amp = iqgen::amplitudeForLevel(level);
    OutFile out(out_path);
    if (!out.open())
        return ioError("writing " + out_path + ": " + errnoText());
    uint64_t clipped = 0;
    try {
        iqgen::IqWriter w(out.stream(), rate, amp, snr, cfo, seed);
        std::vector<std::complex<float>> buf;
        buf.reserve(kChunk);
        if (uat) {
            iqgen::UatSynth synth(amp);
            auto advance = [&](int64_t sample) {
                while (synth.emittedSamples() < sample) {
                    buf.clear();
                    synth.emitUntil(std::min<int64_t>(sample, synth.emittedSamples() + static_cast<int64_t>(kChunk)),
                                    buf);
                    w.write(buf.data(), buf.size());
                }
            };
            for (const auto& f : placed.frames) {
                advance(f.start);
                synth.addFrame(f.start, iqgen::uatFrameBits(f.payload));
            }
            advance(static_cast<int64_t>(out_samples));
        } else {
            iqgen::ModesSynth synth(amp);
            auto advance = [&](int64_t tick) {
                while (synth.emittedTicks() + per <= tick) {
                    buf.clear();
                    synth.emitUntil(std::min<int64_t>(tick, synth.emittedTicks() + per * static_cast<int64_t>(kChunk)),
                                    buf);
                    w.write(buf.data(), buf.size());
                }
            };
            for (const auto& f : placed.frames) {
                advance(f.start);
                synth.addFrame(f.start, f.payload);
            }
            advance(static_cast<int64_t>(out_samples) * per);
        }
        clipped = w.clipped();
    } catch (const std::runtime_error&) {  // IqWriter: the stream failed
        const std::string why = errnoText();
        out.discard();
        return ioError("writing " + out_path + ": " + why);
    } catch (const std::exception& e) {  // a synthesiser precondition: a bug
        out.discard();
        return ioError(std::string("internal error: ") + e.what());
    }
    if (!out.close()) {
        const std::string why = errnoText();
        out.discard();
        return ioError("writing " + out_path + ": " + why);
    }

    // The sidecar, in start order.
    uint64_t expected = 0;
    OutFile side(sidecarPath(out_path));
    if (!side.open()) {
        const std::string why = errnoText();
        out.discard();
        return ioError("writing " + side.path() + ": " + why);
    }
    for (const auto& f : placed.frames) {
        const bool exp = !f.overlap && (uat || iqgen::modesClean(f.payload));
        expected += exp ? 1 : 0;
        side.stream() << f.start / per << ' ' << iqgen::toHex(f.payload, !uat) << ' '
                      << (exp ? "expected" : "optional") << (f.overlap ? " overlap" : "") << '\n';
    }
    if (!side.close()) {
        const std::string why = errnoText();
        side.discard();
        out.discard();
        return ioError("writing " + side.path() + ": " + why);
    }

    warnClipped(clipped);
    std::cout << "frames=" << placed.frames.size() << " expected=" << expected << " overlaps=" << placed.overlaps
              << " clipped=" << clipped << " skipped=" << stats.skipped
              << " seconds=" << fmt3(static_cast<double>(out_samples) / rate) << " bytes=" << bytes << "\n";
    return 0;
}

// ---- resample ----------------------------------------------------------------

int resample(int argc, char** argv)
{
    std::string in_path, out_path;
    int64_t in_rate = 0, out_rate = 2400000;
    po::options_description desc(
        std::string(kUsage) +
        "resample: converts CU8 from IN_RATE to --out-rate (IN may be '-' for stdin).\n"
        "The rates' ratio must reduce to factors of at most 1000 (2083333 -> 2400000 does not).\n"
        "The polyphase filter has 32 taps per phase whatever the ratio, so for large\n"
        "down-sampling factors (beyond a few times) its stop band degrades: resample in steps");
    desc.add_options()("help,h", "show this help")("output,o", po::value(&out_path), "output CU8 file")(
        "out-rate", po::value(&out_rate)->default_value(2400000), "output sample rate in S/s");
    po::options_description hidden;
    hidden.add_options()("in", po::value(&in_path))("in-rate", po::value(&in_rate));
    po::options_description all;
    all.add(desc).add(hidden);
    po::positional_options_description pos;
    pos.add("in", 1).add("in-rate", 1);
    po::variables_map vm;
    try {
        po::store(po::command_line_parser(argc, argv).options(all).positional(pos).run(), vm);
        po::notify(vm);
    } catch (const std::exception& e) {
        return usageError(e.what());
    }
    if (vm.count("help")) {
        std::cout << desc << "\n";
        return 0;
    }
    if (in_path.empty() || !vm.count("in-rate"))
        return usageError("resample needs IN and IN_RATE");
    if (out_path.empty())
        return usageError("-o OUT.cu8 is required");
    if (in_rate <= 0 || out_rate <= 0)
        return usageError("sample rates must be positive");
    const auto g = std::gcd(static_cast<uint64_t>(in_rate), static_cast<uint64_t>(out_rate));
    const uint64_t up = static_cast<uint64_t>(out_rate) / g, down = static_cast<uint64_t>(in_rate) / g;
    if (up > iqgen::kResampleMaxFactor || down > iqgen::kResampleMaxFactor)
        return usageError("cannot resample from " + std::to_string(in_rate) + " to " + std::to_string(out_rate) +
                          " S/s: the ratio reduces to " + std::to_string(up) + "/" + std::to_string(down) +
                          ", above the limit of " + std::to_string(iqgen::kResampleMaxFactor));

    std::ifstream file;
    std::istream* in = &std::cin;
    if (in_path != "-") {
        file.open(in_path, std::ios::binary);
        if (!file)
            return usageError("cannot open " + in_path + ": " + errnoText());
        in = &file;
    }
    OutFile out(out_path);
    if (!out.open())
        return ioError("writing " + out_path + ": " + errnoText());
    iqgen::ResampleCounts n{0, 0, 0};
    try {
        n = iqgen::resampleCu8(*in, static_cast<uint64_t>(in_rate), static_cast<uint64_t>(out_rate), out.stream());
    } catch (const std::runtime_error&) {
        const std::string why = errnoText();
        out.discard();
        return ioError("writing " + out_path + ": " + why);
    }
    if (!out.close()) {
        const std::string why = errnoText();
        out.discard();
        return ioError("writing " + out_path + ": " + why);
    }
    if (in->bad())
        return ioError("reading " + in_path + ": " + errnoText());
    warnClipped(n.clipped);
    std::cout << "in_samples=" << n.in_samples << " out_samples=" << n.out_samples << "\n";
    return 0;
}

// ---- info --------------------------------------------------------------------

int info(int argc, char** argv)
{
    std::string in_path;
    double rate = 2400000;
    po::options_description desc(std::string(kUsage) +
                                 "info: sample count, seconds, mean and peak |I,Q| and DC of a CU8 file "
                                 "(IN may be '-' for stdin)");
    desc.add_options()("help,h", "show this help")("rate", po::value(&rate)->default_value(2400000),
                                                     "sample rate in S/s");
    po::options_description hidden;
    hidden.add_options()("in", po::value(&in_path));
    po::options_description all;
    all.add(desc).add(hidden);
    po::positional_options_description pos;
    pos.add("in", 1);
    po::variables_map vm;
    try {
        po::store(po::command_line_parser(argc, argv).options(all).positional(pos).run(), vm);
        po::notify(vm);
    } catch (const std::exception& e) {
        return usageError(e.what());
    }
    if (vm.count("help")) {
        std::cout << desc << "\n";
        return 0;
    }
    if (in_path.empty())
        return usageError("info needs IN");
    if (!std::isfinite(rate) || rate <= 0)
        return usageError("--rate must be positive");
    std::ifstream file;
    std::istream* in = &std::cin;
    if (in_path != "-") {
        file.open(in_path, std::ios::binary);
        if (!file)
            return usageError("cannot open " + in_path + ": " + errnoText());
        in = &file;
    }
    const iqgen::Cu8Info i = iqgen::cu8Info(*in, rate);
    if (in->bad())
        return ioError("reading " + in_path + ": " + errnoText());
    std::cout << "samples=" << i.samples << " seconds=" << fmt3(i.seconds) << " mean_mag=" << fmt3(i.mean_mag)
              << " peak_mag=" << fmt3(i.peak_mag) << " dc_i=" << fmt3(i.dc_i) << " dc_q=" << fmt3(i.dc_q) << "\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    std::ios::sync_with_stdio(false);
    if (argc < 2) {
        std::cerr << "skycue-iqgen: no command\n" << kUsage;
        return 2;
    }
    const std::string cmd = argv[1];
    if (cmd == "--help" || cmd == "-h" || cmd == "help") {
        std::cout << kUsage << kOverview;
        return 0;
    }
    // Each command parses argv[1..] (its own name standing in for argv[0]).
    try {
        if (cmd == "modes")
            return generate(false, argc - 1, argv + 1);
        if (cmd == "uat")
            return generate(true, argc - 1, argv + 1);
        if (cmd == "resample")
            return resample(argc - 1, argv + 1);
        if (cmd == "info")
            return info(argc - 1, argv + 1);
    } catch (const std::exception& e) {
        return ioError(e.what());
    }
    std::cerr << "skycue-iqgen: unknown command '" << cmd << "'\n" << kUsage;
    return 2;
}
