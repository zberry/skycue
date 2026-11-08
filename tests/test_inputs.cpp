#define BOOST_TEST_MODULE skycued_inputs
#include <boost/test/unit_test.hpp>


#include "asterix/cat021.hpp"
#include "asterix/cat021_encode.hpp"
#include "engine_test_support.hpp"
#include "file_util.hpp"
#include "inputs/input.hpp"
#include "inputs/input_def.hpp"
#include "inputs/input_manager.hpp"
#include "inputs/inputs_file.hpp"
#include "inputs/ping_cat021_input.hpp"
#include "inputs/ping_json_input.hpp"
#include "inputs/source_input.hpp"
#include "inputs/uat_raw_input.hpp"
#include "json_lite.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cmath>

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

bool exists(const std::string& path) { return ::access(path.c_str(), F_OK) == 0; }

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
        BOOST_TEST(std::string(e.what()) == "capture needs RECORD_DIR");
    }
    InputDef reserved = psJson("ps", freeUdpPort());
    reserved.id = "readsb";
    BOOST_CHECK_THROW(m.on([&](InputManager& im) { return im.create(reserved); }), InputDefError);
    BOOST_TEST(m.ids().empty());
    BOOST_TEST(!exists(cfg.inputs_file));
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
    BOOST_TEST(!exists(cfg.inputs_file));
    m.on([](InputManager& im) { return im.setEnabled("readsb", true); });  // no change
    BOOST_TEST(!exists(cfg.inputs_file));
    m.on([](InputManager& im) { return im.setEnabled("readsb", false); });
    BOOST_TEST(exists(cfg.inputs_file));
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
    BOOST_TEST(!exists(cfg.inputs_file));
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
        BOOST_TEST(!exists(cfg.inputs_file + ".rejected"));
        BOOST_TEST(exists(cfg.inputs_file));
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
        BOOST_TEST(!exists(cfg.inputs_file + ".rejected"));
        BOOST_TEST(readAll(cfg.inputs_file) == text);

        // A hand edit that adds another capture entry is applied too.
        handWrite(cfg.inputs_file, fileOf({psJson("a", a, R"(,"capture":true)"), psJson("b", b),
                                           psJson("c", c, R"(,"capture":true)")}));
        BOOST_REQUIRE(eventuallyWithin(3s, [&] { return m.find("c").has_value(); }));
        BOOST_TEST(!exists(cfg.inputs_file + ".rejected"));
        BOOST_TEST(m.ids().size() == 3u);

        // An API write of an unrelated entry still succeeds; one that asks
        // for capture is refused.
        m.on([&](InputManager& im) { return im.setEnabled("b", false); });
        BOOST_CHECK_THROW(m.on([&](InputManager& im) { return im.create(psJson("d", freeUdpPort(), R"(,"capture":true)")); }),
                          InputDefError);
        log = cap.text();
    }
    std::size_t warns = 0;
    const std::string warn = "capture needs RECORD_DIR: not capturing";
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
    BOOST_REQUIRE(eventuallyWithin(3s, [&] { return exists(cfg.inputs_file + ".rejected"); }));
    BOOST_TEST(!exists(cfg.inputs_file));
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
    BOOST_REQUIRE(eventuallyWithin(3s, [&] { return exists(cfg.inputs_file + ".rejected"); }));
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
