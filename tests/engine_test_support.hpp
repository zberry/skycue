#pragma once

// Fixtures shared by the Engine tests (test_engine, test_engine_api,
// test_recording_independence). Header-only; every function is inline.

#include "config.hpp"
#include "engine.hpp"
#include "forwarder.hpp"
#include "inputs/input.hpp"
#include "inputs/input_manager.hpp"
#include "inputs/source_input.hpp"
#include "observation.hpp"
#include "recorder.hpp"
#include "replay_source.hpp"
#include "test_slow.hpp"

#include <skycue/client.hpp>
#include <skycue/types.hpp>

#include <boost/asio.hpp>

#include <dirent.h>
#include <ftw.h>
#include <stdlib.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace enginetest {

using namespace std::chrono_literals;
using skycue::AddressType;
using skycue::DropReason;
using skycue::Track;
using skycue::TrackId;
using skycued::Config;
using skycued::Engine;
using skycued::Forwarder;
using skycued::ForwarderConfig;
using skycued::Input;
using skycued::InputDef;
using skycued::InputManager;
using skycued::InputType;
using skycued::ObservationSource;
using skycued::TrackObservation;
namespace asio = boost::asio;
using asio::ip::tcp;
using steady = std::chrono::steady_clock;

inline const std::chrono::milliseconds kWait = slow(5s);
inline const std::string kFixture = std::string(SKYCUE_FIXTURE_DIR) + "/readsb-sample.jsonl";

inline int removeEntry(const char* p, const struct stat*, int, struct FTW*) { return ::remove(p); }

struct TempDir {
    std::string path;
    TempDir()
    {
        const char* base = std::getenv("TMPDIR");
        std::string tmpl = std::string(base && *base ? base : "/tmp") + "/skycue-engine-XXXXXX";
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        if (!::mkdtemp(buf.data()))
            throw std::runtime_error("mkdtemp failed");
        path = buf.data();
    }
    ~TempDir() { ::nftw(path.c_str(), removeEntry, 16, FTW_DEPTH | FTW_PHYS); }
};

