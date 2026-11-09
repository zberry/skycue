#include "web_server.hpp"

#include "engine.hpp"
#include "file_util.hpp"
#include "log.hpp"
#include "runtime_config.hpp"
#include "web_api.hpp"
#include "web_inputs.hpp"
#include "web_outputs.hpp"
#include "web_stats.hpp"

// Boost.Beast as of Boost 1.66 (the oldest supported): http::async_read with
// a request_parser, http::async_write, websocket::stream<tcp::socket> with
// async_accept / async_read / async_write / async_ping / control_callback.
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace skycued {

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace websocket = boost::beast::websocket;
using tcp = asio::ip::tcp;
using error_code = boost::system::error_code;
using steady = std::chrono::steady_clock;

constexpr std::uint64_t kBodyLimit = 64 * 1024;           // request bodies
constexpr std::size_t kWsMessageLimit = 64 * 1024;        // what a WebSocket client may send
constexpr std::size_t kStaticFileLimit = 8u << 20;        // larger files are not served (500)
constexpr std::size_t kLingerDrainLimit = 1u << 20;       // bytes read and discarded before closing
constexpr auto kLingerTime = std::chrono::seconds(2);

std::string endpointText(const tcp::socket& s)
{
    error_code ec;
    const auto ep = s.remote_endpoint(ec);
    if (ec)
        return "?";
    std::ostringstream os;
    os << ep;
    return os.str();
}

template <class S> std::string toString(const S& view) { return std::string(view.data(), view.size()); }

}  // namespace

struct HttpSession;
struct WsSession;

struct WebServer::Impl : std::enable_shared_from_this<WebServer::Impl> {
    struct Item {
        EventKind kind;
        std::string json;  // empty for filters_changed / lists_changed: built at flush
    };

    asio::io_context& io;
    Engine& engine;
    RuntimeConfig& runtime;
    WebConfig cfg;
    tcp::acceptor acceptor;
    asio::steady_timer accept_retry;
    uint16_t port = 0;
    bool started = false, stopped = false;
    std::map<HttpSession*, std::weak_ptr<HttpSession>> http_sessions;
    std::map<WsSession*, std::weak_ptr<WsSession>> ws_sessions;  // handshake pending or done
    size_t ws_open = 0;                                          // handshake done, not closed
    std::vector<Item> pending;                                   // events of this io turn
    bool flush_posted = false;
    uint64_t batch = 0;  // the batch `pending` is collecting
    webapi::StatsApiState stats_api;  // the reset rate limit

    Impl(asio::io_context& io_, Engine& e, RuntimeConfig& rc, WebConfig c)
        : io(io_), engine(e), runtime(rc), cfg(std::move(c)), acceptor(io_), accept_retry(io_)
    {
    }

    void start();
    void stop();
    void accept();
    void onEvent(const EngineEvent& ev);
    void onRuntimeChange();
    void enqueue(Item&& item);
    void flush();
    void wsOpened(WsSession* s);
    void wsClosed(WsSession* s, bool was_open);
};

// ---- one HTTP connection ----

struct HttpSession : std::enable_shared_from_this<HttpSession> {
    std::shared_ptr<WebServer::Impl> srv;
    tcp::socket sock;
    beast::flat_buffer buf;
    std::optional<http::request_parser<http::string_body>> parser;
    asio::steady_timer timer;
    std::array<char, 4096> scratch{};
    size_t drained = 0;
    bool closed = false;
    std::string peer;

    HttpSession(std::shared_ptr<WebServer::Impl> s, tcp::socket&& socket)
        : srv(std::move(s)), sock(std::move(socket)), timer(srv->io)
    {
        peer = endpointText(sock);
    }

    void arm(std::chrono::steady_clock::duration d)
    {
        timer.expires_after(d);
        std::weak_ptr<HttpSession> weak = shared_from_this();
        timer.async_wait([weak](const error_code& ec) {
            if (ec)
                return;
            if (auto self = weak.lock())
                self->close();
        });
    }

