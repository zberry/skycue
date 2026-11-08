#pragma once

// Helpers shared by the net library tests: free local ports, an io_context
// on its own thread, a polling wait and an IPv6 loopback probe.

#include "slow.hpp"

#include <boost/asio.hpp>

#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

namespace skycue::net::test {

// A port that was free a moment ago (bind 127.0.0.1:0, read it, close).
inline uint16_t freeTcpPort()
{
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor a(io, {boost::asio::ip::make_address("127.0.0.1"), 0});
    return a.local_endpoint().port();
}

inline uint16_t freeUdpPort()
{
    boost::asio::io_context io;
    boost::asio::ip::udp::socket s(io, {boost::asio::ip::make_address("127.0.0.1"), 0});
    return s.local_endpoint().port();
}

inline bool haveIpv6Loopback()
{
    try {
        boost::asio::io_context io;
        boost::asio::ip::tcp::acceptor a(io, {boost::asio::ip::make_address("::1"), 0});
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

// An io_context run on a thread of its own. call() runs a function on that
// thread and returns its result (rethrowing its exception); it throws
// std::runtime_error when the function has not run within `limit`.
struct IoThread {
    boost::asio::io_context io;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> guard{io.get_executor()};
    std::thread thread{[this] { io.run(); }};

    IoThread() = default;
    IoThread(const IoThread&) = delete;
    IoThread& operator=(const IoThread&) = delete;

    template <class F>
    auto call(F&& f, std::chrono::milliseconds limit = slow(std::chrono::seconds(5))) -> std::invoke_result_t<F>
    {
        using R = std::invoke_result_t<F>;
        auto task = std::make_shared<std::packaged_task<R()>>(std::forward<F>(f));
        auto fut = task->get_future();
        boost::asio::post(io, [task] { (*task)(); });
        if (fut.wait_for(limit) != std::future_status::ready)
            throw std::runtime_error("io thread call timed out");
        return fut.get();
    }

    // Stops the io_context and joins the thread; the destructor does it too.
    void join()
    {
        guard.reset();
        io.stop();
        if (thread.joinable())
            thread.join();
    }

    ~IoThread() { join(); }
};

// Polls p every 5 ms until it is true or the limit passes.
template <class Pred>
bool eventually(Pred p, std::chrono::milliseconds limit = slow(std::chrono::seconds(5)))
{
    const auto end = std::chrono::steady_clock::now() + limit;
    while (!p()) {
        if (std::chrono::steady_clock::now() >= end)
            return p();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

// For "nothing happens" assertions: polls p every 5 ms for the whole window
// and returns false at once when it turns false. A bounded poll instead of a
// fixed sleep: it fails as soon as the unwanted event shows, and the window
// (give it slow(...)) is the only wait that is unavoidable.
template <class Pred>
bool holds(Pred p, std::chrono::milliseconds window)
{
    const auto end = std::chrono::steady_clock::now() + window;
    do {
        if (!p())
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() < end);
    return true;
}

} // namespace skycue::net::test
