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
// Peer events: the Fanout takes over the peer handler of an attached Listener
// (Listener::onPeer) and the connection handler of an attached Dialer
// (Dialer::onConnection), so a host registers peer events through
// Fanout::onPeerUp / onPeerDown only; a handler set on the destination
// afterwards would replace Fanout's. Every TCP peer gets a Fanout-wide unique
// id (listener streams alone would collide between listeners; a dialer's
// connection gets a fresh id each time it connects). detach() restores empty
// handlers. The handlers refer to Fanout's state only weakly, so they are
// safe if the Fanout is destroyed first, but a host should still detach
// destinations before destroying the Fanout.

#include "skycue/net/buffer.hpp"
#include "skycue/net/counters.hpp"
#include "skycue/net/dialer.hpp"
#include "skycue/net/listener.hpp"
#include "skycue/net/udp_sender.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace skycue::net {

class Fanout {
public:
    Fanout();
    // Older form: the io_context was never used. Kept for existing callers.
    explicit Fanout(boost::asio::io_context& io);
    ~Fanout();
    Fanout(const Fanout&) = delete;
    Fanout& operator=(const Fanout&) = delete;

    void attach(const std::shared_ptr<Listener>& l); // publishes with sendAll; peer events re-emitted via onPeerUp/onPeerDown
    void attach(const std::shared_ptr<Dialer>& d);   // publishes with send
    void attach(std::shared_ptr<UdpSender> u); // publishes with send
    // Attaching the same destination twice is a no-op; detaching one that is
    // not attached is a no-op.
    void detach(const std::shared_ptr<Listener>& l);
    void detach(const std::shared_ptr<Dialer>& d);
    void detach(const std::shared_ptr<UdpSender>& u);

    void publish(const Buf& b);

    // Greeting hook: called when a TCP peer of an attached listener connects
    // (from the listener's accept handler), or when an attached dialer
    // connects (from its connect handler), with the Fanout's id for the peer,
    // its remote address, and sendToThatPeer, which queues a buffer for that
    // peer only; whatever is published after the hook returns is queued
    // behind it. The function holds a shared_ptr to the Listener or Dialer:
    // do not retain it; use it only during the call. A destination that is
    // already connected when attached gets no peer-up.
    using PeerUpHook = std::function<void(std::uint64_t peer, const boost::asio::ip::tcp::endpoint& who,
                                          const std::function<void(const Buf&)>& sendToThatPeer)>;
    void onPeerUp(PeerUpHook h);
    // Called (asynchronously) when a TCP peer of an attached destination is gone.
    void onPeerDown(std::function<void(std::uint64_t peer)> h);

    struct PeerInfo {
        PeerStats stats;       // send side of the connection
        bool dialled = false;  // the peer is an attached dialer's connection
        const void* source = nullptr; // the Listener or Dialer it belongs to; compare only, never dereference
    };
    // Empty when the peer is gone (closed, cut off, or its destination detached).
    std::optional<PeerInfo> peerInfo(std::uint64_t peer) const;

    // Sums over destinations: messages_out, bytes_out, dropped, peers (messages_out
    // counts one per TCP peer, since each peer is a sink). last_error is the first non-empty one.
    Counters counters() const;
    std::size_t destinations() const; // attached listeners + dialers + UDP senders

private:
    struct State;
    std::shared_ptr<State> st_;
};

} // namespace skycue::net