    void readNext()
    {
        parser.emplace();
        parser->body_limit(kBodyLimit);
        arm(srv->cfg.idle_timeout);
        auto self = shared_from_this();
        http::async_read(sock, buf, *parser, [self](const error_code& ec, auto&&...) { self->onRead(ec); });
    }

    void onRead(const error_code& ec)
    {
        if (closed || srv->stopped)
            return;
        if (ec == http::error::body_limit) {
            respond(11, false, 413, webapi::errorJson("request body over 64 KiB"), "application/json", "no-store");
            return;
        }
        if (ec) {
            // A malformed request gets a 400; a connection that went away is closed.
            if (ec.category() == http::make_error_code(http::error::bad_target).category() &&
                ec != http::error::end_of_stream && ec != http::error::partial_message)
                respond(11, false, 400, "bad request\n", "text/plain; charset=utf-8", "no-store");
            else
                close();
            return;
        }
        handle(parser->release());
    }

    void handle(http::request<http::string_body>&& req)
    {
        const std::string method = toString(req.method_string());
        const std::string target = toString(req.target());
        const std::string path = target.substr(0, target.find_first_of("?#"));
        const bool keep = req.keep_alive();
        const unsigned version = req.version();
        LOG_DEBUG("web: " << peer << " " << method << " " << target);

        if (path == "/ws") {
            if (websocket::is_upgrade(req)) {
                upgrade(std::move(req));
                return;
            }
            respond(version, keep, 400, "WebSocket upgrade required\n", "text/plain; charset=utf-8", "no-store");
            return;
        }
        if (webapi::isApiPath(path)) {
            std::vector<std::pair<std::string, std::string>> fields;
            for (const auto& field : req)
                fields.emplace_back(toString(field.name_string()), toString(field.value()));
            const webapi::ApiRequest api = webapi::makeApiRequest(method, target, fields, std::move(req.body()),
                                                                  peer);
            webapi::ApiContext ctx{srv->engine,     srv->runtime,    srv->cfg.www_dir,
                                   srv->cfg.daemon, &srv->stats_api, srv->cfg.map_dir,
                                   &srv->cfg.on_restart};
            const auto r = webapi::handleApi(api, ctx);
            if (r.status >= 500)
                LOG_ERROR("web: " << method << " " << path << ": " << r.body);
            else if (method != "GET")
                LOG_INFO("web: " << peer << " " << method << " " << path << " -> " << r.status);
            respond(version, keep, r.status, r.body, r.content_type, "no-store", r.allow);
            return;
        }
        const bool head = req.method() == http::verb::head;
        if (req.method() != http::verb::get && !head) {
            respond(version, keep, 405, "method not allowed\n", "text/plain; charset=utf-8", "no-store",
                    "GET, HEAD");
            return;
        }
        const auto rel = webapi::staticPath(target);
        if (!rel) {
            respond(version, keep, 400, "bad path\n", "text/plain; charset=utf-8", "no-store");
            return;
        }
        // staticPath has already refused any "." or ".." segment, so the
        // file stays under whichever directory it is read from.
        const bool map_file = !srv->cfg.map_dir.empty() && (rel->compare(0, 7, "/tiles/") == 0 ||
                                                            rel->compare(0, 10, "/overlays/") == 0);
        std::string file;
        switch (readRegularFile((map_file ? srv->cfg.map_dir : srv->cfg.www_dir) + *rel, kStaticFileLimit, file)) {
        case ReadStatus::ok: break;
        case ReadStatus::missing:
            respond(version, keep, 404, "not found\n", "text/plain; charset=utf-8", "no-cache");
            return;
        case ReadStatus::too_large:
            LOG_WARN("web: " << *rel << " is over " << (kStaticFileLimit >> 20) << " MiB; not served");
            respond(version, keep, 500, "file too large to serve\n", "text/plain; charset=utf-8", "no-store");
            return;
        case ReadStatus::error:
            LOG_WARN("web: " << *rel << ": cannot read: " << std::strerror(errno));
            respond(version, keep, 500, "cannot read the file\n", "text/plain; charset=utf-8", "no-store");
            return;
        }
        respond(version, keep, 200, std::move(file), webapi::contentType(*rel), webapi::cacheControl(*rel), "",
                head);
    }

