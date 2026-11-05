#include "check_inputs.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>

#include <memory>

namespace skycued {

namespace asio = boost::asio;
using asio::ip::tcp;

namespace {

struct Probe : std::enable_shared_from_this<Probe> {
    Probe(asio::io_context& io, InputCheckResult& r) : result(r), resolver(io), socket(io), timer(io) {}

    InputCheckResult& result;
    tcp::resolver resolver;
    tcp::socket socket;
    asio::steady_timer timer;
    bool done = false;
    bool resolving = true;  // an async_resolve is still running

    void finish(bool ok, const std::string& error)
    {
        if (done)
            return;
        done = true;
        result.connected = ok;
        result.error = ok ? std::string() : error;
        timer.cancel();
        boost::system::error_code ignored;
        socket.close(ignored);
        resolver.cancel();
    }

    void start(std::chrono::milliseconds timeout)
    {
        auto self = shared_from_this();
        timer.expires_after(timeout);
        timer.async_wait([self](const boost::system::error_code& ec) {
            if (!ec)
                self->finish(false, "timed out");
        });
        resolver.async_resolve(result.input.host, std::to_string(result.input.port),
                               [self](const boost::system::error_code& ec, tcp::resolver::results_type eps) {
                                   self->resolving = false;
                                   if (self->done)
                                       return;
                                   if (ec) {
                                       self->finish(false, "cannot resolve host: " + ec.message());
                                       return;
                                   }
                                   asio::async_connect(self->socket, eps,
                                                       [self](const boost::system::error_code& cec, const tcp::endpoint&) {
                                                           if (self->done)
                                                               return;
                                                           self->finish(!cec, cec.message());
                                                       });
                               });
    }
};

}  // namespace

std::vector<InputCheckResult> checkInputs(const std::vector<InputCheck>& inputs, std::chrono::milliseconds timeout)
{
    std::vector<InputCheckResult> results(inputs.size());
    for (size_t i = 0; i < inputs.size(); ++i)
        results[i].input = inputs[i];
    // Heap-allocated: a name lookup cannot be cancelled, and destroying the
    // io_context would wait for it (as long as libc takes). When one is still
    // running at the deadline the io_context is left alone, so the caller is
    // not held; the lookup ends with the process.
    auto* io_ptr = new asio::io_context;
    asio::io_context& io = *io_ptr;
    std::vector<std::shared_ptr<Probe>> probes;
    for (auto& r : results) {
        probes.push_back(std::make_shared<Probe>(io, r));
        probes.back()->start(timeout);
    }
    io.run_for(timeout + std::chrono::milliseconds(500));
    for (auto& p : probes)
        p->finish(false, "timed out");  // no-op for those already done
    bool lookup_pending = false;
    for (auto& p : probes)
        lookup_pending = lookup_pending || p->resolving;
    if (!lookup_pending) {
        // The probes' sockets and timers first: they must not outlive the
        // io_context they were made on.
        probes.clear();
        delete io_ptr;
    }
    return results;
}

std::string inputCheckLine(const InputCheckResult& r)
{
    return r.input.id + " " + r.input.host + ":" + std::to_string(r.input.port) + ": " +
           (r.connected ? std::string("connected") : r.error);
}

}  // namespace skycued
