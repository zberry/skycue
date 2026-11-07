#include "skycue/net/write_queue.hpp"

#include <boost/asio/write.hpp>

#include <deque>
#include <vector>

namespace skycue::net {

namespace asio = boost::asio;

struct WriteQueue::State {
    std::shared_ptr<asio::ip::tcp::socket> sock;
    std::size_t cap = 0;
    std::deque<Buf> queue; // the first `in_flight` entries are being written
    std::size_t in_flight = 0;
    std::size_t queued_bytes = 0;
    uint64_t bytes_written = 0;
    uint64_t messages_written = 0;
    uint64_t gen = 0; // changes on clear() and destruction; voids the handler of a write in flight
    bool started = false;
    std::function<void(const boost::system::error_code&)> on_error;

    static void writeLoop(const std::shared_ptr<State>& st)
    {
        if (!st->started || st->in_flight != 0 || st->queue.empty())
            return;
        st->in_flight = st->queue.size();
        // The handler owns the buffers it writes, so they outlive a clear().
        auto keep = std::make_shared<std::vector<Buf>>(st->queue.begin(), st->queue.begin() + static_cast<std::ptrdiff_t>(st->in_flight));
        std::vector<asio::const_buffer> bufs;
        bufs.reserve(keep->size());
        for (const auto& b : *keep)
            bufs.emplace_back(b->data(), b->size());
        const uint64_t gen = st->gen;
        auto sock = st->sock;
        asio::async_write(*sock, bufs, [st, keep, sock, gen](const boost::system::error_code& ec, std::size_t n) {
            if (gen != st->gen)
                return;
            if (ec) {
                const auto cb = st->on_error; // a copy: the callback may destroy the queue
                if (cb)
                    cb(ec);
                return;
            }
            st->bytes_written += n;
            for (std::size_t i = 0; i < st->in_flight; ++i) {
                st->queued_bytes -= st->queue.front()->size();
                st->queue.pop_front();
                ++st->messages_written;
            }
            st->in_flight = 0;
            writeLoop(st);
        });
    }
};

WriteQueue::WriteQueue(std::shared_ptr<asio::ip::tcp::socket> sock, std::size_t cap) : st_(std::make_shared<State>())
{
    st_->sock = std::move(sock);
    st_->cap = cap;
}

WriteQueue::~WriteQueue()
{
    clear();
    st_->on_error = nullptr;
}

bool WriteQueue::enqueue(const Buf& b)
{
    auto& s = *st_;
    if (!b || s.queued_bytes + b->size() > s.cap)
        return false;
    s.queued_bytes += b->size();
    s.queue.push_back(b);
    State::writeLoop(st_);
    return true;
}

void WriteQueue::start(std::function<void(const boost::system::error_code&)> on_error)
{
    st_->on_error = std::move(on_error);
    st_->started = true;
    State::writeLoop(st_);
}

void WriteQueue::clear()
{
    auto& s = *st_;
    ++s.gen;
    s.queue.clear();
    s.in_flight = 0;
    s.queued_bytes = 0;
}

std::size_t WriteQueue::queuedBytes() const { return st_->queued_bytes; }
uint64_t WriteQueue::bytesWritten() const { return st_->bytes_written; }
uint64_t WriteQueue::messagesWritten() const { return st_->messages_written; }

} // namespace skycue::net
