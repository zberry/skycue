#include "hub/hub.hpp"

#include <skycue/net/dialer.hpp>
#include <skycue/net/endpoint.hpp>
#include <skycue/net/fanout.hpp>
#include <skycue/net/listener.hpp>
#include <skycue/net/udp_sender.hpp>

#include <boost/asio/steady_timer.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <utility>

namespace skycued {

namespace {

namespace net = skycue::net;

constexpr std::size_t kUdpCap = 65507;
constexpr auto kPoll = std::chrono::seconds(1);

std::optional<RawKind> rawKindOf(OutputKind kind)
{
    switch (kind) {
    case OutputKind::beast: return RawKind::beast;
    case OutputKind::uat: return RawKind::uat;
    case OutputKind::json: return RawKind::json;
    case OutputKind::pingstation_json: return RawKind::pingstation_json;
    case OutputKind::cat021: return RawKind::cat021;
    case OutputKind::cue: return std::nullopt;
    }
    return std::nullopt;
}

bool isLineKind(RawKind kind)
{
    return kind == RawKind::uat || kind == RawKind::json || kind == RawKind::pingstation_json;
}

std::size_t busIndex(RawKind kind) { return static_cast<std::size_t>(kind); }

}  // namespace

struct Hub::Impl {
    struct Bus {
        net::Fanout stream, datagram;
    };

    struct Output {
        OutputDef def;
        std::shared_ptr<net::Listener> listener;
        std::shared_ptr<net::Dialer> dialer;
        std::shared_ptr<net::UdpSender> sender;
        bool running = false;
        std::string start_error;
        OutputCounters base;  // totals of the destinations stopped so far, plus hub-side drops
        bool seen_up = false;
        std::string seen_error;

        bool isStream() const { return def.transport != InputTransport::udp; }
    };

    Impl(boost::asio::io_context& io_, Forwarder* cue_) : io(io_), cue(cue_), timer(io_)
    {
        for (auto& bus : buses) {
            auto changed = [this] { refresh(); };
            bus.stream.onPeerUp([changed](uint64_t, const boost::asio::ip::tcp::endpoint&,
                                          const std::function<void(const net::Buf&)>&) { changed(); });
            bus.stream.onPeerDown([changed](uint64_t, net::DownReason, const std::string&) { changed(); });
        }
    }

    boost::asio::io_context& io;
    Forwarder* cue;  // the cue outputs come with the forwarder
    std::array<Bus, 5> buses;
    std::vector<std::unique_ptr<Output>> outputs;
    std::function<void()> on_changed;
    boost::asio::steady_timer timer;
    bool stopped = false;

    Output* findOutput(const std::string& id) const
    {
        for (const auto& o : outputs)
            if (o->def.id == id)
                return o.get();
        return nullptr;
    }

    Bus* busOf(const OutputDef& def)
    {
        const auto kind = rawKindOf(def.kind);
        return kind ? &buses[busIndex(*kind)] : nullptr;
    }

    net::Fanout& fanoutOf(Output& o) { return o.isStream() ? busOf(o.def)->stream : busOf(o.def)->datagram; }

    static bool upOf(const Output& o)
    {
        if (!o.running)
            return false;
        if (o.listener)
            return o.listener->listening();
        if (o.dialer)
            return o.dialer->connected();
        if (o.sender)
            return o.sender->targets() > 0;
        return false;
    }

    static std::string errorOf(const Output& o)
    {
        std::string live;
        if (o.listener)
            live = o.listener->counters().last_error;
        else if (o.dialer)
            live = o.dialer->counters().last_error;
        else if (o.sender)
            live = o.sender->counters().last_error;
        if (!live.empty())
            return live;
        return o.start_error;
    }

