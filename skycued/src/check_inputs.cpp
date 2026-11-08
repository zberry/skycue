#include "check_inputs.hpp"

#include "skycue/net/dialer.hpp"
#include "skycue/net/log.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <memory>
#include <mutex>

namespace skycued {

namespace asio = boost::asio;
namespace net = skycue::net;

namespace {

// The Dialer reports success through onConnection() and every failure only
// through the net log, so the probe reads the reason from there. The wording
// is the one the probe always printed: the system's message for a refused
// connect ("Connection refused", "timed out", ...), "cannot resolve host: ..."
// for a failed lookup.
std::string reasonFromLog(const std::string& msg)
{
    std::string why = msg.substr(0, msg.find("; retrying in"));
    const auto lookup = why.find("resolve ");
    if (lookup != std::string::npos) {
        const auto colon = why.find(": ", lookup);
        return "cannot resolve host: " + (colon == std::string::npos ? why : why.substr(colon + 2));
    }
    // "connect host:port: [address: ]reason"
    const auto at = why.find("connect ");
    if (at == std::string::npos)
        return std::string();
    const auto first = why.find(": ", at);
    if (first == std::string::npos)
        return std::string();
    why = why.substr(first + 2);
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

    // One shot per input: the first result (connected, or the first failure the
    // Dialer logs) settles it, and the Dialer is stopped before it can retry.
    std::mutex mu;  // the net log sink may be called from any thread
    std::vector<bool> done(inputs.size(), false);
    size_t remaining = inputs.size();
    std::vector<std::shared_ptr<net::Dialer>> dialers;
    auto settle = [&](size_t i, bool ok, const std::string& error) {
        std::lock_guard<std::mutex> lk(mu);
        if (done[i])
            return;
        done[i] = true;
        results[i].connected = ok;
        results[i].error = ok ? std::string() : error;
        if (--remaining == 0)
            io.stop();
    };
    net::setLogSink([&](net::LogLevel, const std::string& name, const std::string& msg) {
        if (name.rfind("probe", 0) != 0)
            return;
        const size_t i = std::stoul(name.substr(5));
        const std::string why = reasonFromLog(msg);
        if (!why.empty() && i < inputs.size())
            settle(i, false, why);
    });

    for (size_t i = 0; i < inputs.size(); ++i) {
        net::TcpOptions o;
        o.name = "probe" + std::to_string(i);
        o.connect_timeout = timeout;
        dialers.push_back(net::Dialer::create(io, inputs[i].host, inputs[i].port, o));
        dialers.back()->onConnection([&settle, i](bool up) {
            if (up)
                settle(i, true, std::string());
        });
        dialers.back()->start();
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
    for (auto& d : dialers)
        d->stop();
    net::setLogSink(nullptr);
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
