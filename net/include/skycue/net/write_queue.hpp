#pragma once

// WriteQueue: the send side of one TCP connection. It holds the Bufs waiting
// to be written, writes them in contiguous blocks (what is queued when a
// write starts, up to 64 KiB, is copied into one buffer and goes out in one
// async_write; a larger single message is written alone) and enforces a byte
// cap. Nothing waits for more data: a block holds only what is already queued. Used by
// Dialer and Listener, one per connection. All members run on the
// io_context's thread.
//
// Lifetime: the socket is held by shared_ptr and by every pending handler. A
// write completion that arrives after clear() or after the WriteQueue is
// destroyed is ignored, so on_error is never called late.

#include "skycue/net/buffer.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/system/error_code.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace skycue::net {

class WriteQueue {
public:
    WriteQueue(std::shared_ptr<boost::asio::ip::tcp::socket> sock, std::size_t cap);
    ~WriteQueue();
    WriteQueue(const WriteQueue&) = delete;
    WriteQueue& operator=(const WriteQueue&) = delete;

    // Queues b and begins a write when none is running and start() was
    // called. Returns false, with nothing queued, when queuedBytes() plus
    // b's size would exceed the cap.
    bool enqueue(const Buf& b);
    // Begins writing what is queued and whatever is enqueued later. A write
    // error is reported once through on_error (from the io thread, never
    // from inside enqueue()); the queue then stays stalled until clear().
    // on_error may destroy the WriteQueue.
    void start(std::function<void(const boost::system::error_code&)> on_error);
    // Drops everything queued and voids the write in flight, if any. It
    // cancels nothing: the caller closes the socket.
    void clear();

    std::size_t queuedBytes() const; // queued or being written, not yet completed
    uint64_t bytesWritten() const;   // completed writes only
    uint64_t messagesWritten() const;

private:
    struct State;
    std::shared_ptr<State> st_;
};

} // namespace skycue::net