    static OutputCounters countersOf(const Output& o)
    {
        OutputCounters c = o.base;
        const net::Counters* live = nullptr;
        if (o.listener) {
            live = &o.listener->counters();
            c.peers = o.listener->peers();
        } else if (o.dialer) {
            live = &o.dialer->counters();
            c.peers = o.dialer->connected() ? 1 : 0;
        } else if (o.sender) {
            live = &o.sender->counters();
            c.peers = 0;
        }
        if (live) {
            c.bytes += live->bytes_out;
            c.messages += live->messages_out;
            c.dropped += live->dropped;
        }
        c.last_error = errorOf(o);
        return c;
    }

    void start(Output& o)
    {
        o.start_error.clear();
        if (o.def.kind == OutputKind::cue) {
            o.start_error = "cue outputs need the forwarder";
            return;
        }
        try {
            const net::Endpoint ep = toEndpoint(o.def);
            const std::string name = "output " + o.def.id;
            switch (o.def.transport) {
            case InputTransport::tcp_listen: {
                net::ListenOptions opts;
                opts.peer = ep.peer;
                opts.fail_fast = false;
                opts.tcp.name = name;
                o.listener = net::Listener::create(io, ep.bind, ep.port, opts);
                fanoutOf(o).attach(o.listener);
                o.running = true;
                o.listener->start();
                break;
            }
            case InputTransport::tcp_connect: {
                net::TcpOptions opts;
                opts.name = name;
                o.dialer = net::Dialer::create(io, ep.host, ep.port, opts);
                fanoutOf(o).attach(o.dialer);
                o.running = true;
                o.dialer->start();
                break;
            }
            case InputTransport::udp: {
                o.sender = net::UdpSender::create(io, kUdpCap, name);
                net::UdpTarget target;
                target.host = ep.group.empty() ? ep.host : ep.group;
                target.port = ep.port;
                target.iface = ep.iface;
                target.ttl = ep.ttl;
                target.loopback = ep.loopback;
                o.sender->addTarget(target);
                fanoutOf(o).attach(o.sender);
                o.running = true;
                o.sender->start();
                break;
            }
            }
        } catch (const std::exception& e) {
            o.start_error = e.what();
            if (o.start_error.empty())
                o.start_error = "start failed";
        }
    }

    // Detach first, then stop: the fanout never sees a half-stopped destination.
    void stopOutput(Output& o)
    {
        if (o.listener) {
            fanoutOf(o).detach(o.listener);
            o.listener->stop();
        }
        if (o.dialer) {
            fanoutOf(o).detach(o.dialer);
            o.dialer->stop();
        }
        if (o.sender) {
            fanoutOf(o).detach(o.sender);
            o.sender->stop();
        }
        if (o.listener || o.dialer || o.sender) {
            OutputCounters live = countersOf(o);
            live.peers = 0;
            live.last_error.clear();
            o.base = live;
        }
        o.listener.reset();
        o.dialer.reset();
        o.sender.reset();
        o.running = false;
        o.start_error.clear();
    }

    // Fires the callback when an output's up flag or error differs from what
    // was last reported. Returns whether it fired.
    bool refresh(bool force = false)
    {
        bool changed = force;
        for (const auto& o : outputs) {
            const bool up = upOf(*o);
            const std::string err = errorOf(*o);
            if (up != o->seen_up || err != o->seen_error) {
                o->seen_up = up;
                o->seen_error = err;
                changed = true;
            }
        }
        if (changed && on_changed && !stopped)
            on_changed();
        return changed;
    }

    void armTimer()
    {
        if (stopped || outputs.empty())
            return;
        timer.expires_after(kPoll);
        timer.async_wait([this](const boost::system::error_code& ec) {
            if (ec)
                return;
            refresh();
            armTimer();
        });
    }