    void respond(unsigned version, bool keep, unsigned status, std::string body, const std::string& content_type,
                 const char* cache_control, const std::string& allow = "", bool head = false)
    {
        auto res = std::make_shared<http::response<http::string_body>>();
        res->version(version);
        res->result(status);
        res->set(http::field::server, "skycued");
        if (!content_type.empty())  // a 204 has none
            res->set(http::field::content_type, content_type);
        res->set(http::field::cache_control, cache_control);
        if (!allow.empty())
            res->set(http::field::allow, allow);
        res->keep_alive(keep);
        const std::size_t size = body.size();
        if (!head)
            res->body() = std::move(body);
        res->prepare_payload();
        if (status == 204)
            res->erase(http::field::content_length);  // RFC 7230 3.3.2: none on a 204
        if (head)
            res->content_length(size);  // the GET's length, no body
        const bool close_after = res->need_eof();
        arm(srv->cfg.idle_timeout);
        auto self = shared_from_this();
        http::async_write(sock, *res, [self, res, close_after](const error_code& ec, auto&&...) {
            if (self->closed || self->srv->stopped)
                return;
            if (ec) {
                self->close();
                return;
            }
            if (close_after)
                self->linger();
            else
                self->readNext();
        });
    }

    // Half-closes, then reads and discards what the client still sends
    // (closing with unread data would reset the connection before the client
    // has read the response) until it closes, for at most kLingerTime.
    void linger()
    {
        error_code ignored;
        sock.shutdown(tcp::socket::shutdown_send, ignored);
        arm(kLingerTime);
        drain();
    }

    void drain()
    {
        auto self = shared_from_this();
        sock.async_read_some(asio::buffer(scratch), [self](const error_code& ec, std::size_t n) {
            if (self->closed)
                return;
            self->drained += n;
            if (ec || self->drained > kLingerDrainLimit)
                self->close();
            else
                self->drain();
        });
    }

    void upgrade(http::request<http::string_body>&& req);

    // Detaches from the server; the socket is closed unless it was handed on.
    void close()
    {
        if (closed)
            return;
        closed = true;
        error_code ignored;
        timer.cancel();
        if (sock.is_open()) {
            sock.shutdown(tcp::socket::shutdown_both, ignored);
            sock.close(ignored);
        }
        srv->http_sessions.erase(this);
    }
};

// ---- one WebSocket client ----

struct WsSession : std::enable_shared_from_this<WsSession> {
    std::shared_ptr<WebServer::Impl> srv;
    websocket::stream<tcp::socket> ws;
    http::request<http::string_body> req;  // the upgrade request, kept until the accept completes
    beast::flat_buffer rbuf;
    std::deque<std::shared_ptr<const std::string>> queue;  // front: being written while `writing`
    size_t queued_bytes = 0;
    bool writing = false, ping_due = false, open = false, closed = false;
    asio::steady_timer ping_timer;
    steady::time_point last_heard{};
    // Pings and pongs count as hearing from the client. A member, passed as
    // an lvalue: Boost 1.66's control_callback(Callback&) keeps a reference
    // to it (later versions take a std::function by value and copy it).
    std::function<void(websocket::frame_type, beast::string_view)> on_control;
    // Of batch skip_batch, the first skip_items events precede the snapshot.
    uint64_t skip_batch = std::numeric_limits<uint64_t>::max();
    size_t skip_items = 0;
    std::string peer;

    WsSession(std::shared_ptr<WebServer::Impl> s, tcp::socket&& socket, http::request<http::string_body>&& r)
        : srv(std::move(s)), ws(std::move(socket)), req(std::move(r)), ping_timer(srv->io)
    {
        peer = endpointText(ws.next_layer());
    }

