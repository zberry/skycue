#define BOOST_TEST_MODULE skycued_inputs
#include <boost/test/unit_test.hpp>


#include "test_util.hpp"
#include "asterix/cat021.hpp"
#include "asterix/cat021_encode.hpp"
#include "engine_test_support.hpp"
#include "file_util.hpp"
#include "inputs/input.hpp"
#include "config.hpp"
#include "inputs/beast_raw_input.hpp"
#include "inputs/input_def.hpp"
#include "inputs/input_manager.hpp"
#include "inputs/inputs_file.hpp"
#include "inputs/ping_cat021_input.hpp"
#include "inputs/ping_json_input.hpp"
#include "inputs/source_input.hpp"
#include "inputs/uat_raw_input.hpp"
#include "hub/hub.hpp"
#include "json_lite.hpp"
#include "outputs/output_def.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cmath>

using skycue_test::fileExists;

using namespace enginetest;
using asio::ip::udp;
using skycued::UatRawInput;
using skycued::Input;
using skycued::InputConflict;
using skycued::InputDef;
using skycued::InputDefError;
using skycued::InputEntry;
using skycued::InputForbidden;
using skycued::InputManager;
using skycued::InputManagerConfig;
using skycued::InputState;
using skycued::InputsFile;
using skycued::InputType;
using skycued::PingCat021Input;
using skycued::PingJsonInput;
using skycued::SourceInput;
namespace ax = skycued::asterix;

namespace {

using Bytes = std::vector<uint8_t>;

template <class F> bool eventuallyWithin(std::chrono::milliseconds limit, F pred)
{
    const auto until = steady::now() + limit;
    while (steady::now() < until) {
        if (pred())
            return true;
        std::this_thread::sleep_for(5ms);
    }
    return pred();
}
template <class F> bool eventually(F pred) { return eventuallyWithin(kWait, pred); }

// Waits until count() reaches `target` or stops growing for 200 ms (a
// datagram the kernel dropped never arrives), at most kWait.
template <class F> void settle(F count, uint64_t target)
{
    const auto until = steady::now() + kWait;
    uint64_t last = count();
    auto changed = steady::now();
    while (last < target && steady::now() < until && steady::now() - changed < 200ms) {
        std::this_thread::sleep_for(2ms);
        const uint64_t now = count();
        if (now != last) {
            last = now;
            changed = steady::now();
        }
    }
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
// Names in `dir` other than . and ..
std::vector<std::string> dirEntries(const std::string& dir)
{
    std::vector<std::string> out;
    if (DIR* d = ::opendir(dir.c_str())) {
        while (dirent* e = ::readdir(d))
            if (std::string(e->d_name) != "." && std::string(e->d_name) != "..")
                out.push_back(e->d_name);
        ::closedir(d);
    }
    return out;
}


std::string fixture(const std::string& name)
{
    const std::string text = readAll(std::string(SKYCUE_FIXTURE_DIR) + "/pingstation/" + name);
    BOOST_REQUIRE_MESSAGE(!text.empty(), "cannot open fixture " << name);
    return text;
}

InputDef defOf(const std::string& json)
{
    const auto o = skycued::JsonObject::parse(json);
    if (!o)
        throw std::runtime_error("test JSON does not parse: " + json);
    return skycued::parseInputDef(*o);
}

InputDef psJson(const std::string& id, uint16_t port, const std::string& extra = "")
{
    return defOf(R"({"id":")" + id + R"(","type":"pingstation_json","bind":"127.0.0.1","port":)" +
                 std::to_string(port) + extra + "}");
}

InputDef cat021(const std::string& id, const std::string& transport, uint16_t port)
{
    const std::string where = transport == "tcp_connect" ? R"("host":"127.0.0.1")" : R"("bind":"127.0.0.1")";
    return defOf(R"({"id":")" + id + R"(","type":"pingstation_cat021","transport":")" + transport + R"(",)" +
                 where + R"(,"port":)" + std::to_string(port) + "}");
}

// True when 127.0.0.1:port can be bound for UDP right now (nothing holds it).
bool udpPortFree(uint16_t port)
{
    asio::io_context io;
    udp::socket s(io);
    boost::system::error_code ec;
    s.open(udp::v4(), ec);
    s.bind(udp::endpoint(asio::ip::make_address("127.0.0.1"), port), ec);
    return !ec;
}

struct UdpSender {
    asio::io_context io;
    udp::socket sock{io, udp::endpoint(udp::v4(), 0)};
    void send(uint16_t port, const void* data, std::size_t size)
    {
        boost::system::error_code ignored;
        sock.send_to(asio::buffer(data, size), udp::endpoint(asio::ip::make_address("127.0.0.1"), port), 0, ignored);
    }
    void send(uint16_t port, const std::string& s) { send(port, s.data(), s.size()); }
    void send(uint16_t port, const Bytes& b) { send(port, b.data(), b.size()); }
};

// Connects to 127.0.0.1:port, retrying while the listener is not bound yet.
tcp::socket connectWhenListening(asio::io_context& io, uint16_t port)
{
    const auto until = steady::now() + kWait;
    for (;;) {
        tcp::socket s(io);
        boost::system::error_code ec;
        s.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), ec);
        if (!ec)
            return s;
        if (steady::now() > until)
            throw std::runtime_error("cannot connect to port " + std::to_string(port));
        std::this_thread::sleep_for(10ms);
    }
}

void writeAll(tcp::socket& s, const Bytes& b) { asio::write(s, asio::buffer(b)); }

// A CAT021 record the mapping turns into an observation of `address`.
ax::Cat021Record record(uint32_t address)
{
    ax::Cat021Record r;
    r.sac = 0;
    r.sic = 0;
    r.trd = ax::Descriptor040{};
    r.address = address;
    r.lat131 = static_cast<int32_t>(std::lround(42.5 / ax::kLsb131Deg));
    r.lon131 = static_cast<int32_t>(std::lround(-93.25 / ax::kLsb131Deg));
    return r;
}

Bytes block(const std::vector<uint32_t>& addresses)
{
    std::vector<ax::Cat021Record> records;
    for (uint32_t a : addresses)
        records.push_back(record(a));
    return ax::encodeBlock(records);
}

// CAT/LEN around `body`.
Bytes rawBlock(uint8_t cat, const Bytes& body)
{
    const std::size_t len = body.size() + 3;
    Bytes b{cat, static_cast<uint8_t>(len >> 8), static_cast<uint8_t>(len & 0xFF)};
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 11
    // gcc 11 false positive: -Wstringop-overread ("reading 1 or more bytes from a
    // region of size 0") on the memcpy inlined from vector::insert, on a path
    // where body is empty and insert copies nothing.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overread"
#endif
    b.insert(b.end(), body.begin(), body.end());
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 11
#pragma GCC diagnostic pop
#endif
    return b;
}

// A block of another category: garbage to a pingStation input. It holds no
// 0x15 byte, so a framer that dropped it resyncs exactly on the next block.
const Bytes kJunk = rawBlock(48, {0xAA, 0xBB, 0xCC});

// Everything an Input or the InputManager reported; filled on the io thread.
struct Seen {
    std::mutex m;
    std::vector<TrackObservation> obs;
    int statuses = 0, changes = 0;

    void add(const TrackObservation& o)
    {
        std::lock_guard<std::mutex> lock(m);
        obs.push_back(o);
    }
    std::size_t count()
    {
        std::lock_guard<std::mutex> lock(m);
        return obs.size();
    }
    std::vector<TrackObservation> all()
    {
        std::lock_guard<std::mutex> lock(m);
        return obs;
    }
    int statusCount()
    {
        std::lock_guard<std::mutex> lock(m);
        return statuses;
    }
};

// One Input, started on the io thread.
struct Probe {
    IoThread& t;
    std::unique_ptr<Input> in;
    Seen seen;

    Probe(IoThread& thread, std::unique_ptr<Input> input) : t(thread)
    {
        t.call([&] {
            in = std::move(input);
            in->onObservation([this](const TrackObservation& o) { seen.add(o); });
            in->onStatus([this] {
                std::lock_guard<std::mutex> lock(seen.m);
                ++seen.statuses;
            });
            in->start();
        });
    }
    ~Probe()
    {
        t.call([&] { in.reset(); });
    }
    InputState state()
    {
        return t.call([&] { return in->state(); });
    }
    std::optional<steady::time_point> loggedAt()
    {
        return t.call([&] { return in->lastErrorLoggedAt(); });
    }
};

// A readsb built-in over an ObservationSource the test drives. Touched on the
// io thread only.
struct FakeHub {
    int starts = 0, stops = 0;
    struct Source;
    Source* current = nullptr;
};
struct FakeHub::Source : ObservationSource {
    explicit Source(FakeHub& h) : hub(h) {}
    ~Source() override
    {
        if (hub.current == this)
            hub.current = nullptr;
    }
    void onObservation(std::function<void(const TrackObservation&)> h) override { obs = std::move(h); }
    void onConnection(std::function<void(bool)> h) override { conn = std::move(h); }
    void start() override
    {
        ++hub.starts;
        hub.current = this;
    }
    void stop() override { ++hub.stops; }
    FakeHub& hub;
    std::function<void(const TrackObservation&)> obs;
    std::function<void(bool)> conn;
};

std::unique_ptr<Input> fakeReadsb(FakeHub& hub)
{
    InputDef d;
    d.id = "readsb";
    d.type = InputType::readsb;
    d.host = "127.0.0.1";
    d.port = 1;
    d.builtin = true;
    return std::make_unique<SourceInput>(
        d, SourceInput::SourceFactory([&hub]() -> std::unique_ptr<ObservationSource> {
            return std::make_unique<FakeHub::Source>(hub);
        }));
}

// The manager on the io thread: built, given its built-ins, loaded and started.
struct Mgr {
    IoThread& t;
    std::unique_ptr<InputManager> m;
    Seen seen;
    std::function<void(const TrackObservation&)> hook;  // io thread; set through on()

    Mgr(IoThread& thread, InputManagerConfig cfg, std::function<void(InputManager&)> builtins = {}) : t(thread)
    {
        t.call([&] {
            m = std::make_unique<InputManager>(t.io, cfg);
            m->onObservation([this](const TrackObservation& o) {
                seen.add(o);
                if (auto h = hook)  // a copy: the hook may reset itself
                    h(o);
            });
            m->onChanged([this] {
                std::lock_guard<std::mutex> lock(seen.m);
                ++seen.changes;
            });
            if (builtins)
                builtins(*m);
            m->load();
            m->start();
        });
    }
    ~Mgr()
    {
        t.call([&] { m.reset(); });
    }
    template <class F> auto on(F f) -> decltype(f(std::declval<InputManager&>()))
    {
        return t.call([&] { return f(*m); });
    }
    std::optional<InputEntry> find(const std::string& id)
    {
        return on([&](InputManager& im) { return im.find(id); });
    }
    InputState state(const std::string& id)
    {
        const auto e = find(id);
        BOOST_REQUIRE_MESSAGE(e, "no input " << id);
        return e->state;
    }
    std::vector<std::string> ids()
    {
        std::vector<std::string> out;
        for (const auto& e : on([](InputManager& im) { return im.list(); }))
            out.push_back(e.def.id);
        return out;
    }
};