    OutputState stateOf(const Output& o) const
    {
        OutputState s;
        s.def = o.def;
        s.up = upOf(o);
        s.counters = countersOf(o);
        return s;
    }
};

Hub::Hub(boost::asio::io_context& io, Forwarder* cue) : impl_(std::make_unique<Impl>(io, cue)) {}

Hub::~Hub()
{
    try {
        stop();
    } catch (...) {  // a destructor does not throw
        impl_->stopped = true;
    }
}

void Hub::publish(const RawMessage& message)
{
    Impl& im = *impl_;
    if (im.stopped || !message.bytes)
        return;
    auto& bus = im.buses[busIndex(message.kind)];
    if (bus.datagram.destinations() > 0)
        bus.datagram.publish(message.bytes);
    if (bus.stream.destinations() == 0)
        return;
    if (!isLineKind(message.kind)) {
        bus.stream.publish(message.bytes);
        return;
    }
    const auto& raw = *message.bytes;
    if (message.kind == RawKind::pingstation_json &&
        std::any_of(raw.begin(), raw.end(), [](uint8_t c) { return c == '\n' || c == '\r'; })) {
        // One line per message on a stream: a text with a line break would split.
        for (const auto& o : im.outputs)
            if (o->isStream() && o->def.enabled && o->def.kind == OutputKind::pingstation_json)
                ++o->base.dropped;
        return;
    }
    std::vector<uint8_t> line;
    line.reserve(raw.size() + 1);
    line.insert(line.end(), raw.begin(), raw.end());
    line.push_back('\n');
    bus.stream.publish(std::make_shared<const std::vector<uint8_t>>(std::move(line)));
}

bool Hub::wants(RawKind kind) const
{
    if (impl_->stopped)
        return false;
    return std::any_of(impl_->outputs.begin(), impl_->outputs.end(), [kind](const auto& o) {
        return o->def.enabled && rawKindOf(o->def.kind) == kind;
    });
}

void Hub::apply(const std::vector<OutputDef>& desired, const std::map<std::string, bool>& builtin_enabled)
{
    Impl& im = *impl_;
    if (im.stopped)
        return;
    bool changed = false;
    std::vector<std::unique_ptr<Impl::Output>> next;
    next.reserve(desired.size());
    for (const auto& given : desired) {
        OutputDef def = given;
        if (def.builtin) {
            const auto it = builtin_enabled.find(def.id);
            if (it != builtin_enabled.end())
                def.enabled = it->second;
        }
        std::unique_ptr<Impl::Output> out;
        for (auto& existing : im.outputs)
            if (existing && existing->def.id == def.id) {
                out = std::move(existing);
                break;
            }
        if (!out) {
            out = std::make_unique<Impl::Output>();
            out->def = def;
            if (def.enabled)
                im.start(*out);
            changed = true;
        } else if (out->def != def) {
            im.stopOutput(*out);
            out->def = def;
            if (def.enabled)
                im.start(*out);
            changed = true;
        }
        next.push_back(std::move(out));
    }
    for (auto& left : im.outputs)
        if (left) {
            im.stopOutput(*left);
            changed = true;
        }
    std::stable_partition(next.begin(), next.end(), [](const auto& o) { return o->def.builtin; });
    im.outputs = std::move(next);
    im.refresh(changed);
    if (!im.outputs.empty())
        im.armTimer();
}

std::vector<OutputState> Hub::list() const
{
    std::vector<OutputState> states;
    states.reserve(impl_->outputs.size());
    for (const auto& o : impl_->outputs)
        states.push_back(impl_->stateOf(*o));
    return states;
}

std::optional<OutputState> Hub::find(const std::string& id) const
{
    const auto* o = impl_->findOutput(id);
    if (!o)
        return std::nullopt;
    return impl_->stateOf(*o);
}

void Hub::onChanged(std::function<void()> callback) { impl_->on_changed = std::move(callback); }

void Hub::stop()
{
    Impl& im = *impl_;
    if (im.stopped)
        return;
    im.stopped = true;
    im.timer.cancel();
    for (auto& o : im.outputs)
        im.stopOutput(*o);
}

}  // namespace skycued
