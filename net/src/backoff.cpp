#include "skycue/net/backoff.hpp"

#include <algorithm>
#include <utility>

namespace skycue::net {

Backoff::Backoff(std::chrono::milliseconds min, std::chrono::milliseconds max)
    : min_(std::max(min, std::chrono::milliseconds(1))), max_(std::max(min_, max)), cur_(min_) {}

std::chrono::milliseconds Backoff::next()
{
    const auto d = cur_;
    cur_ = std::min(cur_ * 2, max_);
    return d;
}

void Backoff::reset() { cur_ = min_; }

RetryTimer::RetryTimer(boost::asio::io_context& io) : timer_(io) {}

void RetryTimer::after(std::chrono::milliseconds d, std::function<void()> fn)
{
    const uint64_t gen = ++generation_;
    timer_.expires_after(d); // cancels a pending wait; its handler sees a stale generation
    timer_.async_wait([this, gen, fn = std::move(fn)](const boost::system::error_code& ec) {
        if (ec || gen != generation_)
            return;
        fn();
    });
}

void RetryTimer::cancel()
{
    ++generation_;
    timer_.cancel();
}

} // namespace skycue::net