    void start()
    {
        srv->ws_sessions[this] = shared_from_this();
        ws.read_message_max(kWsMessageLimit);
        // Invoked during async_read (always outstanding), which holds `self`.
        on_control = [this](websocket::frame_type, beast::string_view) { last_heard = steady::now(); };
        ws.control_callback(on_control);
        auto self = shared_from_this();
        ws.async_accept(req, [self](const error_code& ec) { self->onAccept(ec); });
    }

    void onAccept(const error_code& ec)
    {
        if (closed || srv->stopped)
            return;
        if (ec) {
            LOG_DEBUG("web: websocket handshake with " << peer << " failed: " << ec.message());
            close();
            return;
        }
        req = {};
        open = true;
        last_heard = steady::now();
        ws.text(true);
        srv->wsOpened(this);
        send(std::make_shared<const std::string>(webapi::stateJson(srv->engine, srv->runtime, "snapshot")));
        if (closed)
            return;
        read();
        armPing();
    }

    void read()
    {
        auto self = shared_from_this();
        ws.async_read(rbuf, [self](const error_code& ec, auto&&...) {
            if (self->closed)
                return;
            if (ec) {
                LOG_DEBUG("web: websocket " << self->peer << ": " << ec.message());
                self->close();
                return;
            }
            self->last_heard = steady::now();
            LOG_DEBUG("web: websocket " << self->peer << " sent " << self->rbuf.size() << " bytes; ignored");
            self->rbuf.consume(self->rbuf.size());
            self->read();
        });
    }

    void armPing()
    {
        ping_timer.expires_after(srv->cfg.ping_interval);
        auto self = shared_from_this();
        ping_timer.async_wait([self](const error_code& ec) {
            if (ec || self->closed)
                return;
            if (steady::now() - self->last_heard > 2 * self->srv->cfg.ping_interval) {
                LOG_WARN("web: websocket client " << self->peer << " stopped answering pings; disconnecting");
                self->close();
                return;
            }
            self->ping_due = true;
            self->write();
            self->armPing();
        });
    }

    void send(std::shared_ptr<const std::string> msg)
    {
        if (closed)
            return;
        if (queued_bytes + msg->size() > srv->cfg.max_ws_queue_bytes) {
            LOG_WARN("web: websocket client " << peer << " is too slow (" << queued_bytes
                                              << " bytes queued); disconnecting");
            close();
            return;
        }
        queued_bytes += msg->size();
        queue.push_back(std::move(msg));
        write();
    }

    // Starts the next write unless one is in flight: a due ping first, then
    // the oldest queued message.
    void write()
    {
        if (writing || closed)
            return;
        auto self = shared_from_this();
        if (ping_due) {
            ping_due = false;
            writing = true;
            ws.async_ping(websocket::ping_data{}, [self](const error_code& ec) {
                self->writing = false;
                if (self->closed)
                    return;
                if (ec) {
                    self->close();
                    return;
                }
                self->write();
            });
            return;
        }
        if (queue.empty())
            return;
        writing = true;
        ws.async_write(asio::buffer(*queue.front()), [self](const error_code& ec, auto&&...) {
            self->writing = false;
            if (self->closed)
                return;
            if (ec) {
                LOG_DEBUG("web: websocket " << self->peer << ": " << ec.message());
                self->close();
                return;
            }
            self->queued_bytes -= self->queue.front()->size();
            self->queue.pop_front();
            self->write();
        });
    }

    // Closes the socket at once (any operation in flight completes with an
    // error; the queue is freed with the session).
    void close()
    {
        if (closed)
            return;
        closed = true;
        error_code ignored;
        ping_timer.cancel();
        auto& s = ws.next_layer();
        s.shutdown(tcp::socket::shutdown_both, ignored);
        s.close(ignored);
        srv->wsClosed(this, open);
        open = false;
    }
};

void HttpSession::upgrade(http::request<http::string_body>&& req)
{
    // The socket moves to the WebSocket session; this one is done.
    timer.cancel();
    auto ws = std::make_shared<WsSession>(srv, std::move(sock), std::move(req));
    closed = true;
    srv->http_sessions.erase(this);
    ws->start();
}

// ---- the server ----

