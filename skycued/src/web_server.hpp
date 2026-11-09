#pragma once

// WebServer: the embedded HTTP/1.1 + WebSocket server behind the web UI
// (Boost.Beast). Routes and payloads are in docs/web-api.md; the JSON is
// built by web_api.hpp.
//
// HTTP: each connection reads one request at a time (body at most 64 KiB,
// else 413 and the connection is closed), answers it and keeps the
// connection open when the request asks for that; a connection idle for
// idle_timeout is closed. Static files come from www_dir only (see
// webapi::staticPath), those under /tiles/ and /overlays/ from map_dir
// instead when it is set; when www_dir does not exist every static path is 404
// but the API still works.
//
// WebSocket (GET /ws): on upgrade the client gets {"type":"snapshot", ...}
// (the /api/state object), then the Engine's events, batched: every event of
// one io turn goes out as one {"type":"events","items":[...]} message. The
// WebServer is the Engine's event sink (Engine::onEvent) and fans out to
// every client; it is also the RuntimeConfig's change callback
// (RuntimeConfig::onChange), on which it sends the filters and lists items,
// so every page sees overlay_active change after "save as baseline". The
// inputs_changed events of one io turn become a single {"type":"inputs"}
// item (webapi::inputsJson), built when the message is; the outputs_changed
// events likewise become one {"type":"outputs"} item (webapi::outputsJson). Each client has at most one write in flight and a queue of
// messages behind it; a client whose queued bytes (the one in flight
// included) would pass max_ws_queue_bytes is disconnected (WARN), without
// affecting the others. A ping goes out every ping_interval; a client that
// has sent nothing (no pong) for two intervals is disconnected. Text the
// client sends is ignored (DEBUG): commands go through the HTTP API.
// Engine::setWebClientCount is kept equal to the number of connected clients.
//
// Threading: lives on the Engine's io_context; call everything from its
// thread. The Engine and the RuntimeConfig must outlive it.

#include <boost/asio/io_context.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace skycued {

class Engine;
class RuntimeConfig;
struct DaemonInfo;

struct WebConfig {
    std::string bind = "127.0.0.1";  // numeric address to listen on
    uint16_t port = 8080;            // 0: a port the OS picks (see localPort)
    std::string www_dir;             // static files
    // /tiles/*, /overlays/* and /api/tiles-available (paths.map_dir); "": www_dir.
    // When set, those paths are read from here only, never from www_dir.
    std::string map_dir;
    const DaemonInfo* daemon = nullptr;  // for GET /api/config; must outlive the server
    // POST /api/restart calls this (web.allow_restart); it must only schedule the shutdown, so the
    // reply can still be written. Empty: the route answers 500.
    std::function<void()> on_restart;
    size_t max_ws_queue_bytes = 2u << 20;
    std::chrono::milliseconds ping_interval{15000};
    std::chrono::milliseconds idle_timeout{30000};  // HTTP keep-alive connections
};

class WebServer {
public:
    WebServer(boost::asio::io_context& io, Engine& engine, RuntimeConfig& runtime, WebConfig config);
    ~WebServer();  // stop()
    WebServer(const WebServer&) = delete;
    WebServer& operator=(const WebServer&) = delete;

    // Listens and becomes the Engine's event sink and the RuntimeConfig's
    // change callback. Throws std::runtime_error
    // ("web: cannot listen on <bind>:<port>: <reason>") when it cannot bind.
    // Logs a WARN when www_dir does not exist, and one when map_dir is set
    // and does not exist. Once.
    void start();
    // Closes the listener and every HTTP and WebSocket connection, clears the
    // Engine's event sink and the RuntimeConfig's change callback, and sets
    // the web client count to 0. Idempotent.
    void stop();

    uint16_t localPort() const;     // the bound port; 0 before start()
    size_t webSocketClients() const;  // connected WebSocket clients

    struct Impl;  // internal (named by the session types in web_server.cpp)

private:
    std::shared_ptr<Impl> impl_;
};

}  // namespace skycued