constexpr std::chrono::milliseconds kPoll{50};

InputManagerConfig fileConfig(const TempDir& tmp)
{
    InputManagerConfig cfg;
    cfg.inputs_file = tmp.path + "/skycue.inputs.json";
    cfg.poll_interval = kPoll;
    return cfg;
}

// Writes `text` the way an editor that saves atomically does.
void handWrite(const std::string& path, const std::string& text)
{
    {
        std::ofstream out(path + ".edit", std::ios::binary | std::ios::trunc);
        out << text;
    }
    BOOST_REQUIRE(::rename((path + ".edit").c_str(), path.c_str()) == 0);
}

std::string fileOf(const std::vector<InputDef>& inputs)
{
    InputsFile f;
    f.inputs = inputs;
    return skycued::inputsFileJson(f);
}

// Redirects stderr (the log) to a file until text() is called.
struct StderrCapture {
    std::string path;
    int saved = -1;
    explicit StderrCapture(std::string p) : path(std::move(p))
    {
        std::fflush(stderr);
        saved = ::dup(2);
        const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        ::dup2(fd, 2);
        ::close(fd);
    }
    std::string text()
    {
        if (saved >= 0) {
            std::fflush(stderr);
            ::dup2(saved, 2);
            ::close(saved);
            saved = -1;
        }
        return readAll(path);
    }
    ~StderrCapture() { text(); }
};

}  // namespace

// ---------------------------------------------------------------- inputs

BOOST_AUTO_TEST_CASE(ping_json_input_end_to_end)
{
    IoThread t;
    const uint16_t port = freeUdpPort();
    Probe p(t, std::make_unique<PingJsonInput>(t.io, psJson("ps", port)));
    BOOST_REQUIRE(eventually([&] { return p.state().connected; }));
    UdpSender tx;
    tx.send(port, fixture("traffic-1090.json"));
    BOOST_REQUIRE(eventually([&] { return p.seen.count() == 1; }));
    const auto o = p.seen.all()[0];
    BOOST_TEST(o.input_id == "ps");
    BOOST_TEST(o.id.address == 0xAC5E81u);

    tx.send(port, fixture("status.json"));
    BOOST_REQUIRE(eventually([&] { return p.seen.statusCount() == 1; }));
    const InputState s = p.state();
    BOOST_REQUIRE(s.device_status);
    BOOST_TEST(s.device_status->guid == "0000000000000001");
    BOOST_TEST(s.counters.messages == 2u);
    BOOST_TEST(s.counters.observations == 1u);
    BOOST_TEST(s.counters.rejected == 0u);
    BOOST_TEST(s.counters.connects == 1u);
    BOOST_TEST(s.receiving);
    BOOST_TEST(p.seen.count() == 1u);
}

