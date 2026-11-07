#include "skycue/net/fanout.hpp"

#include <algorithm>
#include <vector>

namespace skycue::net {

struct Fanout::State {
    std::vector<std::shared_ptr<Listener>> listeners;
    std::vector<std::shared_ptr<Dialer>> dialers;
    std::vector<std::shared_ptr<UdpSender>> senders;
    std::function<void(const std::function<void(Buf)>&)> up;
    std::function<void(uint64_t)> down;
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

Fanout::Fanout(boost::asio::io_context&) : st_(std::make_shared<State>()) {}

Fanout::~Fanout() = default;

void Fanout::attach(std::shared_ptr<Listener> l)
{
    if (!l || contains(st_->listeners, l))
        return;
    st_->listeners.push_back(l);
    std::weak_ptr<State> weak = st_;
    std::weak_ptr<Listener> wl = l;
    l->onPeer([weak, wl](uint64_t stream, bool up, const boost::asio::ip::tcp::endpoint&) {
        auto st = weak.lock();
        if (!st)
            return;
        if (up) {
            auto h = st->up; // a copy: the hook may re-register
            auto lp = wl.lock();
            if (h && lp) {
                std::function<void(Buf)> to = [lp, stream](Buf b) { lp->send(stream, std::move(b)); };
                h(to);
            }
        } else {
            auto h = st->down;
            if (h)
                h(stream);
        }
    });
}

void Fanout::attach(std::shared_ptr<Dialer> d)
{
    if (d && !contains(st_->dialers, d))
        st_->dialers.push_back(std::move(d));
}

void Fanout::attach(std::shared_ptr<UdpSender> u)
{
    if (u && !contains(st_->senders, u))
        st_->senders.push_back(std::move(u));
}

void Fanout::detach(const std::shared_ptr<Listener>& l)
{
    if (!l || !contains(st_->listeners, l))
        return;
    erase(st_->listeners, l);
    l->onPeer({});
}

void Fanout::detach(const std::shared_ptr<Dialer>& d) { erase(st_->dialers, d); }

void Fanout::detach(const std::shared_ptr<UdpSender>& u) { erase(st_->senders, u); }

void Fanout::publish(Buf b)
{
    if (!b)
        return;
    // Copies, so a destination cannot invalidate the iteration.
    const auto ls = st_->listeners;
    const auto ds = st_->dialers;
    const auto us = st_->senders;
    for (auto& l : ls)
        l->sendAll(b);
    for (auto& d : ds)
        d->send(b);
    for (auto& u : us)
        u->send(b);
}

void Fanout::onPeerUp(std::function<void(const std::function<void(Buf)>&)> h) { st_->up = std::move(h); }

void Fanout::onPeerDown(std::function<void(uint64_t)> h) { st_->down = std::move(h); }

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
    for (auto& l : st_->listeners)
        add(l->counters());
    for (auto& d : st_->dialers)
        add(d->counters());
    for (auto& u : st_->senders)
        add(u->counters());
    return c;
}

std::size_t Fanout::destinations() const
{
    return st_->listeners.size() + st_->dialers.size() + st_->senders.size();
}

} // namespace skycue::net