void WebServer::Impl::start()
{
    if (started)
        return;
    started = true;
    error_code ec;
    const auto addr = asio::ip::make_address(cfg.bind, ec);
    if (ec)
        throw std::runtime_error("web: cannot listen on " + cfg.bind + ":" + std::to_string(cfg.port) +
                                 ": not a numeric address");
    const tcp::endpoint ep(addr, cfg.port);
    acceptor.open(ep.protocol(), ec);
    if (!ec)
        acceptor.set_option(tcp::acceptor::reuse_address(true), ec);
    if (!ec)
        acceptor.bind(ep, ec);
    if (!ec)
        acceptor.listen(asio::socket_base::max_listen_connections, ec);
    if (ec) {
        error_code ignored;
        acceptor.close(ignored);
        throw std::runtime_error("web: cannot listen on " + cfg.bind + ":" + std::to_string(cfg.port) + ": " +
                                 ec.message());
    }
    port = acceptor.local_endpoint(ec).port();
    LOG_INFO("web: listening on http://" << (addr.is_v6() ? "[" + cfg.bind + "]" : cfg.bind) << ":" << port
                                         << "/ (www " << cfg.www_dir
                                         << (cfg.map_dir.empty() ? std::string() : ", maps " + cfg.map_dir) << ")");
    if (!isDirectory(cfg.www_dir))
        LOG_WARN("web: www directory " << cfg.www_dir << " does not exist; the UI pages will be 404 (the API works)");
    if (!cfg.map_dir.empty() && !isDirectory(cfg.map_dir))
        LOG_WARN("web: map directory " << cfg.map_dir
                                       << " (paths.map_dir) does not exist; the map has no tiles or overlays (the API works)");
    Impl* self = this;  // stop() clears the sink before this goes away
    engine.onEvent([self](const EngineEvent& ev) { self->onEvent(ev); });
    runtime.onChange([self] { self->onRuntimeChange(); });
    accept();
}

void WebServer::Impl::accept()
{
    auto socket = std::make_shared<tcp::socket>(io);
    auto self = shared_from_this();
    acceptor.async_accept(*socket, [self, socket](const error_code& ec) {
        if (self->stopped)
            return;
        if (ec) {
            LOG_WARN("web: accept failed: " << ec.message());
            self->accept_retry.expires_after(std::chrono::milliseconds(100));
            self->accept_retry.async_wait([self](const error_code& e) {
                if (!e && !self->stopped)
                    self->accept();
            });
            return;
        }
        auto s = std::make_shared<HttpSession>(self, std::move(*socket));
        self->http_sessions[s.get()] = s;
        s->readNext();
        self->accept();
    });
}

void WebServer::Impl::stop()
{
    if (stopped)
        return;
    stopped = true;
    error_code ignored;
    acceptor.close(ignored);
    accept_retry.cancel();
    if (started) {
        engine.onEvent(nullptr);
        runtime.onChange(nullptr);
    }
    pending.clear();
    const auto http_copy = http_sessions;
    for (const auto& kv : http_copy)
        if (auto s = kv.second.lock())
            s->close();
    const auto ws_copy = ws_sessions;
    for (const auto& kv : ws_copy)
        if (auto s = kv.second.lock())
            s->close();
    http_sessions.clear();
    ws_sessions.clear();
    ws_open = 0;
    if (started)
        engine.setWebClientCount(0);
}

void WebServer::Impl::onEvent(const EngineEvent& ev)
{
    if (stopped || ws_open == 0)
        return;
    Item item{ev.kind, webapi::eventJson(ev)};
    if (item.json.empty() && ev.kind != EventKind::filters_changed && ev.kind != EventKind::lists_changed &&
        ev.kind != EventKind::inputs_changed && ev.kind != EventKind::outputs_changed)
        return;
    enqueue(std::move(item));
}

void WebServer::Impl::onRuntimeChange()
{
    // The overlay (overlay_active) may have changed without an Engine event.
    if (stopped || ws_open == 0)
        return;
    enqueue(Item{EventKind::filters_changed, std::string()});
    enqueue(Item{EventKind::lists_changed, std::string()});
}

