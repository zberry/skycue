#pragma once

// The stubbed daemon fuzz_web and fuzz_ws answer API requests with: an
// Engine with no recorders and no inputs, its InputManager in replay mode
// (so every /api/inputs write is a 409 and no listener is ever opened), a
// Forwarder that is never started, a RuntimeConfig on a skycue.conf of its
// own and a DaemonInfo for GET /api/config. Header-only.
//
// The files live in a fresh directory under $SKYCUE_FUZZ_WORK/<target>/
// (scripts/dev/run-fuzz.sh exports it), or, for a hand run without it, under
// std::filesystem::temp_directory_path(); it is removed at exit.

#include "config.hpp"
#include "engine.hpp"
#include "forwarder.hpp"
#include "inputs/input_manager.hpp"
#include "runtime_config.hpp"
#include "web_config.hpp"

#include <boost/asio/io_context.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace skycuefuzz {

// A fresh directory for `target`'s files, removed at exit.
inline std::string makeWorkDir(const char* target)
{
    namespace fs = std::filesystem;
    const char* work = std::getenv("SKYCUE_FUZZ_WORK");
    const fs::path base =
        (work != nullptr && *work != '\0') ? fs::path(work) / target : fs::temp_directory_path() / "skycue-fuzz";
    fs::create_directories(base);
    std::string tmpl = (base / (std::string(target) + "-XXXXXX")).string();
    if (mkdtemp(tmpl.data()) == nullptr)
        std::abort();
    static std::string dir;
    dir = tmpl;
    std::atexit([] {
        std::error_code ec;
        fs::remove_all(dir, ec);
    });
    return dir;
}

struct ApiStub {
    boost::asio::io_context& io;
    std::string dir, conf, www;
    skycued::DaemonInfo daemon;
    std::unique_ptr<skycued::Forwarder> fwd;
    std::unique_ptr<skycued::Engine> engine;
    std::unique_ptr<skycued::RuntimeConfig> runtime;

    ApiStub(boost::asio::io_context& io_context, std::string directory)
        : io(io_context), dir(std::move(directory)), conf(dir + "/skycue.conf"), www(dir + "/www")
    {
        daemon.version = "fuzz";
        daemon.config_path = conf;
        daemon.www_dir = www;
        daemon.log_level = "error";
        daemon.inputs_path = dir + "/skycue.inputs.json";
        reset();
    }
    ~ApiStub()
    {
        runtime.reset();
        engine.reset();
        fwd.reset();
    }

    // Back to the starting state: every file the last requests wrote (the
    // overlay, a rewritten skycue.conf, list files) removed, skycue.conf
    // written afresh and the Engine and RuntimeConfig built again on it.
    void reset()
    {
        namespace fs = std::filesystem;
        runtime.reset();
        engine.reset();
        fwd.reset();
        // Collect the entries first: removing them while the same
        // directory_iterator walks the directory is unspecified.
        std::error_code ec;
        std::vector<fs::path> entries;
        for (const auto& entry : fs::directory_iterator(dir, ec))
            entries.push_back(entry.path());
        for (const auto& p : entries)
            fs::remove_all(p, ec);
        fs::create_directories(www);
        std::ofstream(conf) << "RECEIVER_LAT=38.9\nRECEIVER_LON=-77.0\nWEB_PORT=0\nCUE_TCP_PORT=0\n";

        skycued::ForwarderConfig fc;
        fc.tcp_listen_ephemeral = true;
        fwd = std::make_unique<skycued::Forwarder>(io, fc);
        skycued::InputManagerConfig mc;
        mc.inputs_file = daemon.inputs_path;
        mc.replaying = true;
        auto inputs = std::make_unique<skycued::InputManager>(io, mc);
        inputs->load();
        engine = std::make_unique<skycued::Engine>(io, skycued::parseConfigFile(conf), std::move(inputs), *fwd,
                                                   nullptr, nullptr);
        runtime = std::make_unique<skycued::RuntimeConfig>(io, conf, *engine);
        runtime->load();
    }
};

}  // namespace skycuefuzz
