#include "skycue/net/fanout.hpp"

#include <algorithm>
#include <map>
#include <vector>

namespace skycue::net {

namespace {
struct ListenerEntry {
    std::shared_ptr<Listener> l;
    std::map<uint64_t, uint64_t> ids; // listener stream -> Fanout peer id
};
struct DialerEntry {
    std::shared_ptr<Dialer> d;
    uint64_t id = 0; // peer id of the current connection; 0 when not connected
};
} // namespace

struct Fanout::State {
    std::vector<std::shared_ptr<ListenerEntry>> listeners;
    std::vector<std::shared_ptr<DialerEntry>> dialers;
    std::vector<std::shared_ptr<UdpSender>> senders;
    PeerUpHook up;
    std::function<void(uint64_t)> down;
    uint64_t next_peer = 1;
};

namespace {

template <class T>
bool contains(const std::vector<std::shared_ptr<T>>& v, const std::shared_ptr<T>& p)
{
    return std::find(v.begin(), v.end(), p) != v.end();
}

template <class T>
void erase(std::vector<std::shared_ptr<T>>& v, const std::shared_ptr<T>& p)
{
    v.erase(std::remove(v.begin(), v.end(), p), v.end());
}

} // namespace

Fanout::Fanout() : st_(std::make_shared<State>()) {}
Fanout::Fanout(boost::asio::io_context&) : Fanout() {}

Fanout::~Fanout() = default;

void Fanout::attach(const std::shared_ptr<Listener>& l)
{
    if (!l)
        return;
    for (const auto& e : st_->listeners)
        if (e->l == l)
            return;
    auto entry = std::make_shared<ListenerEntry>();
    entry->l = l;
    st_->listeners.push_back(entry);
    std::weak_ptr<State> weak = st_;
    std::weak_ptr<ListenerEntry> we = entry;
    l->onPeer([weak, we](uint64_t stream, bool up, const boost::asio::ip::tcp::endpoint& who) {
        auto st = weak.lock();
        auto e = we.lock();
        if (!st || !e)
            return;
        if (up) {
            const uint64_t id = st->next_peer++;
            e->ids[stream] = id;
            auto h = st->up; // a copy: the hook may re-register
            if (h) {
                const std::shared_ptr<Listener> lp = e->l;
                const std::function<void(const Buf&)> to = [lp, stream](const Buf& b) { lp->send(stream, b); };
                h(id, who, to);
            }
        } else {
            const auto it = e->ids.find(stream);
            if (it == e->ids.end())
                return;
            const uint64_t id = it->second;
            e->ids.erase(it);
            auto h = st->down;
            if (h)
                h(id);
        }
    });
}

void Fanout::attach(const std::shared_ptr<Dialer>& d)
{
    if (!d)
        return;
    for (const auto& e : st_->dialers)
        if (e->d == d)
            return;
    auto entry = std::make_shared<DialerEntry>();
    entry->d = d;
    st_->dialers.push_back(entry);
    std::weak_ptr<State> weak = st_;
    std::weak_ptr<DialerEntry> we = entry;
    d->onConnection([weak, we](bool up) {
        auto st = weak.lock();
        auto e = we.lock();
        if (!st || !e)
            return;
        if (up) {
            e->id = st->next_peer++;
            auto h = st->up;
            if (h) {
                const std::shared_ptr<Dialer> dp = e->d;
                const std::function<void(const Buf&)> to = [dp](const Buf& b) { dp->send(b); };
                h(e->id, e->d->peerEndpoint(), to);
            }
        } else if (e->id != 0) {
            const uint64_t id = e->id;
            e->id = 0;
            auto h = st->down;
            if (h)
                h(id);
        }
    });
}

void Fanout::attach(std::shared_ptr<UdpSender> u)
{
    if (u && !contains(st_->senders, u))
        st_->senders.push_back(std::move(u));
}

void Fanout::detach(const std::shared_ptr<Listener>& l)
{
    if (!l)
        return;
    auto& v = st_->listeners;
    const auto it = std::find_if(v.begin(), v.end(), [&](const auto& e) { return e->l == l; });
    if (it == v.end())
        return;
    v.erase(it);
    l->onPeer({});
}

void Fanout::detach(const std::shared_ptr<Dialer>& d)
{
    if (!d)
        return;
    auto& v = st_->dialers;
    const auto it = std::find_if(v.begin(), v.end(), [&](const auto& e) { return e->d == d; });
    if (it == v.end())
        return;
    v.erase(it);
    d->onConnection({});
}

void Fanout::detach(const std::shared_ptr<UdpSender>& u) { erase(st_->senders, u); }

void Fanout::publish(const Buf& b)
{
    if (!b)
        return;
    // Copies, so a destination cannot invalidate the iteration.
    const auto ls = st_->listeners;
    const auto ds = st_->dialers;
    const auto us = st_->senders;
    for (auto& e : ls)
        e->l->sendAll(b);
    for (auto& e : ds)
        e->d->send(b);
    for (auto& u : us)
        u->send(b);
}

void Fanout::onPeerUp(PeerUpHook h) { st_->up = std::move(h); }

void Fanout::onPeerDown(std::function<void(uint64_t)> h) { st_->down = std::move(h); }

std::optional<Fanout::PeerInfo> Fanout::peerInfo(uint64_t peer) const
{
    for (const auto& e : st_->listeners)
        for (const auto& kv : e->ids)
            if (kv.second == peer) {
                const auto ps = e->l->peerStats(kv.first);
                if (!ps)
                    return std::nullopt;
                return PeerInfo{*ps, false, e->l.get()};
            }
    for (const auto& e : st_->dialers)
        if (e->id == peer && e->d->connected())
            return PeerInfo{e->d->peerStats(), true, e->d.get()};
    return std::nullopt;
}

Counters Fanout::counters() const
{
    Counters c;
    auto add = [&c](const Counters& x) {
        c.messages_out += x.messages_out;
        c.bytes_out += x.bytes_out;
        c.dropped += x.dropped;
        c.peers += x.peers;
        if (c.last_error.empty())
            c.last_error = x.last_error;
    };
    for (auto& e : st_->listeners)
        add(e->l->counters());
    for (auto& e : st_->dialers)
        add(e->d->counters());
    for (auto& u : st_->senders)
        add(u->counters());
    return c;
}

std::size_t Fanout::destinations() const
{
    return st_->listeners.size() + st_->dialers.size() + st_->senders.size();
}

} // namespace skycue::net
