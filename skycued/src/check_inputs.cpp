#include "check_inputs.hpp"

#include "skycue/net/dialer.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <memory>

namespace skycued {

namespace asio = boost::asio;
namespace net = skycue::net;

namespace {

// The Dialer's failure text in the wording the probe always printed: the
// system's message for a refused connect ("Connection refused", ...),
// "cannot resolve host: ..." for a failed lookup.
std::string reasonFromFailure(const std::string& why)
{
    if (why.rfind("resolve ", 0) == 0) {
        const auto colon = why.find(": ");
        return "cannot resolve host: " + (colon == std::string::npos ? why : why.substr(colon + 2));
    }
    // "connect host:port: [address: ]reason"
    const auto last = why.rfind(": ");
    return last == std::string::npos ? why : why.substr(last + 2);
}

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

    // One shot per input: the first result (connected, or the first failed
    // attempt cycle) settles it and stops its Dialer before it can retry.
    std::vector<char> done(inputs.size(), 0);
    size_t remaining = inputs.size();
    std::vector<std::shared_ptr<net::Dialer>> dialers;
    auto settle = [&](size_t i, bool ok, const std::string& error) {
        if (done[i])
            return;
        done[i] = 1;
        results[i].connected = ok;
        results[i].error = ok ? std::string() : error;
        dialers[i]->stop();
        if (--remaining == 0)
            io.stop();
    };

    for (size_t i = 0; i < inputs.size(); ++i) {
        net::TcpOptions o;
        o.connect_timeout = timeout;
        dialers.push_back(net::Dialer::create(io, inputs[i].host, inputs[i].port, o));
        dialers[i]->onConnection([&settle, i](bool up) {
            if (up)
                settle(i, true, std::string());
        });
        dialers[i]->onFailure([&settle, i](const std::string& why) { settle(i, false, reasonFromFailure(why)); });
        dialers[i]->start();
    }
    // Backstop for what the Dialer cannot bound: a hung name lookup, or many
    // addresses each taking the full timeout.
    asio::steady_timer deadline(io);
    deadline.expires_after(timeout + std::chrono::milliseconds(200));
    deadline.async_wait([&](const boost::system::error_code& ec) {
        if (!ec)
            io.stop();
    });
    if (remaining > 0)
        io.run();
    bool abandoned = false;
    for (size_t i = 0; i < inputs.size(); ++i) {
        if (!done[i]) {
            settle(i, false, "timed out");
            abandoned = true;
        }
    }
    if (!abandoned) {
        deadline.cancel();
        dialers.clear();
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
