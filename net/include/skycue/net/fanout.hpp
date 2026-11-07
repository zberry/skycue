#pragma once

// Fanout: the publish side. It owns a dynamic set of destinations (TCP
// listeners, outbound dialers, UDP senders) and delivers one Buf to every one
// of them. All members run on the io_context's thread (it must be run by one
// thread), like the destinations themselves.
//
// publish() never blocks and never throws for a destination's failure: a slow
// or absent destination drops (and counts) on its own. detach() stops delivery
// to that destination at once. Destinations are held by shared_ptr; Fanout
// does not start or stop them.
//
// Peer events: attach(Listener) installs the listener's onPeer handler.
// Fanout owns it, so a host that needs peer events registers them through
// Fanout::onPeerUp / onPeerDown, not through Listener::onPeer (a handler set
// there afterwards would replace Fanout's). detach(Listener) installs an empty
// handler again. The handler refers to Fanout's state only weakly, so it is
// safe if the Fanout is destroyed first, but a host should still detach
// listeners before destroying the Fanout.

#include "skycue/net/buffer.hpp"
#include "skycue/net/counters.hpp"
#include "skycue/net/dialer.hpp"
#include "skycue/net/listener.hpp"
#include "skycue/net/udp_sender.hpp"

#include <boost/asio/io_context.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace skycue::net {

class Fanout {
public:
    explicit Fanout(boost::asio::io_context& io);
    ~Fanout();
    Fanout(const Fanout&) = delete;
    Fanout& operator=(const Fanout&) = delete;

    void attach(const std::shared_ptr<Listener>& l); // publishes with sendAll; peer events re-emitted via onPeerUp/onPeerDown
    void attach(std::shared_ptr<Dialer> d);   // publishes with send
    void attach(std::shared_ptr<UdpSender> u); // publishes with send
    // Attaching the same destination twice is a no-op; detaching one that is
    // not attached is a no-op.
    void detach(const std::shared_ptr<Listener>& l);
    void detach(const std::shared_ptr<Dialer>& d);
    void detach(const std::shared_ptr<UdpSender>& u);

    void publish(const Buf& b);

    // Greeting hook: called when a TCP peer of an attached listener connects,
    // from the listener's accept handler. sendToThatPeer queues a buffer for
    // that peer only; whatever is published after the hook returns is queued
    // behind it. The function is valid only for the duration of the call.
    void onPeerUp(std::function<void(const std::function<void(Buf)>& sendToThatPeer)> h);
    // Called (asynchronously) when a TCP peer of an attached listener is gone.
    void onPeerDown(std::function<void(uint64_t stream)> h);

    // Sums over destinations: messages_out, bytes_out, dropped, peers (messages_out
    // counts one per TCP peer, since each peer is a sink). last_error is the first non-empty one.
    Counters counters() const;
    std::size_t destinations() const; // attached listeners + dialers + UDP senders

private:
    struct State;
    std::shared_ptr<State> st_;
};

} // namespace skycue::net