BOOST_AUTO_TEST_CASE(ping_cat021_udp_and_tcp)
{
    IoThread t;
    const Bytes blk = block({0xABC001, 0xABC002});
    const std::set<uint32_t> want{0xABC001, 0xABC002};
    auto addresses = [](Seen& seen) {
        std::set<uint32_t> out;
        for (const auto& o : seen.all())
            out.insert(o.id.address);
        return out;
    };

    // udp
    const uint16_t uport = freeUdpPort();
    Probe udp_in(t, std::make_unique<PingCat021Input>(t.io, cat021("cu", "udp", uport)));
    BOOST_REQUIRE(eventually([&] { return udp_in.state().connected; }));
    UdpSender tx;
    tx.send(uport, blk);
    BOOST_REQUIRE(eventually([&] { return udp_in.seen.count() == 2; }));
    BOOST_TEST((addresses(udp_in.seen) == want));
    BOOST_TEST(udp_in.seen.all()[0].input_id == "cu");

    // tcp_listen, the block split across writes
    asio::io_context cio;
    const uint16_t lport = freeTcpPort();
    Probe listen_in(t, std::make_unique<PingCat021Input>(t.io, cat021("cl", "tcp_listen", lport)));
    tcp::socket c = connectWhenListening(cio, lport);
    const std::size_t half = blk.size() / 2;
    writeAll(c, Bytes(blk.begin(), blk.begin() + static_cast<std::ptrdiff_t>(half)));
    BOOST_REQUIRE(eventually([&] { return listen_in.state().counters.bytes == half; }));
    writeAll(c, Bytes(blk.begin() + static_cast<std::ptrdiff_t>(half), blk.end()));
    BOOST_REQUIRE(eventually([&] { return listen_in.seen.count() == 2; }));
    BOOST_TEST((addresses(listen_in.seen) == want));

    // tcp_connect to a test server
    tcp::acceptor acc(cio, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const uint16_t cport = acc.local_endpoint().port();
    Probe connect_in(t, std::make_unique<PingCat021Input>(t.io, cat021("cc", "tcp_connect", cport)));
    tcp::socket server = acc.accept();
    writeAll(server, blk);
    BOOST_REQUIRE(eventually([&] { return connect_in.seen.count() == 2; }));
    BOOST_TEST((addresses(connect_in.seen) == want));

    for (Probe* p : {&udp_in, &listen_in, &connect_in}) {
        const InputState s = p->state();
        BOOST_TEST(s.counters.messages == 2u);
        BOOST_TEST(s.counters.observations == 2u);
        BOOST_TEST(s.counters.rejected == 0u);
        BOOST_TEST(s.counters.bytes == blk.size());
        BOOST_TEST(s.connected);
    }
    BOOST_TEST(listen_in.state().peers == 1u);
}

BOOST_AUTO_TEST_CASE(ping_cat021_tcp_garbage_resync_counts_rejected)
{
    IoThread t;
    asio::io_context cio;
    const uint16_t port = freeTcpPort();
    Probe p(t, std::make_unique<PingCat021Input>(t.io, cat021("cl", "tcp_listen", port)));
    tcp::socket c = connectWhenListening(cio, port);
    Bytes stream{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    const Bytes blk = block({0xABC001});
    stream.insert(stream.end(), blk.begin(), blk.end());
    writeAll(c, stream);
    BOOST_REQUIRE(eventually([&] { return p.seen.count() == 1; }));
    const InputState s = p.state();
    BOOST_TEST(s.counters.rejected == 1u);
    BOOST_TEST(s.counters.messages == 1u);
    BOOST_TEST(!s.last_error.empty());
}

BOOST_AUTO_TEST_CASE(ping_cat021_counters_per_spec)
{
    IoThread t;
    const uint16_t port = freeUdpPort();
    Probe p(t, std::make_unique<PingCat021Input>(t.io, cat021("cu", "udp", port)));
    BOOST_REQUIRE(eventually([&] { return p.state().connected; }));
    UdpSender tx;

    tx.send(port, kJunk);  // another category: rejected
    BOOST_REQUIRE(eventually([&] { return p.state().counters.rejected == 1; }));

    ax::Cat021Record sim = record(0xABC003);
    sim.trd->has_ext1 = true;
    sim.trd->sim = true;
    tx.send(port, ax::encodeBlock({sim}));  // a SIM record: records_skipped
    BOOST_REQUIRE(eventually([&] { return p.state().counters.records_skipped == 1; }));

    // FRN 43 is spare: its length is unknown and the block is lost.
    tx.send(port, rawBlock(21, {0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x80, 0x00, 0x00}));
    BOOST_REQUIRE(eventually([&] { return p.state().counters.records_skipped == 2; }));

    Bytes truncated = block({0xABC004});
    truncated.push_back(0x15);  // a trailing byte that is no block
    tx.send(port, truncated);
    BOOST_REQUIRE(eventually([&] { return p.seen.count() == 1; }));

    const InputState s = p.state();
    BOOST_TEST(s.counters.rejected == 2u);         // the CAT 48 block, the truncated datagram
    BOOST_TEST(s.counters.records_skipped == 2u);  // SIM, the unknown item length
    BOOST_TEST(s.counters.messages == 2u);         // the SIM record and the good one
    BOOST_TEST(s.counters.observations == 1u);
    BOOST_TEST(p.seen.all()[0].id.address == 0xABC004u);
}

// Each connection has its own framer, dropped when the connection closes: the
// second half of a block on a new connection is garbage, not the rest of the
// half block from the old one.
BOOST_AUTO_TEST_CASE(tcp_connect_reconnect_resets_framer)
{
    IoThread t;
    asio::io_context cio;
    tcp::acceptor acc(cio, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const uint16_t port = acc.local_endpoint().port();
    auto input = std::make_unique<PingCat021Input>(t.io, cat021("cc", "tcp_connect", port));
    PingCat021Input* cat = input.get();
    Probe p(t, std::move(input));
    auto framers = [&] { return t.call([&] { return cat->framerCount(); }); };
    const Bytes blk = block({0xABC001});
    const auto half = static_cast<std::ptrdiff_t>(blk.size() / 2);
    const Bytes first(blk.begin(), blk.begin() + half), second(blk.begin() + half, blk.end());
    {
        tcp::socket s1 = acc.accept();
        writeAll(s1, first);
        BOOST_REQUIRE(eventually([&] { return p.state().counters.bytes == first.size(); }));
        BOOST_TEST(framers() == 1u);
    }  // closed: the framer goes and the input reconnects
    BOOST_REQUIRE(eventually([&] { return framers() == 0; }));
    tcp::socket s2 = acc.accept();
    Bytes stream = second;
    stream.insert(stream.end(), blk.begin(), blk.end());
    writeAll(s2, stream);
    BOOST_REQUIRE(eventually([&] { return p.state().counters.bytes == first.size() + stream.size(); }));
    BOOST_REQUIRE(eventually([&] { return p.seen.count() == 1; }));
    const InputState s = p.state();
    BOOST_TEST(p.seen.count() == 1u);
    BOOST_TEST(p.seen.all()[0].id.address == 0xABC001u);
    BOOST_TEST(s.counters.messages == 1u);
    BOOST_TEST(s.counters.rejected == 1u);  // the stray second half
    BOOST_TEST(s.counters.connects == 2u);
    BOOST_TEST(framers() == 1u);
}

BOOST_AUTO_TEST_CASE(malformed_flood_counted_and_log_rate_limited)
{
    IoThread t;
    const auto before = steady::now();

    // UDP: the kernel may drop a few of 1000 datagrams; send in steps so the
    // receive buffer rarely overflows.
    const uint16_t uport = freeUdpPort();
    Probe u(t, std::make_unique<PingCat021Input>(t.io, cat021("cu", "udp", uport)));
    BOOST_REQUIRE(eventually([&] { return u.state().connected; }));
    BOOST_TEST(!u.loggedAt());
    UdpSender tx;
    for (int sent = 0; sent < 1000;) {
        for (int i = 0; i < 50; ++i, ++sent)
            tx.send(uport, kJunk);
        settle([&] { return u.state().counters.rejected; }, static_cast<uint64_t>(sent));
    }
    InputState s = u.state();
    BOOST_TEST(s.counters.rejected >= 900u);
    BOOST_TEST(s.counters.rejected <= 1000u);
    BOOST_TEST(contains(s.last_error, "CAT 48"));
    const auto logged = u.loggedAt();
    BOOST_REQUIRE(logged);
    BOOST_TEST((*logged >= before));
    // Still within 10 s: more junk is counted but not logged again.
    const uint64_t counted = s.counters.rejected;
    tx.send(uport, kJunk);
    BOOST_REQUIRE(eventually([&] { return u.state().counters.rejected == counted + 1; }));
    BOOST_TEST((u.loggedAt() == logged));

    // tcp_listen: the same junk, each block followed by a good one, so the
    // framer counts exactly one dropped run per junk block.
    asio::io_context cio;
    const uint16_t lport = freeTcpPort();
    Probe l(t, std::make_unique<PingCat021Input>(t.io, cat021("cl", "tcp_listen", lport)));
    tcp::socket c = connectWhenListening(cio, lport);
    const Bytes good = block({0xABC001});
    Bytes stream;
    for (int i = 0; i < 1000; ++i) {
        stream.insert(stream.end(), kJunk.begin(), kJunk.end());
        stream.insert(stream.end(), good.begin(), good.end());
    }
    writeAll(c, stream);
    BOOST_REQUIRE(eventually([&] { return l.seen.count() == 1000; }));
    s = l.state();
    BOOST_TEST(s.counters.rejected == 1000u);
    BOOST_TEST(s.counters.messages == 1000u);
    BOOST_TEST(s.counters.bytes == stream.size());
    BOOST_TEST(contains(s.last_error, "resynchronised"));
    const auto tcp_logged = l.loggedAt();
    BOOST_REQUIRE(tcp_logged);
    writeAll(c, kJunk);
    writeAll(c, good);
    BOOST_REQUIRE(eventually([&] { return l.state().counters.rejected == 1001; }));
    BOOST_TEST((l.loggedAt() == tcp_logged));
}

// ---------------------------------------------------------------- manager

BOOST_AUTO_TEST_CASE(create_persists_and_starts)
{
    IoThread t;
    TempDir tmp;
    const auto cfg = fileConfig(tmp);
    Mgr m(t, cfg);
    const uint16_t port = freeUdpPort();
    const InputDef d = psJson("ps", port, R"(,"name":"North")");
    const InputEntry e = m.on([&](InputManager& im) { return im.create(d); });
    BOOST_TEST(e.def.id == "ps");
    BOOST_TEST(!e.def.builtin);
    const auto file = skycued::parseInputsFile(readAll(cfg.inputs_file));
    BOOST_REQUIRE_EQUAL(file.inputs.size(), 1u);
    BOOST_TEST((file.inputs[0] == d));
    BOOST_REQUIRE(eventually([&] { return m.state("ps").connected; }));
    UdpSender tx;
    tx.send(port, fixture("traffic-1090.json"));
    BOOST_REQUIRE(eventually([&] { return m.seen.count() == 1; }));
    BOOST_TEST(m.seen.all()[0].input_id == "ps");
    BOOST_TEST(m.on([](InputManager& im) { return im.connectedCount(); }) == 1u);
    const auto lines = m.on([](InputManager& im) { return im.statsLines(); });
    BOOST_REQUIRE_EQUAL(lines.size(), 1u);
    BOOST_TEST(lines[0].id == "ps");
    BOOST_TEST(lines[0].observations == 1u);
}

BOOST_AUTO_TEST_CASE(create_duplicate_id_409)
{
    IoThread t;
    TempDir tmp;
    Mgr m(t, fileConfig(tmp));
    m.on([&](InputManager& im) { return im.create(psJson("ps", freeUdpPort())); });
    BOOST_CHECK_THROW(m.on([&](InputManager& im) { return im.create(psJson("ps", freeUdpPort())); }), InputConflict);
    BOOST_TEST(m.ids().size() == 1u);
}

BOOST_AUTO_TEST_CASE(create_invalid_400_message_verbatim)
{
    IoThread t;
    TempDir tmp;
    const auto cfg = fileConfig(tmp);
    Mgr m(t, cfg);
    InputDef bad = psJson("ps", 4000);
    bad.port = 0;
    try {
        m.on([&](InputManager& im) { return im.create(bad); });
        BOOST_FAIL("port 0 accepted");
    } catch (const InputDefError& e) {
        BOOST_TEST(std::string(e.what()) == "port must be an integer from 1 to 65535");
    }
    InputDef capture = psJson("ps", freeUdpPort(), R"(,"capture":true)");
    try {
        m.on([&](InputManager& im) { return im.create(capture); });
        BOOST_FAIL("capture without a record dir accepted");
    } catch (const InputDefError& e) {
        BOOST_TEST(std::string(e.what()) == "capture needs recording.dir");
    }
    InputDef reserved = psJson("ps", freeUdpPort());
    reserved.id = "readsb";
    BOOST_CHECK_THROW(m.on([&](InputManager& im) { return im.create(reserved); }), InputDefError);
    BOOST_TEST(m.ids().empty());
    BOOST_TEST(!fileExists(cfg.inputs_file));
}

BOOST_AUTO_TEST_CASE(replace_resets_connection_keeps_counters)
{
    IoThread t;
    TempDir tmp;
    const auto cfg = fileConfig(tmp);
    Mgr m(t, cfg);
    const uint16_t port = freeUdpPort();
    m.on([&](InputManager& im) { return im.create(psJson("ps", port)); });
    BOOST_REQUIRE(eventually([&] { return m.state("ps").connected; }));
    UdpSender tx;
    for (int i = 0; i < 3; ++i)
        tx.send(port, fixture("traffic-1090.json"));
    BOOST_REQUIRE(eventually([&] { return m.state("ps").counters.messages == 3; }));

    const InputEntry e = m.on([&](InputManager& im) { return im.replace("ps", psJson("ps", port, R"(,"name":"North")")); });
    BOOST_TEST(e.def.name == "North");
    BOOST_TEST(skycued::parseInputsFile(readAll(cfg.inputs_file)).inputs.at(0).name == "North");
    BOOST_REQUIRE(eventually([&] { return m.state("ps").counters.connects == 2; }));
    BOOST_TEST(m.state("ps").connected);
    for (int i = 0; i < 2; ++i)
        tx.send(port, fixture("traffic-1090.json"));
    BOOST_REQUIRE(eventually([&] { return m.state("ps").counters.messages == 5; }));
    BOOST_TEST(m.state("ps").counters.observations == 5u);
}

BOOST_AUTO_TEST_CASE(edit_while_receiving_rebinds_same_port)
{
    IoThread t;
    TempDir tmp;
    Mgr m(t, fileConfig(tmp));
    const uint16_t port = freeUdpPort();
    m.on([&](InputManager& im) { return im.create(psJson("ps", port)); });
    std::atomic<bool> stop{false};
    // Read on this thread: fixture() asserts, and Boost.Test assertions are
    // not thread-safe (TSan saw the sender race the main thread on them).
    const std::string text = fixture("traffic-1090.json");
    std::thread sender([&] {
        UdpSender tx;
        while (!stop) {
            tx.send(port, text);
            std::this_thread::sleep_for(1ms);
        }
    });
    BOOST_REQUIRE(eventually([&] { return m.seen.count() > 0; }));
    for (int i = 0; i < 5; ++i)
        m.on([&](InputManager& im) { return im.replace("ps", psJson("ps", port, R"(,"name":"n)" + std::to_string(i) + "\"")); });
    const std::size_t after = m.seen.count();
    const bool continued = eventually([&] { return m.seen.count() > after + 10; });
    stop = true;
    sender.join();
    BOOST_TEST(continued);
    const InputState s = m.state("ps");
    BOOST_TEST(s.connected);
    BOOST_TEST(s.counters.connects == 6u);
    BOOST_TEST(s.last_error.empty());
    BOOST_TEST(m.find("ps")->def.name == "n4");
}

BOOST_AUTO_TEST_CASE(disable_stops_enable_restarts)
{
    IoThread t;
    TempDir tmp;
    const auto cfg = fileConfig(tmp);
    Mgr m(t, cfg);
    const uint16_t port = freeUdpPort();
    m.on([&](InputManager& im) { return im.create(psJson("ps", port)); });
    BOOST_REQUIRE(eventually([&] { return m.state("ps").connected; }));

    const InputEntry off = m.on([](InputManager& im) { return im.setEnabled("ps", false); });
    BOOST_TEST(!off.def.enabled);
    BOOST_TEST(!off.state.connected);
    BOOST_TEST(udpPortFree(port));
    BOOST_TEST(!skycued::parseInputsFile(readAll(cfg.inputs_file)).inputs.at(0).enabled);
    BOOST_TEST(m.on([](InputManager& im) { return im.enabledCount(); }) == 0u);

    m.on([](InputManager& im) { return im.setEnabled("ps", true); });
    BOOST_REQUIRE(eventually([&] { return m.state("ps").connected; }));
    BOOST_TEST(m.state("ps").counters.connects == 2u);
    BOOST_TEST(skycued::parseInputsFile(readAll(cfg.inputs_file)).inputs.at(0).enabled);
    UdpSender tx;
    tx.send(port, fixture("traffic-1090.json"));
    BOOST_REQUIRE(eventually([&] { return m.seen.count() == 1; }));
}

BOOST_AUTO_TEST_CASE(remove_builtin_forbidden)
{
    IoThread t;
    TempDir tmp;
    FakeHub hub;
    Mgr m(t, fileConfig(tmp), [&](InputManager& im) { im.addBuiltin(fakeReadsb(hub)); });
    BOOST_CHECK_THROW(m.on([](InputManager& im) { im.remove("readsb"); }), InputForbidden);
    BOOST_CHECK_THROW(m.on([](InputManager& im) { im.remove("nope"); }), skycued::InputNotFound);
    BOOST_TEST(m.find("readsb")->def.builtin);
    BOOST_TEST(m.on([&](InputManager& im) { return im.builtin("readsb") != nullptr; }));
}

BOOST_AUTO_TEST_CASE(replace_builtin_forbidden)
{
    IoThread t;
    TempDir tmp;
    FakeHub hub;
    Mgr m(t, fileConfig(tmp), [&](InputManager& im) { im.addBuiltin(fakeReadsb(hub)); });
    InputDef d = m.find("readsb")->def;
    d.builtin = false;
    d.port = 30099;
    BOOST_CHECK_THROW(m.on([&](InputManager& im) { return im.replace("readsb", d); }), InputForbidden);
    BOOST_TEST(m.find("readsb")->def.port == 1u);
    BOOST_TEST(m.on([&](InputManager&) { return hub.starts; }) == 1);
}

BOOST_AUTO_TEST_CASE(builtin_enabled_persisted)
{
    IoThread t;
    TempDir tmp;
    const auto cfg = fileConfig(tmp);
    {
        FakeHub hub;
        Mgr m(t, cfg, [&](InputManager& im) { im.addBuiltin(fakeReadsb(hub)); });
        BOOST_TEST(m.on([&](InputManager&) { return hub.starts; }) == 1);
        m.on([&](InputManager&) {
            hub.current->conn(true);
            return 0;
        });
        BOOST_TEST(m.on([](InputManager& im) { return im.anyTrackInputConnected(); }));
        m.on([](InputManager& im) { return im.setEnabled("readsb", false); });
        BOOST_TEST(m.on([&](InputManager&) { return hub.stops; }) == 1);
        BOOST_TEST(!m.on([](InputManager& im) { return im.anyTrackInputConnected(); }));
        BOOST_TEST(skycued::parseInputsFile(readAll(cfg.inputs_file)).builtin_enabled.at("readsb") == false);
    }
    FakeHub hub;
    Mgr m(t, cfg, [&](InputManager& im) { im.addBuiltin(fakeReadsb(hub)); });
    BOOST_TEST(!m.find("readsb")->def.enabled);
    BOOST_TEST(m.on([&](InputManager&) { return hub.starts; }) == 0);
    // Enabled again: a new source from the factory.
    m.on([](InputManager& im) { return im.setEnabled("readsb", true); });
    BOOST_TEST(m.on([&](InputManager&) { return hub.starts; }) == 1);
    BOOST_TEST(skycued::parseInputsFile(readAll(cfg.inputs_file)).builtin_enabled.at("readsb") == true);
}

BOOST_AUTO_TEST_CASE(remove_forgets_counters)
{
    IoThread t;
    TempDir tmp;
    const auto cfg = fileConfig(tmp);
    Mgr m(t, cfg);
    const uint16_t port = freeUdpPort();
    m.on([&](InputManager& im) { return im.create(psJson("ps", port)); });
    BOOST_REQUIRE(eventually([&] { return m.state("ps").connected; }));
    UdpSender tx;
    tx.send(port, fixture("traffic-1090.json"));
    BOOST_REQUIRE(eventually([&] { return m.state("ps").counters.messages == 1; }));
    m.on([&](InputManager& im) { return im.replace("ps", psJson("ps", port, R"(,"name":"x")")); });
    BOOST_TEST(m.state("ps").counters.messages == 1u);  // kept across the replace

    m.on([](InputManager& im) { im.remove("ps"); });
    BOOST_TEST(!m.find("ps"));
    BOOST_TEST(skycued::parseInputsFile(readAll(cfg.inputs_file)).inputs.empty());
    BOOST_TEST(udpPortFree(port));
    m.on([&](InputManager& im) { return im.create(psJson("ps", port)); });
    const InputState s = m.state("ps");
    BOOST_TEST(s.counters.messages == 0u);
    BOOST_TEST(s.counters.connects <= 1u);
}

// A handler that replaces or removes the input whose observation it is
// handling: the input stops at once, its destruction waits until the stack
// unwinds, and the rest of that datagram is not passed on.
BOOST_AUTO_TEST_CASE(replace_and_remove_from_own_callback)
{
    IoThread t;
    TempDir tmp;
    Mgr m(t, fileConfig(tmp));
    const uint16_t port = freeUdpPort();
    m.on([&](InputManager& im) { return im.create(psJson("ps", port)); });
    BOOST_REQUIRE(eventually([&] { return m.state("ps").connected; }));
    const std::string two = R"({"aircraft":[{"icaoAddress":"AC5E81"},{"icaoAddress":"AC5E82"}]})";
    UdpSender tx;

    m.on([&](InputManager&) {
        m.hook = [&m, &im = *m.m, port](const TrackObservation&) {
            m.hook = nullptr;
            im.replace("ps", psJson("ps", port, R"(,"name":"again")"));
        };
        return 0;
    });
    tx.send(port, two);
    BOOST_REQUIRE(eventually([&] { return m.seen.count() == 1 && m.find("ps")->def.name == "again"; }));
    BOOST_REQUIRE(eventually([&] { return m.state("ps").connected; }));

    m.on([&](InputManager&) {
        m.hook = [&m, &im = *m.m](const TrackObservation&) {
            m.hook = nullptr;
            im.remove("ps");
        };
        return 0;
    });
    tx.send(port, two);
    BOOST_REQUIRE(eventually([&] { return !m.find("ps"); }));
    BOOST_TEST(m.seen.count() == 2u);
    BOOST_TEST(m.ids().empty());
}

BOOST_AUTO_TEST_CASE(no_file_until_first_change)
{
    IoThread t;
    TempDir tmp;
    const auto cfg = fileConfig(tmp);
    FakeHub hub;
    Mgr m(t, cfg, [&](InputManager& im) { im.addBuiltin(fakeReadsb(hub)); });
    BOOST_TEST(m.ids() == std::vector<std::string>{"readsb"});
    BOOST_TEST(!fileExists(cfg.inputs_file));
    m.on([](InputManager& im) { return im.setEnabled("readsb", true); });  // no change
    BOOST_TEST(!fileExists(cfg.inputs_file));
    m.on([](InputManager& im) { return im.setEnabled("readsb", false); });
    BOOST_TEST(fileExists(cfg.inputs_file));
}

BOOST_AUTO_TEST_CASE(malformed_file_at_load_renamed_rejected)
{
    IoThread t;
    TempDir tmp;
    const auto cfg = fileConfig(tmp);
    const std::string text = R"({"version":1,"inputs":[{"id":"ps","type":"pingstation_json","port":0}]})";
    handWrite(cfg.inputs_file, text);
    FakeHub hub;
    Mgr m(t, cfg, [&](InputManager& im) { im.addBuiltin(fakeReadsb(hub)); });
    BOOST_TEST(m.ids() == std::vector<std::string>{"readsb"});
    BOOST_TEST(!fileExists(cfg.inputs_file));
    BOOST_TEST(readAll(cfg.inputs_file + ".rejected") == text);
    BOOST_TEST(m.on([&](InputManager&) { return hub.starts; }) == 1);
}

// A name stored before the stricter name rule (invalid UTF-8, a C1 control)
// is repaired at load, logged at ERROR with its input id, and the rest of
// the file loads unchanged; the file is not renamed .rejected.
BOOST_AUTO_TEST_CASE(bad_name_in_file_repaired_at_load)
{
    TempDir tmp, logdir;
    const auto cfg = fileConfig(tmp);
    const uint16_t a = freeUdpPort(), b = freeUdpPort(), c = freeUdpPort();
    auto entry = [](const std::string& id, uint16_t port, const std::string& name) {
        return R"({"id":")" + id + R"(","name":")" + name + R"(","type":"pingstation_json","bind":"127.0.0.1","port":)" +
               std::to_string(port) + "}";
    };
    // "ps" + an overlong C0 AF (which the JSON parser lets through) + "b" +
    // U+0085 (a C1 control) + "c".
    const std::string text = R"({"version":1,"inputs":[)" + entry("a", a, "North") + "," +
                             entry("b", b, "ps\xC0\xAF" "b\xC2\x85" "c") + "," + entry("c", c, "South") + "]}";
    handWrite(cfg.inputs_file, text);
    std::string log;
    {
        StderrCapture cap(logdir.path + "/stderr.txt");
        IoThread t;
        Mgr m(t, cfg);
        BOOST_TEST(m.ids() == (std::vector<std::string>{"a", "b", "c"}));
        BOOST_TEST(m.find("a")->def.name == "North");
        BOOST_TEST(m.find("b")->def.name == "ps\xEF\xBF\xBD\xEF\xBF\xBD" "bc");
        BOOST_TEST(m.find("c")->def.name == "South");
        BOOST_TEST(!fileExists(cfg.inputs_file + ".rejected"));
        BOOST_TEST(fileExists(cfg.inputs_file));
        log = cap.text();
    }
    BOOST_TEST(contains(log, "ERROR"), log);
    BOOST_TEST(contains(log, "input 'b': name repaired"), log);
    BOOST_TEST(!contains(log, "input 'a'"), log);
}

BOOST_AUTO_TEST_CASE(capture_without_record_dir_in_file_keeps_inputs)
{
    TempDir tmp, logdir;
    const auto cfg = fileConfig(tmp);  // no record_dir
    const uint16_t a = freeUdpPort(), b = freeUdpPort(), c = freeUdpPort();
    const std::string text = fileOf({psJson("a", a, R"(,"capture":true)"), psJson("b", b)});
    handWrite(cfg.inputs_file, text);
    std::string log;
    {
        StderrCapture cap(logdir.path + "/stderr.txt");
        IoThread t;
        Mgr m(t, cfg);
        BOOST_TEST(m.ids() == (std::vector<std::string>{"a", "b"}));
        BOOST_TEST(m.find("a")->def.capture);
        BOOST_REQUIRE(eventually([&] { return m.state("a").connected; }));
        BOOST_TEST(!fileExists(cfg.inputs_file + ".rejected"));
        BOOST_TEST(readAll(cfg.inputs_file) == text);

        // A hand edit that adds another capture entry is applied too.
        handWrite(cfg.inputs_file, fileOf({psJson("a", a, R"(,"capture":true)"), psJson("b", b),
                                           psJson("c", c, R"(,"capture":true)")}));
        BOOST_REQUIRE(eventuallyWithin(3s, [&] { return m.find("c").has_value(); }));
        BOOST_TEST(!fileExists(cfg.inputs_file + ".rejected"));
        BOOST_TEST(m.ids().size() == 3u);

        // An API write of an unrelated entry still succeeds; one that asks
        // for capture is refused.
        m.on([&](InputManager& im) { return im.setEnabled("b", false); });
        BOOST_CHECK_THROW(m.on([&](InputManager& im) { return im.create(psJson("d", freeUdpPort(), R"(,"capture":true)")); }),
                          InputDefError);
        log = cap.text();
    }
    std::size_t warns = 0;
    const std::string warn = "capture needs recording.dir: not capturing";
    for (auto at = log.find(warn); at != std::string::npos; at = log.find(warn, at + 1))
        ++warns;
    BOOST_TEST(warns == 2u, log);  // the load, the hand edit
    for (const auto& name : dirEntries(tmp.path))
        BOOST_TEST(!contains(name, "capture-"), name);  // nothing captured
}

BOOST_AUTO_TEST_CASE(hand_edit_applied_within_3s)
{
    IoThread t;
    TempDir tmp;
    const auto cfg = fileConfig(tmp);
    Mgr m(t, cfg);
    const uint16_t a = freeUdpPort();
    m.on([&](InputManager& im) { return im.create(psJson("a", a)); });
    BOOST_REQUIRE(eventually([&] { return m.state("a").connected; }));
    const uint16_t b = freeUdpPort();
    handWrite(cfg.inputs_file, fileOf({psJson("a", a), psJson("b", b)}));
    BOOST_REQUIRE(eventuallyWithin(3s, [&] {
        const auto e = m.find("b");
        return e && e->state.connected;
    }));
    BOOST_TEST(m.state("a").counters.connects == 1u);  // unchanged: not restarted

    // A hand edit that removes one and changes another.
    handWrite(cfg.inputs_file, fileOf({psJson("b", b, R"(,"name":"B")")}));
    BOOST_REQUIRE(eventuallyWithin(3s, [&] { return !m.find("a"); }));
    BOOST_TEST(m.find("b")->def.name == "B");
    BOOST_TEST(udpPortFree(a));
}

BOOST_AUTO_TEST_CASE(hand_edit_with_one_bad_entry_keeps_previous_set)
{
    IoThread t;
    TempDir tmp;
    const auto cfg = fileConfig(tmp);
    Mgr m(t, cfg);
    const uint16_t a = freeUdpPort();
    m.on([&](InputManager& im) { return im.create(psJson("a", a)); });
    BOOST_REQUIRE(eventually([&] { return m.state("a").connected; }));
    // a unchanged, c with a port of 0.
    const std::string text = R"({"version":1,"inputs":[)" + skycued::inputDefJson(psJson("a", a)) +
                             R"(,{"id":"c","type":"pingstation_json","port":0}]})";
    handWrite(cfg.inputs_file, text);
    BOOST_REQUIRE(eventuallyWithin(3s, [&] { return fileExists(cfg.inputs_file + ".rejected"); }));
    BOOST_TEST(!fileExists(cfg.inputs_file));
    BOOST_TEST(m.ids() == std::vector<std::string>{"a"});
    BOOST_TEST(m.state("a").connected);
    BOOST_TEST(m.state("a").counters.connects == 1u);

    const uint16_t b = freeUdpPort();
    m.on([&](InputManager& im) { return im.create(psJson("b", b)); });
    const auto file = skycued::parseInputsFile(readAll(cfg.inputs_file));
    BOOST_TEST(file.inputs.size() == 2u);
    BOOST_TEST(readAll(cfg.inputs_file + ".rejected") == text);
}

BOOST_AUTO_TEST_CASE(new_file_after_rejected_is_loaded)
{
    IoThread t;
    TempDir tmp;
    const auto cfg = fileConfig(tmp);
    Mgr m(t, cfg);
    const uint16_t a = freeUdpPort();
    m.on([&](InputManager& im) { return im.create(psJson("a", a)); });
    BOOST_REQUIRE(eventually([&] { return m.state("a").connected; }));
    UdpSender tx;
    tx.send(a, fixture("traffic-1090.json"));
    BOOST_REQUIRE(eventually([&] { return m.state("a").counters.messages == 1; }));

    handWrite(cfg.inputs_file, "{ not json");
    BOOST_REQUIRE(eventuallyWithin(3s, [&] { return fileExists(cfg.inputs_file + ".rejected"); }));
    BOOST_TEST(m.state("a").connected);

    const uint16_t b = freeUdpPort();
    handWrite(cfg.inputs_file, fileOf({psJson("a", a), psJson("b", b)}));
    BOOST_REQUIRE(eventuallyWithin(3s, [&] {
        const auto e = m.find("b");
        return e && e->state.connected;
    }));
    // a ran on through the time without a file: same instance, same counts.
    const InputState s = m.state("a");
    BOOST_TEST(s.connected);
    BOOST_TEST(s.counters.connects == 1u);
    BOOST_TEST(s.counters.messages == 1u);
}

BOOST_AUTO_TEST_CASE(inputs_file_deleted_while_running_keeps_set)
{
    IoThread t;
    TempDir tmp;
    const auto cfg = fileConfig(tmp);
    Mgr m(t, cfg);
    const uint16_t a = freeUdpPort();
    m.on([&](InputManager& im) { return im.create(psJson("a", a)); });
    BOOST_REQUIRE(eventually([&] { return m.state("a").connected; }));
    UdpSender tx;
    tx.send(a, fixture("traffic-1090.json"));
    BOOST_REQUIRE(eventually([&] { return m.state("a").counters.messages == 1; }));

    BOOST_REQUIRE(::unlink(cfg.inputs_file.c_str()) == 0);
    std::this_thread::sleep_for(kPoll * 4);  // the removal settles and is seen
    BOOST_TEST(m.ids() == std::vector<std::string>{"a"});
    InputState s = m.state("a");
    BOOST_TEST(s.connected);
    BOOST_TEST(s.counters.connects == 1u);
    BOOST_TEST(s.counters.messages == 1u);

    const uint16_t b = freeUdpPort();
    handWrite(cfg.inputs_file, fileOf({psJson("a", a), psJson("b", b)}));
    BOOST_REQUIRE(eventually([&] {
        const auto e = m.find("b");
        return e && e->state.connected;
    }));
    s = m.state("a");
    BOOST_TEST(s.counters.connects == 1u);
    BOOST_TEST(s.counters.messages == 1u);
}

BOOST_AUTO_TEST_CASE(replace_with_same_definition_changes_nothing)
{
    IoThread t;
    TempDir tmp;
    const auto cfg = fileConfig(tmp);
    Mgr m(t, cfg);
    const uint16_t port = freeUdpPort();
    const InputDef d = psJson("ps", port, R"(,"name":"North")");
    m.on([&](InputManager& im) { return im.create(d); });
    BOOST_REQUIRE(eventually([&] { return m.state("ps").connected; }));
    const auto stamp = skycued::FileStamp::of(cfg.inputs_file);
    const int changes = m.on([&](InputManager&) { return m.seen.changes; });
    const InputEntry e = m.on([&](InputManager& im) { return im.replace("ps", d); });
    BOOST_TEST(e.state.connected);
    BOOST_TEST(e.state.counters.connects == 1u);
    BOOST_TEST((skycued::FileStamp::of(cfg.inputs_file) == stamp));
    BOOST_TEST(m.on([&](InputManager&) { return m.seen.changes; }) == changes);
}

// A timer wait that completed before the manager was destroyed still runs;
// it must not touch the manager. Each round lets a 1 ms wait expire while the
// io thread is busy, then destroys the manager from the next handler, so the
// completed wait is often already queued behind it.
BOOST_AUTO_TEST_CASE(destroyed_with_pending_timer)
{
    IoThread t;
    TempDir tmp;
    auto cfg = fileConfig(tmp);
    cfg.poll_interval = 1ms;
    for (int round = 0; round < 20; ++round) {
        auto m = std::make_shared<std::unique_ptr<InputManager>>(std::make_unique<InputManager>(t.io, cfg));
        t.call([&] {
            (*m)->load();
            (*m)->start();
        });
        asio::post(t.io, [] { std::this_thread::sleep_for(3ms); });
        asio::post(t.io, [m] { m->reset(); });
        BOOST_TEST(t.call([m] { return !*m; }));
    }
    BOOST_TEST(t.call([] { return 42; }) == 42);  // the io thread ran the queued handlers and goes on
}

// A device status changes device_status only: no onChanged (inputs_changed),
// so a status every second does not resend the inputs list.
BOOST_AUTO_TEST_CASE(device_status_does_not_fire_changed)
{
    IoThread t;
    TempDir tmp;
    Mgr m(t, fileConfig(tmp));
    const uint16_t port = freeUdpPort();
    m.on([&](InputManager& im) { return im.create(psJson("ps", port)); });
    BOOST_REQUIRE(eventually([&] { return m.state("ps").connected; }));
    const int changes = m.on([&](InputManager&) { return m.seen.changes; });
    UdpSender tx;
    tx.send(port, fixture("status.json"));
    BOOST_REQUIRE(eventually([&] { return m.state("ps").device_status.has_value(); }));
    BOOST_TEST(m.on([&](InputManager&) { return m.seen.changes; }) == changes);
}

BOOST_AUTO_TEST_CASE(empty_inputs_file_path_is_in_memory)
{
    IoThread t;
    TempDir tmp, logs;
    char cwd[4096];
    BOOST_REQUIRE(::getcwd(cwd, sizeof cwd));
    BOOST_REQUIRE(::chdir(tmp.path.c_str()) == 0);
    std::string log;
    {
        std::unique_ptr<Mgr> m;
        {
            StderrCapture cap(logs.path + "/stderr");
            m = std::make_unique<Mgr>(t, InputManagerConfig{});
            log = cap.text();
        }
        const uint16_t port = freeUdpPort();
        m->on([&](InputManager& im) { return im.create(psJson("ps", port)); });
        BOOST_REQUIRE(eventually([&] { return m->state("ps").connected; }));
        m->on([](InputManager& im) { return im.setEnabled("ps", false); });
        m->on([](InputManager& im) { im.remove("ps"); });
        BOOST_TEST(m->ids().empty());
    }
    BOOST_REQUIRE(::chdir(cwd) == 0);
    BOOST_TEST(countOf(log, "inputs not persisted (no inputs file)") == 1u);
    BOOST_TEST(contains(log, "WARN"));
    BOOST_TEST(dirEntries(tmp.path).empty());
}

BOOST_AUTO_TEST_CASE(transport_error_reaches_last_error)
{
    IoThread t;
    TempDir tmp;
    Mgr m(t, fileConfig(tmp));
    asio::io_context hio;
    udp::socket holder(hio, udp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const uint16_t port = holder.local_endpoint().port();
    m.on([&](InputManager& im) { return im.create(psJson("ps", port)); });
    BOOST_REQUIRE(eventually([&] { return contains(m.state("ps").last_error, std::to_string(port)); }));
    BOOST_TEST(!m.state("ps").connected);
    BOOST_TEST(m.on([](InputManager& im) { return im.anyTrackInputConnected(); }) == false);
}

BOOST_AUTO_TEST_CASE(write_failure_changes_nothing)
{
    if (::geteuid() == 0) {
        BOOST_TEST_MESSAGE("skipped: root writes into a read-only directory");
        return;
    }
    IoThread t;
    TempDir tmp;
    const std::string dir = tmp.path + "/conf";
    BOOST_REQUIRE(::mkdir(dir.c_str(), 0755) == 0);
    InputManagerConfig cfg;
    cfg.inputs_file = dir + "/skycue.inputs.json";
    cfg.poll_interval = kPoll;
    Mgr m(t, cfg);
    const uint16_t a = freeUdpPort();
    m.on([&](InputManager& im) { return im.create(psJson("a", a)); });
    const std::string before = readAll(cfg.inputs_file);
    BOOST_REQUIRE(::chmod(dir.c_str(), 0555) == 0);
    const uint16_t b = freeUdpPort();
    BOOST_CHECK_THROW(m.on([&](InputManager& im) { return im.create(psJson("b", b)); }), std::runtime_error);
    BOOST_CHECK_THROW(m.on([](InputManager& im) { im.remove("a"); }), std::runtime_error);
    BOOST_CHECK_THROW(m.on([](InputManager& im) { return im.setEnabled("a", false); }), std::runtime_error);
    ::chmod(dir.c_str(), 0755);
    BOOST_TEST(m.ids() == std::vector<std::string>{"a"});
    BOOST_TEST(m.find("a")->def.enabled);
    BOOST_TEST(readAll(cfg.inputs_file) == before);
    BOOST_TEST(udpPortFree(b));
}

BOOST_AUTO_TEST_CASE(replaying_refuses_writes)
{
    IoThread t;
    TempDir tmp;
    auto cfg = fileConfig(tmp);
    cfg.replaying = true;
    handWrite(cfg.inputs_file, fileOf({psJson("a", freeUdpPort())}));
    FakeHub hub;
    Mgr m(t, cfg, [&](InputManager& im) { im.addBuiltin(fakeReadsb(hub)); });
    BOOST_TEST(m.ids() == std::vector<std::string>{"readsb"});  // a replay never starts user inputs
    try {
        m.on([&](InputManager& im) { return im.create(psJson("b", freeUdpPort())); });
        BOOST_FAIL("create while replaying");
    } catch (const InputConflict& e) {
        BOOST_TEST(std::string(e.what()) == "replaying: inputs cannot be changed");
    }
    BOOST_CHECK_THROW(m.on([](InputManager& im) { return im.setEnabled("readsb", false); }), InputConflict);
    BOOST_CHECK_THROW(m.on([](InputManager& im) { im.remove("readsb"); }), InputConflict);
    BOOST_CHECK_THROW(m.on([&](InputManager& im) { return im.replace("a", psJson("a", 4000)); }), InputConflict);
    BOOST_TEST(m.find("readsb")->def.enabled);
}

BOOST_AUTO_TEST_CASE(any_track_input_connected_ignores_dump978_and_disabled)
{
    IoThread t;
    TempDir tmp;
    asio::io_context sio;
    tcp::acceptor acc(sio, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    InputDef uat;
    uat.id = "dump978";
    uat.type = InputType::dump978;
    uat.host = "127.0.0.1";
    uat.port = acc.local_endpoint().port();
    uat.builtin = true;
    FakeHub hub;
    Mgr m(t, fileConfig(tmp), [&](InputManager& im) {
        im.addBuiltin(fakeReadsb(hub));
        im.addBuiltin(std::make_unique<UatRawInput>(t.io, uat));
    });
    tcp::socket server = acc.accept();
    BOOST_REQUIRE(eventually([&] { return m.state("dump978").connected; }));
    BOOST_TEST(!m.on([](InputManager& im) { return im.anyTrackInputConnected(); }));
    BOOST_TEST(m.on([](InputManager& im) { return im.connectedCount(); }) == 1u);

    const uint16_t port = freeUdpPort();
    m.on([&](InputManager& im) { return im.create(psJson("ps", port, R"(,"enabled":false)")); });
    BOOST_TEST(!m.on([](InputManager& im) { return im.anyTrackInputConnected(); }));
    BOOST_TEST(m.on([](InputManager& im) { return im.enabledCount(); }) == 2u);
    m.on([](InputManager& im) { return im.setEnabled("ps", true); });
    BOOST_REQUIRE(eventually([&] { return m.on([](InputManager& im) { return im.anyTrackInputConnected(); }); }));
    BOOST_TEST(m.on([](InputManager& im) { return im.connectedCount(); }) == 2u);

    // dump978 counts uplinks and downlinks.
    asio::write(server, asio::buffer(std::string("+0123;\n-4567;\n")));
    BOOST_REQUIRE(eventually([&] { return m.state("dump978").counters.messages == 2; }));
    BOOST_TEST(m.state("dump978").counters.uplinks == 1u);
    BOOST_TEST(m.state("dump978").counters.bytes == 14u);
}

// ---------------------------------------------------------------- outputs

namespace {

using skycued::Hub;
using skycued::OutputDef;

OutputDef outputOf(const std::string& json)
{
    const auto o = skycued::JsonObject::parse(json);
    BOOST_REQUIRE(o.has_value());
    return skycued::parseOutputDef(*o);
}

OutputDef beastOut(const std::string& id, uint16_t port, const std::string& extra = "")
{
    return outputOf(R"({"id":")" + id + R"(","kind":"beast","transport":"tcp_listen","bind":"127.0.0.1","port":)" +
                    std::to_string(port) + extra + "}");
}

std::string fileWith(const std::vector<InputDef>& inputs, const std::vector<OutputDef>& outputs,
                     const std::map<std::string, bool>& builtin = {})
{
    InputsFile f;
    f.version = 2;
    f.inputs = inputs;
    f.outputs = outputs;
    f.builtin_enabled = builtin;
    return skycued::inputsFileJson(f);
}

// A Hub on the manager's io thread, fed the way main.cpp feeds it.
struct HubWire {
    std::unique_ptr<Hub> hub;
    std::atomic<int> callbacks{0};

    HubWire(Mgr& m, IoThread& t)
    {
        t.call([&] { hub = std::make_unique<Hub>(t.io); });
        m.on([&](InputManager& im) {
            im.onOutputsChanged([this](const std::vector<OutputDef>& defs, const std::map<std::string, bool>& b) {
                ++callbacks;
                hub->apply(defs, b);
            });
            hub->apply(im.outputs(), im.outputBuiltinStates());
            return 0;
        });
    }
    ~HubWire() = default;
    std::optional<skycued::OutputState> find(Mgr& m, const std::string& id)
    {
        return m.on([&](InputManager&) { return hub->find(id); });
    }
};

}  // namespace

BOOST_AUTO_TEST_CASE(outputs_crud_persist_version_2)
{
    IoThread t;
    TempDir tmp;
    const auto cfg = fileConfig(tmp);
    Mgr m(t, cfg);
    HubWire w(m, t);
    const uint16_t port = freeTcpPort();
    const OutputDef o = beastOut("bo", port, R"(,"name":"Beast out")");
    const OutputDef made = m.on([&](InputManager& im) { return im.createOutput(o); });
    BOOST_TEST((made == o));
    auto file = skycued::parseInputsFile(readAll(cfg.inputs_file));
    BOOST_TEST(file.version == 2);
    BOOST_REQUIRE_EQUAL(file.outputs.size(), 1u);
    BOOST_TEST((file.outputs[0] == o));
    BOOST_TEST(w.callbacks.load() == 1);
    BOOST_REQUIRE(eventually([&] {
        const auto s = w.find(m, "bo");
        return s && s->up;
    }));

    // A second output; ids are unique per kind of thing, outputs keep file order.
    BOOST_CHECK_THROW(m.on([&](InputManager& im) { return im.createOutput(o); }), InputConflict);
    const OutputDef j = outputOf(R"({"id":"js","kind":"json","transport":"udp","host":"192.0.2.9","port":4000})");
    m.on([&](InputManager& im) { return im.createOutput(j); });
    file = skycued::parseInputsFile(readAll(cfg.inputs_file));
    BOOST_REQUIRE_EQUAL(file.outputs.size(), 2u);
    BOOST_TEST(file.outputs[1].id == "js");

    // Replace, disable, enable, remove.
    const OutputDef moved = beastOut("bo", freeTcpPort(), R"(,"name":"Moved")");
    m.on([&](InputManager& im) { return im.replaceOutput("bo", moved); });
    BOOST_TEST((skycued::parseInputsFile(readAll(cfg.inputs_file)).outputs[0] == moved));
    BOOST_CHECK_THROW(m.on([&](InputManager& im) { return im.replaceOutput("bo", beastOut("other", 5000)); }),
                      InputDefError);
    BOOST_CHECK_THROW(m.on([&](InputManager& im) { return im.replaceOutput("nope", moved); }), skycued::InputNotFound);
    m.on([](InputManager& im) { im.setOutputEnabled("bo", false); return 0; });
    BOOST_TEST(!skycued::parseInputsFile(readAll(cfg.inputs_file)).outputs[0].enabled);
    BOOST_TEST(!w.find(m, "bo")->up);
    m.on([](InputManager& im) { im.setOutputEnabled("bo", true); return 0; });
    BOOST_REQUIRE(eventually([&] { return w.find(m, "bo")->up; }));
    BOOST_CHECK_THROW(m.on([](InputManager& im) { im.removeOutput("nope"); return 0; }), skycued::InputNotFound);
    m.on([](InputManager& im) { im.removeOutput("bo"); return 0; });
    BOOST_TEST(!w.find(m, "bo").has_value());
    BOOST_TEST(skycued::parseInputsFile(readAll(cfg.inputs_file)).outputs.size() == 1u);
}

BOOST_AUTO_TEST_CASE(last_output_removed_rewrites_v1)
{
    IoThread t;
    TempDir tmp;
    const auto cfg = fileConfig(tmp);
    Mgr m(t, cfg);
    m.on([&](InputManager& im) { return im.create(psJson("ps", freeUdpPort())); });
    BOOST_TEST(skycued::parseInputsFile(readAll(cfg.inputs_file)).version == 1);
    m.on([&](InputManager& im) { return im.createOutput(beastOut("bo", freeTcpPort())); });
    BOOST_TEST(skycued::parseInputsFile(readAll(cfg.inputs_file)).version == 2);
    m.on([](InputManager& im) { im.removeOutput("bo"); return 0; });
    const std::string text = readAll(cfg.inputs_file);
    BOOST_TEST(skycued::parseInputsFile(text).version == 1);
    BOOST_TEST(text.find("outputs") == std::string::npos, text);
    BOOST_TEST(text.find("\"version\": 1") != std::string::npos, text);
}

BOOST_AUTO_TEST_CASE(hand_edit_adds_output_applied_by_id)
{
    IoThread t;
    TempDir tmp;
    const auto cfg = fileConfig(tmp);
    Mgr m(t, cfg);
    HubWire w(m, t);
    const uint16_t a = freeTcpPort(), b = freeTcpPort();
    handWrite(cfg.inputs_file, fileWith({}, {beastOut("bo", a)}));
    BOOST_REQUIRE(eventuallyWithin(3s, [&] {
        const auto s = w.find(m, "bo");
        return s && s->up;
    }));
    {
        tcp::socket client = connectWhenListening(t.io, a);  // t.io is not run on this thread: only a connect
        BOOST_REQUIRE(eventually([&] { return w.find(m, "bo")->counters.peers == 1; }));
    }
    // Same id, another port and a second output: matched by id, the first rebinds.
    handWrite(cfg.inputs_file, fileWith({}, {beastOut("bo", b), beastOut("two", freeTcpPort())}));
    BOOST_REQUIRE(eventuallyWithin(3s, [&] { return w.find(m, "two").has_value(); }));
    BOOST_REQUIRE(eventually([&] { return w.find(m, "bo")->def.port == b && w.find(m, "bo")->up; }));
    BOOST_TEST(m.on([](InputManager& im) { return im.outputs().size(); }) == 2u);
    // Removing one by hand stops it.
    handWrite(cfg.inputs_file, fileWith({}, {beastOut("two", w.find(m, "two")->def.port)}));
    BOOST_REQUIRE(eventuallyWithin(3s, [&] { return !w.find(m, "bo").has_value(); }));
    BOOST_TEST(w.find(m, "two")->up);
}

BOOST_AUTO_TEST_CASE(hand_edit_back_to_v1_rejected_and_outputs_keep_running)
{
    IoThread t;
    TempDir tmp;
    const auto cfg = fileConfig(tmp);
    Mgr m(t, cfg);
    HubWire w(m, t);
    const uint16_t in_port = freeUdpPort();
    m.on([&](InputManager& im) { return im.create(psJson("ps", in_port)); });
    const uint16_t port = freeTcpPort();
    const OutputDef o = beastOut("bo", port);
    m.on([&](InputManager& im) { return im.createOutput(o); });
    BOOST_REQUIRE(eventually([&] { return w.find(m, "bo")->up && m.state("ps").connected; }));

    std::string text = readAll(cfg.inputs_file);
    const auto at = text.find("\"version\": 2");
    BOOST_REQUIRE(at != std::string::npos);
    text.replace(at, 12, "\"version\": 1");
    handWrite(cfg.inputs_file, text);
    BOOST_REQUIRE(eventuallyWithin(3s, [&] { return fileExists(cfg.inputs_file + ".rejected"); }));
    BOOST_TEST(!fileExists(cfg.inputs_file));
    BOOST_TEST(w.find(m, "bo")->up);
    BOOST_TEST((w.find(m, "bo")->def == o));
    BOOST_TEST(m.state("ps").connected);
    BOOST_TEST(m.state("ps").counters.connects == 1u);
    BOOST_TEST(m.on([](InputManager& im) { return im.outputs().size(); }) == 1u);
    tcp::socket client = connectWhenListening(t.io, port);  // still serving
}

BOOST_AUTO_TEST_CASE(invalid_output_edit_renamed_rejected)
{
    IoThread t;
    TempDir tmp;
    const auto cfg = fileConfig(tmp);
    Mgr m(t, cfg);
    HubWire w(m, t);
    const uint16_t port = freeTcpPort();
    m.on([&](InputManager& im) { return im.createOutput(beastOut("bo", port)); });
    BOOST_REQUIRE(eventually([&] { return w.find(m, "bo")->up; }));
    const std::string bad = R"({"version":2,"outputs":[{"id":"bo","kind":"beast","transport":"tcp_listen","port":0}]})";
    handWrite(cfg.inputs_file, bad);
    BOOST_REQUIRE(eventuallyWithin(3s, [&] { return fileExists(cfg.inputs_file + ".rejected"); }));
    BOOST_TEST(readAll(cfg.inputs_file + ".rejected") == bad);
    BOOST_TEST(w.find(m, "bo")->up);
    BOOST_TEST(w.find(m, "bo")->def.port == port);
    // A later API write puts a good file back.
    m.on([&](InputManager& im) { return im.createOutput(beastOut("two", freeTcpPort())); });
    BOOST_TEST(skycued::parseInputsFile(readAll(cfg.inputs_file)).outputs.size() == 2u);
}

BOOST_AUTO_TEST_CASE(output_on_v1_input_port_rejected_input_untouched)
{
    IoThread t;
    TempDir tmp;
    const auto cfg = fileConfig(tmp);
    Mgr m(t, cfg);
    HubWire w(m, t);
    const uint16_t port = freeTcpPort();
    m.on([&](InputManager& im) { return im.create(cat021("cat", "tcp_listen", port)); });
    tcp::socket probe = connectWhenListening(t.io, port);
    try {
        m.on([&](InputManager& im) { return im.createOutput(beastOut("bo", port)); });
        BOOST_FAIL("output on an input's port");
    } catch (const InputDefError& e) {
        BOOST_TEST(std::string(e.what()) == "port " + std::to_string(port) + "/tcp is already used by input cat");
    }
    BOOST_TEST(m.on([](InputManager& im) { return im.outputs().size(); }) == 0u);
    BOOST_TEST(skycued::parseInputsFile(readAll(cfg.inputs_file)).version == 1);

    // The same clash made by hand rejects the whole file; the input keeps running.
    const uint16_t other = freeTcpPort();
    const std::string clash = fileWith({cat021("cat", "tcp_listen", port), cat021("new", "tcp_listen", other)},
                                       {beastOut("bo", port)});
    handWrite(cfg.inputs_file, clash);
    BOOST_REQUIRE(eventuallyWithin(3s, [&] { return fileExists(cfg.inputs_file + ".rejected"); }));
    BOOST_TEST(m.ids() == std::vector<std::string>{"cat"});
    BOOST_TEST(!w.find(m, "bo").has_value());
    tcp::socket client = connectWhenListening(t.io, port);  // the input still accepts
}

BOOST_AUTO_TEST_CASE(output_on_cue_port_rejected)
{
    IoThread t;
    TempDir tmp;
    auto cfg = fileConfig(tmp);
    const uint16_t cue = freeTcpPort(), web = freeTcpPort();
    cfg.reserved_ports = {{"tcp", cue, "the cue stream (cues.tcp_port)"}, {"tcp", web, "the web server (web.port)"}};
    Mgr m(t, cfg);
    try {
        m.on([&](InputManager& im) { return im.createOutput(beastOut("bo", cue)); });
        BOOST_FAIL("output on the cue port");
    } catch (const InputDefError& e) {
        BOOST_TEST(std::string(e.what()) ==
                   "port " + std::to_string(cue) + "/tcp is already used by the cue stream (cues.tcp_port)");
    }
    BOOST_CHECK_THROW(m.on([&](InputManager& im) { return im.createOutput(beastOut("bw", web)); }), InputDefError);
    BOOST_TEST(!fileExists(cfg.inputs_file));
    // A disabled output on the cue port is not a clash.
    m.on([&](InputManager& im) { return im.createOutput(beastOut("off", cue, R"(,"enabled":false)")); });
}

BOOST_AUTO_TEST_CASE(replace_output_with_same_definition_changes_nothing)
{
    IoThread t;
    TempDir tmp;
    const auto cfg = fileConfig(tmp);
    Mgr m(t, cfg);
    HubWire w(m, t);
    const OutputDef o = beastOut("bo", freeTcpPort(), R"(,"name":"Out")");
    m.on([&](InputManager& im) { return im.createOutput(o); });
    const auto stamp = skycued::FileStamp::of(cfg.inputs_file);
    const int callbacks = w.callbacks.load();
    const OutputDef again = m.on([&](InputManager& im) { return im.replaceOutput("bo", o); });
    BOOST_TEST((again == o));
    m.on([](InputManager& im) { im.setOutputEnabled("bo", true); return 0; });  // already enabled
    BOOST_TEST((skycued::FileStamp::of(cfg.inputs_file) == stamp));
    BOOST_TEST(w.callbacks.load() == callbacks);
}

BOOST_AUTO_TEST_CASE(outputs_not_started_and_writes_409_while_replaying)
{
    IoThread t;
    TempDir tmp;
    auto cfg = fileConfig(tmp);
    cfg.replaying = true;
    const uint16_t port = freeTcpPort();
    handWrite(cfg.inputs_file, fileWith({}, {beastOut("bo", port)}));
    FakeHub hub;
    Mgr m(t, cfg, [&](InputManager& im) { im.addBuiltin(fakeReadsb(hub)); });
    HubWire w(m, t);
    BOOST_TEST(m.on([](InputManager& im) { return im.outputs().size(); }) == 0u);
    BOOST_TEST(!w.find(m, "bo").has_value());
    BOOST_CHECK_THROW(m.on([&](InputManager& im) { return im.createOutput(beastOut("two", freeTcpPort())); }),
                      InputConflict);
    BOOST_CHECK_THROW(m.on([&](InputManager& im) { return im.replaceOutput("bo", beastOut("bo", port)); }),
                      InputConflict);
    BOOST_CHECK_THROW(m.on([](InputManager& im) { im.setOutputEnabled("bo", false); return 0; }), InputConflict);
    BOOST_CHECK_THROW(m.on([](InputManager& im) { im.removeOutput("bo"); return 0; }), InputConflict);
    BOOST_TEST(w.callbacks.load() == 0);
    // Nothing listens on the port.
    asio::io_context io;
    boost::system::error_code ec;
    tcp::socket s(io);
    s.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), ec);
    BOOST_TEST(!!ec);
}

// The same WARN on every write is logged once; a changed text is logged again.
BOOST_AUTO_TEST_CASE(warn_logged_once_until_it_changes)
{
    IoThread t;
    TempDir tmp, logs;
    const auto cfg = fileConfig(tmp);
    Mgr m(t, cfg);
    const uint16_t port = freeTcpPort();
    m.on([&](InputManager& im) { return im.create(cat021("cat", "tcp_listen", port)); });
    const auto loop = [&](const std::string& id, uint16_t to) {
        return outputOf(R"({"id":")" + id + R"(","kind":"beast","transport":"tcp_connect","host":"127.0.0.1","port":)" +
                        std::to_string(to) + "}");
    };
    std::string log;
    {
        StderrCapture cap(logs.path + "/stderr");
        m.on([&](InputManager& im) { return im.createOutput(loop("loop", port)); });
        m.on([&](InputManager& im) { return im.createOutput(loop("quiet", freeTcpPort())); });
        m.on([&](InputManager& im) { return im.createOutput(loop("quiet2", freeTcpPort())); });
        m.on([&](InputManager& im) { return im.createOutput(loop("loop2", port)); });
        log = cap.text();
    }
    BOOST_TEST(countOf(log, "output 'loop' sends to 127.0.0.1:") == 1u, log);
    BOOST_TEST(countOf(log, "output 'loop2' sends to 127.0.0.1:") == 1u, log);
}

BOOST_AUTO_TEST_CASE(warn_logged_for_loopback_output_to_own_port)
{
    IoThread t;
    TempDir tmp, logs;
    const auto cfg = fileConfig(tmp);
    Mgr m(t, cfg);
    const uint16_t port = freeTcpPort();
    m.on([&](InputManager& im) { return im.create(cat021("cat", "tcp_listen", port)); });
    std::string log;
    {
        StderrCapture cap(logs.path + "/stderr");
        m.on([&](InputManager& im) {
            return im.createOutput(outputOf(R"({"id":"loop","kind":"beast","transport":"tcp_connect","host":"127.0.0.1","port":)" +
                                            std::to_string(port) + "}"));
        });
        log = cap.text();
    }
    BOOST_TEST(contains(log, "WARN"), log);
    BOOST_TEST(contains(log, "output 'loop' sends to 127.0.0.1:" + std::to_string(port)), log);
    BOOST_TEST(contains(log, "input 'cat'"), log);
    // The write still succeeded.
    BOOST_TEST(m.on([](InputManager& im) { return im.outputs().size(); }) == 1u);
}

BOOST_AUTO_TEST_CASE(builtin_cue_outputs_only_enable_and_disable)
{
    IoThread t;
    TempDir tmp;
    const auto cfg = fileConfig(tmp);
    Mgr m(t, cfg);
    HubWire w(m, t);
    m.on([](InputManager& im) { im.setBuiltinOutputIds({"cue-tcp", "cue-udp-1"}); return 0; });
    BOOST_CHECK_THROW(m.on([&](InputManager& im) { return im.replaceOutput("cue-tcp", beastOut("x", 5000)); }),
                      InputForbidden);
    BOOST_CHECK_THROW(m.on([](InputManager& im) { im.removeOutput("cue-udp-1"); return 0; }), InputForbidden);
    BOOST_CHECK_THROW(m.on([](InputManager& im) { im.setOutputEnabled("cue-udp-9", false); return 0; }),
                      skycued::InputNotFound);
    OutputDef reserved = beastOut("x", 5000);
    reserved.id = "cue-tcp";
    BOOST_CHECK_THROW(m.on([&](InputManager& im) { return im.createOutput(reserved); }), InputDefError);
    BOOST_TEST(!fileExists(cfg.inputs_file));

    m.on([](InputManager& im) { im.setOutputEnabled("cue-tcp", false); return 0; });
    auto file = skycued::parseInputsFile(readAll(cfg.inputs_file));
    BOOST_TEST(file.version == 2);
    BOOST_TEST(file.builtin_enabled.at("cue-tcp") == false);
    BOOST_TEST(w.callbacks.load() == 1);
    const auto states = m.on([](InputManager& im) { return im.outputBuiltinStates(); });
    BOOST_TEST(states.size() == 1u);
    BOOST_TEST(states.at("cue-tcp") == false);
    m.on([](InputManager& im) { im.setOutputEnabled("cue-tcp", false); return 0; });  // no change
    BOOST_TEST(w.callbacks.load() == 1);

    // A hand edit that flips it back is applied.
    handWrite(cfg.inputs_file, R"({"version":2,"inputs":[],"outputs":[],"builtin":{"cue-tcp":{"enabled":true}}})");
    BOOST_REQUIRE(eventuallyWithin(3s, [&] { return w.callbacks.load() == 2; }));
    BOOST_TEST(m.on([](InputManager& im) { return im.outputBuiltinStates(); }).at("cue-tcp") == true);
}

// Enabled is the default: once no cue output is disabled the file is v1 again.
BOOST_AUTO_TEST_CASE(builtin_cue_toggle_round_trip_rewrites_v1)
{
    IoThread t;
    TempDir tmp;
    const auto cfg = fileConfig(tmp);
    Mgr m(t, cfg);
    m.on([](InputManager& im) { im.setBuiltinOutputIds({"cue-tcp"}); return 0; });
    m.on([](InputManager& im) { im.setOutputEnabled("cue-tcp", false); return 0; });
    auto file = skycued::parseInputsFile(readAll(cfg.inputs_file));
    BOOST_TEST(file.version == 2);
    BOOST_TEST(file.builtin_enabled.at("cue-tcp") == false);
    m.on([](InputManager& im) { im.setOutputEnabled("cue-tcp", true); return 0; });
    const std::string text = readAll(cfg.inputs_file);
    file = skycued::parseInputsFile(text);
    BOOST_TEST(file.version == 1);
    BOOST_TEST(file.builtin_enabled.count("cue-tcp") == 0u);
    BOOST_TEST(!contains(text, "cue-tcp"), text);
}

// The file's "builtin" object names the cue outputs too; it must not touch
// the built-in inputs it does not mention, with or without outputs, and a
// replay reads no file at all.
BOOST_AUTO_TEST_CASE(builtin_dump978_stays_enabled_when_the_file_does_not_disable_it)
{
    for (const bool replaying : {false, true}) {
        IoThread t;
        TempDir tmp;
        auto cfg = fileConfig(tmp);
        cfg.replaying = replaying;
        const uint16_t port = freeTcpPort();
        handWrite(cfg.inputs_file, fileWith({}, {beastOut("bo", port)}, {{"cue-tcp", false}, {"cue-udp-1", false}}));
        InputDef uat;
        uat.id = "dump978";
        uat.type = InputType::dump978;
        uat.host = "127.0.0.1";
        uat.port = 1;
        uat.builtin = true;
        FakeHub hub;
        Mgr m(t, cfg, [&](InputManager& im) {
            im.addBuiltin(fakeReadsb(hub));
            im.addBuiltin(std::make_unique<UatRawInput>(t.io, uat));
        });
        BOOST_TEST_CONTEXT("replaying=" << replaying)
        {
            BOOST_TEST(m.find("dump978")->def.enabled);
            BOOST_TEST(m.find("readsb")->def.enabled);
            BOOST_TEST(m.on([](InputManager& im) { return im.enabledCount(); }) == 2u);
            if (!replaying) {
                // The cue states are the outputs' business only.
                const auto states = m.on([](InputManager& im) { return im.outputBuiltinStates(); });
                BOOST_TEST(states.size() == 2u);
                BOOST_TEST(!states.at("cue-tcp"));
            }
        }
    }
}

// inputs.decoder: the built-in readsb.
BOOST_AUTO_TEST_CASE(builtin_readsb_becomes_beast_raw)
{
    skycued::Config c;
    InputDef d = skycued::builtinReadsbDef(c, "192.0.2.9");
    BOOST_TEST(d.id == "readsb");
    BOOST_TEST((d.type == InputType::readsb));
    BOOST_TEST(d.host == "192.0.2.9");
    BOOST_TEST(d.port == 30047);
    c.track_decoder = "skycued";
    d = skycued::builtinReadsbDef(c, "192.0.2.9");
    BOOST_TEST(d.id == "readsb");
    BOOST_TEST((d.type == InputType::beast_raw));
    BOOST_TEST((d.transport == skycued::InputTransport::tcp_connect));
    BOOST_TEST(d.host == "127.0.0.1");
    BOOST_TEST(d.port == 30005);
    BOOST_TEST(d.format == "beast");
    c.readsb_beast_port = 31005;
    BOOST_TEST(skycued::builtinReadsbDef(c, "h").port == 31005);

    // Registered as a built-in it keeps its id and its enabled state.
    IoThread t;
    TempDir tmp;
    auto cfg = fileConfig(tmp);
    handWrite(cfg.inputs_file, fileWith({}, {}, {{"readsb", false}}));
    Mgr m(t, cfg, [&](InputManager& im) { im.addBuiltin(std::make_unique<skycued::BeastRawInput>(t.io, d)); });
    const auto e = m.find("readsb");
    BOOST_REQUIRE(e.has_value());
    BOOST_TEST((e->def.type == InputType::beast_raw));
    BOOST_TEST(e->def.builtin);
    BOOST_TEST(!e->def.enabled);
}

BOOST_AUTO_TEST_CASE(input_reference_is_own_else_receiver_else_none)
{
    InputManagerConfig cfg;
    InputDef d;
    d.type = InputType::beast_raw;
    BOOST_TEST(!skycued::inputReference(d, cfg).has_value());
    cfg.receiver_ref = std::make_pair(51.5, -0.125);
    BOOST_TEST((*skycued::inputReference(d, cfg) == std::make_pair(51.5, -0.125)));
    d.ref_lat = 10.0;
    d.ref_lon = 20.0;
    BOOST_TEST((*skycued::inputReference(d, cfg) == std::make_pair(10.0, 20.0)));
}

BOOST_AUTO_TEST_CASE(skycued_with_readsb_input_warns)
{
    IoThread t;
    TempDir tmp, logs;
    skycued::Config c;
    c.track_decoder = "skycued";
    auto cfg = fileConfig(tmp);
    cfg.decode_raw = true;
    InputDef rs;
    rs.id = "rs2";
    rs.type = InputType::readsb;
    rs.host = "192.0.2.1";
    rs.port = 30047;
    handWrite(cfg.inputs_file, fileWith({rs}, {}));
    std::string log;
    {
        StderrCapture cap(logs.path + "/stderr");
        Mgr m(t, cfg, [&](InputManager& im) {
            im.addBuiltin(std::make_unique<skycued::BeastRawInput>(t.io, skycued::builtinReadsbDef(c, "h")));
        });
        log = cap.text();
    }
    BOOST_TEST(countOf(log, "both feed the track table") == 1u, log);
    BOOST_TEST(contains(log, "WARN"), log);
    BOOST_TEST(contains(log, "'rs2'"), log);
    BOOST_TEST(contains(log, "'readsb'"), log);

    // The pure function: nothing without a user readsb input, nothing for a disabled one.
    InputDef b = skycued::builtinReadsbDef(c, "h");
    BOOST_TEST(skycued::decoderWarnings({b}, {}).empty());
    rs.enabled = false;
    BOOST_TEST(skycued::decoderWarnings({b, rs}, {}).empty());
    rs.enabled = true;
    const auto w = skycued::decoderWarnings({b, rs}, {});
    BOOST_REQUIRE(w.size() == 1u);
    BOOST_TEST(contains(w[0], "'rs2'"));
    BOOST_TEST(contains(w[0], "'readsb'"));

    // Under inputs.decoder=readsb the manager adds none of this.
    IoThread t2;
    TempDir tmp2, logs2;
    auto cfg2 = fileConfig(tmp2);
    handWrite(cfg2.inputs_file, fileWith({rs}, {}));
    FakeHub hub;
    std::string log2;
    {
        StderrCapture cap(logs2.path + "/stderr");
        Mgr m(t2, cfg2, [&](InputManager& im) { im.addBuiltin(fakeReadsb(hub)); });
        log2 = cap.text();
    }
    BOOST_TEST(!contains(log2, "inputs.decoder"), log2);
}

BOOST_AUTO_TEST_CASE(json_output_idle_under_skycued_warns)
{
    IoThread t;
    TempDir tmp, logs;
    skycued::Config c;
    c.track_decoder = "skycued";
    auto cfg = fileConfig(tmp);
    cfg.decode_raw = true;
    const OutputDef js = outputOf(R"({"id":"js","kind":"json","transport":"tcp_listen","bind":"127.0.0.1","port":)" +
                                  std::to_string(freeTcpPort()) + "}");
    handWrite(cfg.inputs_file, fileWith({}, {js}));
    std::string log;
    {
        StderrCapture cap(logs.path + "/stderr");
        Mgr m(t, cfg, [&](InputManager& im) {
            im.addBuiltin(std::make_unique<skycued::BeastRawInput>(t.io, skycued::builtinReadsbDef(c, "h")));
        });
        log = cap.text();
        // Defined and enabled all the same.
        const auto outs = m.on([](InputManager& im) { return im.outputs(); });
        BOOST_REQUIRE(outs.size() == 1u);
        BOOST_TEST(outs[0].enabled);
    }
    BOOST_TEST(countOf(log, "the json output 'js' carries nothing") == 1u, log);

    InputDef b = skycued::builtinReadsbDef(c, "h");
    BOOST_TEST(skycued::decoderWarnings({b}, {js}).size() == 1u);
    OutputDef off = js;
    off.enabled = false;
    BOOST_TEST(skycued::decoderWarnings({b}, {off}).empty());
    OutputDef beast = js;
    beast.kind = skycued::OutputKind::beast;
    BOOST_TEST(skycued::decoderWarnings({b}, {beast}).empty());
    InputDef rs;
    rs.id = "rs2";
    rs.type = InputType::readsb;
    BOOST_TEST(skycued::decoderWarnings({rs}, {js}).empty());  // a readsb JSON input feeds it
}