void WebServer::Impl::enqueue(Item&& item)
{
    pending.push_back(std::move(item));
    if (flush_posted)
        return;
    flush_posted = true;
    auto self = shared_from_this();
    asio::post(io, [self] { self->flush(); });
}

void WebServer::Impl::flush()
{
    flush_posted = false;
    const uint64_t this_batch = batch++;
    if (stopped || pending.empty()) {
        pending.clear();
        return;
    }
    // One inputs item per batch, at the place of the last change: the earlier
    // ones stay "" and are left out (a client that joined in between has the
    // earlier changes in its snapshot and still gets the last one).
    size_t last_inputs = pending.size(), last_outputs = pending.size();
    for (size_t i = 0; i < pending.size(); ++i) {
        auto& item = pending[i];
        if (item.kind == EventKind::filters_changed)
            item.json = webapi::filtersJson(runtime, "filters");
        else if (item.kind == EventKind::lists_changed)
            item.json = webapi::listsJson(engine, runtime, "lists");
        else if (item.kind == EventKind::inputs_changed)
            last_inputs = i;
        else if (item.kind == EventKind::outputs_changed)
            last_outputs = i;
    }
    if (last_inputs < pending.size())
        pending[last_inputs].json = webapi::inputsJson(engine.inputs(), "inputs");
    if (last_outputs < pending.size() && engine.hub())
        pending[last_outputs].json = webapi::outputsJson(*engine.hub(), "outputs");
    // The items from `from` on, as one message; null when none has JSON.
    auto message = [this](size_t from) -> std::shared_ptr<const std::string> {
        std::string msg = "{\"type\":\"events\",\"items\":[";
        bool first = true;
        for (size_t i = from; i < pending.size(); ++i) {
            if (pending[i].json.empty())
                continue;
            if (!first)
                msg += ',';
            first = false;
            msg += pending[i].json;
        }
        if (first)
            return nullptr;
        msg += "]}";
        return std::make_shared<const std::string>(std::move(msg));
    };
    const auto all = message(0);
    std::vector<std::pair<std::shared_ptr<WsSession>, std::shared_ptr<const std::string>>> targets;
    for (const auto& kv : ws_sessions) {
        auto s = kv.second.lock();
        if (!s || !s->open)
            continue;
        if (s->skip_batch != this_batch) {
            if (all)
                targets.emplace_back(std::move(s), all);
        } else if (s->skip_items < pending.size()) {
            // Joined during this batch: only what happened after its snapshot.
            if (auto rest = message(s->skip_items))
                targets.emplace_back(std::move(s), std::move(rest));
        }
    }
    pending.clear();
    for (const auto& t : targets)
        t.first->send(t.second);  // may close it (too slow), which leaves ws_sessions
}

void WebServer::Impl::wsOpened(WsSession* s)
{
    // The events collected so far in this batch are in its snapshot already.
    if (!pending.empty()) {
        s->skip_batch = batch;
        s->skip_items = pending.size();
    }
    ++ws_open;
    engine.setWebClientCount(static_cast<uint32_t>(ws_open));
    LOG_INFO("web: websocket client " << s->peer << " connected (" << ws_open << " now)");
}

void WebServer::Impl::wsClosed(WsSession* s, bool was_open)
{
    ws_sessions.erase(s);
    if (!was_open || stopped)
        return;
    --ws_open;
    engine.setWebClientCount(static_cast<uint32_t>(ws_open));
    LOG_INFO("web: websocket client " << s->peer << " disconnected (" << ws_open << " now)");
}

// ---- WebServer ----

WebServer::WebServer(boost::asio::io_context& io, Engine& engine, RuntimeConfig& runtime, WebConfig config)
    : impl_(std::make_shared<Impl>(io, engine, runtime, std::move(config)))
{
}

WebServer::~WebServer() { impl_->stop(); }

void WebServer::start() { impl_->start(); }
void WebServer::stop() { impl_->stop(); }
uint16_t WebServer::localPort() const { return impl_->port; }
size_t WebServer::webSocketClients() const { return impl_->ws_open; }

}  // namespace skycued