inline std::vector<std::string> filesWithSuffix(const std::string& dir, const std::string& suffix)
{
    std::vector<std::string> out;
    DIR* d = ::opendir(dir.c_str());
    if (!d)
        return out;
    while (dirent* e = ::readdir(d)) {
        const std::string name = e->d_name;
        if (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
            out.push_back(dir + "/" + name);
    }
    ::closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

inline std::string readAll(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

inline size_t countOf(const std::string& text, const std::string& needle)
{
    size_t n = 0;
    for (size_t pos = text.find(needle); pos != std::string::npos; pos = text.find(needle, pos + needle.size()))
        ++n;
    return n;
}

// The concatenated JSONL of one stream ("sent" or "received") in `dir`.
inline std::string streamText(const std::string& dir, const std::string& stream)
{
    std::string text;
    for (const auto& f : filesWithSuffix(dir, ".jsonl"))
        if (f.find("/" + stream + "-") != std::string::npos)
            text += readAll(f);
    return text;
}

// Runs an io_context on its own thread; call() runs a function there and
// waits for its result, so the engine is only ever touched on that thread.
struct IoThread {
    asio::io_context io;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> guard;
    std::thread thread;

    IoThread()
    {
        guard.emplace(asio::make_work_guard(io));
        thread = std::thread([this] { io.run(); });
    }
    ~IoThread() { join(); }

    void join()
    {
        guard.reset();
        io.stop();
        if (thread.joinable())
            thread.join();
    }

    template <class F> auto call(F f) -> decltype(f())
    {
        using R = decltype(f());
        auto task = std::make_shared<std::packaged_task<R()>>(std::move(f));
        auto fut = task->get_future();
        asio::post(io, [task] { (*task)(); });
        if (fut.wait_for(kWait) != std::future_status::ready)
            throw std::runtime_error("io thread call timed out");
        return fut.get();
    }
};

// Records every skycue::Client callback.
struct Events {
    std::mutex m;
    std::condition_variable cv;
    std::vector<Track> updates;
    std::vector<steady::time_point> update_times;  // when each of `updates` arrived
    std::vector<std::pair<TrackId, DropReason>> drops;
    std::vector<steady::time_point> drop_times;

    void attach(skycue::Client& c)
    {
        c.onTrackUpdate([this](const Track& tr) {
            std::lock_guard<std::mutex> lk(m);
            updates.push_back(tr);
            update_times.push_back(steady::now());
            cv.notify_all();
        });
        c.onTrackDropped([this](TrackId id, DropReason r) {
            std::lock_guard<std::mutex> lk(m);
            drops.emplace_back(id, r);
            drop_times.push_back(steady::now());
            cv.notify_all();
        });
    }

    template <class Pred> bool waitFor(Pred p, std::chrono::milliseconds timeout = kWait)
    {
        std::unique_lock<std::mutex> lk(m);
        return cv.wait_for(lk, timeout, p);
    }

    // Callers hold m.
    std::set<TrackId> ids() const
    {
        std::set<TrackId> s;
        for (const auto& u : updates)
            s.insert(u.id);
        return s;
    }
    bool dropped(TrackId id, DropReason r) const
    {
        return std::any_of(drops.begin(), drops.end(),
                           [&](const std::pair<TrackId, DropReason>& d) { return d.first == id && d.second == r; });
    }
    size_t dropsWith(DropReason r) const
    {
        return static_cast<size_t>(std::count_if(drops.begin(), drops.end(),
                                                 [&](const std::pair<TrackId, DropReason>& d) { return d.second == r; }));
    }
    size_t updatesFor(TrackId id) const
    {
        return static_cast<size_t>(
            std::count_if(updates.begin(), updates.end(), [&](const Track& t) { return t.id == id; }));
    }
};

inline skycue::Config clientConfig(uint16_t port)
{
    skycue::Config c;
    c.transport = skycue::Config::Transport::tcp;
    c.host = "127.0.0.1";
    c.port = port;
    c.reconnect_interval = 100ms;
    return c;
}

inline Config baseConfig()
{
    Config c;
    c.receiver_lat = 38.9;
    c.receiver_lon = -77.0;
    c.track_timeout_s = 5;
    return c;
}

// An ObservationSource driven by the test (on the io thread).
struct FakeSource : ObservationSource {
    std::function<void(const TrackObservation&)> on_obs;
    std::function<void(bool)> on_conn;
    bool started = false, stopped = false;

    void onObservation(std::function<void(const TrackObservation&)> h) override { on_obs = std::move(h); }
    void onConnection(std::function<void(bool)> h) override { on_conn = std::move(h); }
    void start() override { started = true; }
    void stop() override { stopped = true; }

    void emit(const TrackObservation& o)
    {
        if (on_obs && !stopped)
            on_obs(o);
    }
    void connection(bool up)
    {
        if (on_conn && !stopped)
            on_conn(up);
    }
};

// An InputManager (in-memory when inputs_file is empty) with one built-in
// SourceInput over `source`, loaded and not started (Engine::start does).
// The id "replay" makes it a replay input, any other a readsb one.
inline std::unique_ptr<InputManager> sourceInputs(asio::io_context& io, std::unique_ptr<ObservationSource> source,
                                                  const std::string& id = "readsb",
                                                  const std::string& inputs_file = "")
{
    skycued::InputManagerConfig mc;
    mc.inputs_file = inputs_file;
    auto inputs = std::make_unique<InputManager>(io, mc);
    InputDef def;
    def.id = id;
    def.type = id == "replay" ? InputType::replay : InputType::readsb;
    def.host = "127.0.0.1";
    def.port = 1;
    inputs->addBuiltin(std::make_unique<skycued::SourceInput>(def, std::move(source)));
    inputs->load();
    return inputs;
}

// An Input driven by the test (on the io thread). Whether it is a track input
// follows its def's type (dump978 is not). start() after stop() runs it again.
struct FakeInput : Input {
    bool running = false;
    int starts = 0;

    explicit FakeInput(InputDef def) : Input(std::move(def)) {}
    void start() override
    {
        running = true;
        ++starts;
    }
    void stop() override
    {
        running = false;
        markStopped();
    }

    void emit(TrackObservation o)
    {
        if (running) {
            noteData(0);
            emitObservation(std::move(o));
        }
    }
    void connection(bool up)
    {
        if (running) {
            if (up)
                ++counters_.connects;
            setConnected(up);
        }
    }
    void uplink(std::chrono::system_clock::time_point t, const std::string& hex)
    {
        if (running) {
            noteData(0);
            ++counters_.uplinks;
            emitUplink(t, hex);
        }
    }
};

inline InputDef fakeDef(const std::string& id, InputType type = InputType::readsb)
{
    InputDef d;
    d.id = id;
    d.type = type;
    d.host = "127.0.0.1";
    d.port = 1;
    return d;
}

// A factory for the multi-input EngineFx: an in-memory InputManager with a
// FakeInput per definition (as built-ins), each also stored in `out` by id.
inline std::function<std::unique_ptr<InputManager>(asio::io_context&)>
fakeInputs(std::vector<InputDef> defs, std::map<std::string, FakeInput*>& out)
{
    return [defs, &out](asio::io_context& io) {
        auto inputs = std::make_unique<InputManager>(io, skycued::InputManagerConfig{});
        for (const auto& d : defs) {
            auto in = std::make_unique<FakeInput>(d);
            out[d.id] = in.get();
            inputs->addBuiltin(std::move(in));
        }
        inputs->load();
        return inputs;
    };
}

inline TrackObservation nearbyObservation(uint32_t address, double lat_offset)
{
    TrackObservation o;
    o.id = TrackId{address, AddressType::icao};
    o.source = skycue::Source::adsb_1090;
    o.received_time = std::chrono::system_clock::now();
    o.position_time = o.received_time;
    o.lat = 38.95 + lat_offset;
    o.lon = -77.0;
    o.alt_baro_ft = 5000;
    o.gs_kt = 200;
    o.track_deg = 90;
    return o;
}

// Engine + Forwarder on an IoThread, torn down on that thread.
struct EngineFx {
    IoThread t;
    std::unique_ptr<Forwarder> fwd;
    std::unique_ptr<skycued::Recorder> received, sent;
    std::unique_ptr<Engine> engine;

    EngineFx(const Config& cfg, std::function<std::unique_ptr<ObservationSource>(asio::io_context&)> make_source,
             std::chrono::milliseconds keyframe_interval = 10s, const std::string& record_dir = "")
        : EngineFx(
              cfg, [make_source](asio::io_context& io) { return sourceInputs(io, make_source(io)); },
              keyframe_interval, record_dir)
    {
    }
    // Several inputs: `make_inputs` builds the manager (not started).
    EngineFx(const Config& cfg, std::function<std::unique_ptr<InputManager>(asio::io_context&)> make_inputs,
             std::chrono::milliseconds keyframe_interval = 10s, const std::string& record_dir = "")
    {
        t.call([&] {
            ForwarderConfig fc;
            fc.tcp_listen_ephemeral = true;
            fc.keyframe_interval = keyframe_interval;
            fwd = std::make_unique<Forwarder>(t.io, fc);
            if (!record_dir.empty()) {
                skycued::RecorderConfig rc;
                rc.dir = record_dir;
                rc.csv = false;
                received = std::make_unique<skycued::Recorder>(rc, "received");
                sent = std::make_unique<skycued::Recorder>(rc, "sent");
            }
            engine = std::make_unique<Engine>(t.io, cfg, make_inputs(t.io), *fwd, received.get(), sent.get());
            engine->start();
        });
    }
    ~EngineFx()
    {
        try {
            t.call([this] {
                engine.reset();
                fwd.reset();
                received.reset();
                sent.reset();
            });
        } catch (...) {
        }
        t.join();
    }
    uint16_t port() { return t.call([this] { return fwd->tcpPort(); }); }
    void stop()
    {
        t.call([this] { engine->stop(); });
    }
    skycued::EngineStats stats()
    {
        return t.call([this] { return engine->stats(); });
    }
};

// A factory for EngineFx that hands out a FakeSource and keeps a pointer to it.
inline std::function<std::unique_ptr<ObservationSource>(asio::io_context&)> fakeSource(FakeSource*& out)
{
    return [&out](asio::io_context&) -> std::unique_ptr<ObservationSource> {
        auto s = std::make_unique<FakeSource>();
        out = s.get();
        return s;
    };
}

inline std::function<std::unique_ptr<ObservationSource>(asio::io_context&)> replay(double speed)
{
    return [speed](asio::io_context& io) -> std::unique_ptr<ObservationSource> {
        return std::make_unique<skycued::ReplayObservationSource>(io, kFixture, speed, false);
    };
}

// A listening socket on an ephemeral port, polled without blocking.
struct TestServer {
    asio::io_context io;
    tcp::acceptor acceptor{io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0)};
    TestServer() { acceptor.non_blocking(true); }
    uint16_t port() const { return acceptor.local_endpoint().port(); }

    std::unique_ptr<tcp::socket> accept(std::chrono::milliseconds timeout)
    {
        const auto deadline = steady::now() + timeout;
        while (steady::now() < deadline) {
            auto s = std::make_unique<tcp::socket>(io);
            boost::system::error_code ec;
            acceptor.accept(*s, ec);
            if (!ec)
                return s;
            std::this_thread::sleep_for(5ms);
        }
        return nullptr;
    }
};

}  // namespace enginetest
