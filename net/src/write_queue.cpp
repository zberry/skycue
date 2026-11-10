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

    // Messages are taken from the front of the queue while their total stays
    // within this; a single larger message is written alone, whole.
    static constexpr std::size_t kCoalesceBytes = 64 * 1024;

    static void writeLoop(const std::shared_ptr<State>& st)
    {
        if (!st->started || st->in_flight != 0 || st->queue.empty())
            return;
        std::size_t count = 1;
        std::size_t bytes = st->queue.front()->size();
        while (count < st->queue.size() && bytes + st->queue[count]->size() <= kCoalesceBytes) {
            bytes += st->queue[count]->size();
            ++count;
        }
        st->in_flight = count;
        // The handler owns what it writes, so it outlives a clear().
        std::shared_ptr<const void> keep;
        asio::const_buffer buf;
        if (count == 1) {
            Buf one = st->queue.front();
            buf = asio::const_buffer(one->data(), one->size());
            keep = std::move(one);
        } else {
            auto block = std::make_shared<std::vector<unsigned char>>();
            block->reserve(bytes);
            for (std::size_t i = 0; i < count; ++i) {
                const auto& b = *st->queue[i];
                block->insert(block->end(), b.data(), b.data() + b.size());
            }
            buf = asio::const_buffer(block->data(), block->size());
            keep = std::move(block);
        }
        const uint64_t gen = st->gen;
        auto sock = st->sock;
        asio::async_write(*sock, buf, [st, keep, sock, gen](const boost::system::error_code& ec, std::size_t n) {
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
