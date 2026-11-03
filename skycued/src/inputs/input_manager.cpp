#include "inputs/input_manager.hpp"

#include "file_util.hpp"
#include "inputs/dump978_input.hpp"
#include "inputs/inputs_file.hpp"
#include "inputs/ping_cat021_input.hpp"
#include "inputs/ping_json_input.hpp"
#include "inputs/source_input.hpp"
#include "json_lite.hpp"
#include "log.hpp"
#include "readsb_json_source.hpp"

#include <boost/asio/post.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <utility>

namespace skycued {

namespace {

// Restores dispatching_ even when a handler throws.
struct Dispatch {
    explicit Dispatch(int& depth) : depth_(depth) { ++depth_; }
    ~Dispatch() { --depth_; }
    Dispatch(const Dispatch&) = delete;
    Dispatch& operator=(const Dispatch&) = delete;
    int& depth_;
};

// Up to one byte more than parseInputsFile accepts, so it sees the excess.
bool readCapped(const std::string& path, std::string& text)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    text.resize(kMaxInputsFileBytes + 1);
    in.read(&text[0], static_cast<std::streamsize>(text.size()));
    if (in.bad())
        return false;
    text.resize(static_cast<std::size_t>(in.gcount()));
    return true;
}

bool sameExceptEnabled(InputDef a, const InputDef& b)
{
    a.enabled = b.enabled;
    return a == b;
}

}  // namespace

UplinkDedupe::UplinkDedupe(std::chrono::milliseconds window) : window_(window) {}

bool UplinkDedupe::duplicate(const std::string& input_id, std::chrono::steady_clock::time_point now,
                             const std::string& hex)
{
    while (!seen_.empty() && now - seen_.front().at > window_)
        seen_.pop_front();
    for (const auto& s : seen_)
        if (s.hex == hex && s.input_id != input_id)
            return true;
    seen_.push_back(Seen{now, input_id, hex});
    return false;
}

std::unique_ptr<Input> makeInput(boost::asio::io_context& io, const InputDef& def, const InputManagerConfig& /*config*/)
{
    switch (def.type) {
    case InputType::readsb: {
        const std::string host = def.host;
        const uint16_t port = def.port;
        return std::make_unique<SourceInput>(
            def, SourceInput::SourceFactory([&io, host, port]() -> std::unique_ptr<ObservationSource> {
                return std::make_unique<ReadsbJsonSource>(io, host, port);
            }));
    }
    case InputType::dump978:
        return std::make_unique<Dump978Input>(io, def);
    case InputType::pingstation_json:
        return std::make_unique<PingJsonInput>(io, def);
    case InputType::pingstation_cat021:
        return std::make_unique<PingCat021Input>(io, def);
    case InputType::replay:
        break;
    }
    throw std::invalid_argument(std::string("makeInput: no input for type ") + inputTypeName(def.type));
}

InputManager::InputManager(boost::asio::io_context& io, InputManagerConfig config)
    : io_(io), config_(std::move(config)), timer_(io)
{
    if (!config_.inputs_file.empty())
        watch_ = std::make_unique<SettledFile>(config_.inputs_file);
}

InputManager::~InputManager() { stop(); }

void InputManager::addBuiltin(std::unique_ptr<Input> input)
{
    if (!input)
        throw std::invalid_argument("addBuiltin: no input");
    InputDef def = input->def();
    def.builtin = true;
    if (slot(def.id))
        throw std::invalid_argument("addBuiltin: duplicate id " + def.id);
    builtin_default_[def.id] = def.enabled;
    const auto it = file_builtin_.find(def.id);
    if (it != file_builtin_.end())
        def.enabled = it->second;
    attach(*input);
    slots_.insert(slots_.begin() + static_cast<std::ptrdiff_t>(builtin_count_), Slot{def, std::move(input)});
    ++builtin_count_;
    if (started_ && def.enabled)
        slots_[builtin_count_ - 1].input->start();
}

void InputManager::load()
{
    if (!watch_ || config_.replaying)
        return;
    const std::string& path = config_.inputs_file;
    if (!FileStamp::of(path).exists) {
        watch_->accept();
        return;
    }
    std::string text;
    if (!readCapped(path, text)) {
        LOG_ERROR(path << ": cannot read: " << std::strerror(errno) << "; built-in inputs only");
        watch_->accept();
        return;
    }
    watch_->accept();
    InputsFile file;
    try {
        file = parseInputsFile(text);
        checkSet(file.inputs);
    } catch (const InputDefError& e) {
        rejectFile(e.what(), "built-in inputs only");
        return;
    }
    file_builtin_ = file.builtin_enabled;
    for (std::size_t i = 0; i < builtin_count_; ++i) {
        const auto it = file_builtin_.find(slots_[i].def.id);
        if (it != file_builtin_.end())
            slots_[i].def.enabled = it->second;
    }
    for (const auto& d : file.inputs)
        slots_.push_back(Slot{d, build(d)});
}

void InputManager::onObservation(std::function<void(const TrackObservation&)> handler)
{
    on_observation_ = std::move(handler);
}

void InputManager::onUplink(
    std::function<void(const std::string&, std::chrono::system_clock::time_point, const std::string&)> handler)
{
    on_uplink_ = std::move(handler);
}

void InputManager::onChanged(std::function<void()> handler) { on_changed_ = std::move(handler); }

void InputManager::start()
{
    if (started_)
        return;
    started_ = true;
    if (config_.inputs_file.empty())
        LOG_WARN("inputs not persisted (no inputs file)");
    for (auto& s : slots_)
        if (s.def.enabled)
            s.input->start();
    schedule();
}

void InputManager::stop()
{
    if (!started_)
        return;
    started_ = false;
    timer_.cancel();
    for (auto& s : slots_)
        s.input->stop();
}

std::vector<InputEntry> InputManager::list() const
{
    std::vector<InputEntry> out;
    out.reserve(slots_.size());
    for (const auto& s : slots_)
        out.push_back(entry(s));
    return out;
}

std::optional<InputEntry> InputManager::find(const std::string& id) const
{
    const Slot* s = slot(id);
    if (!s)
        return std::nullopt;
    return entry(*s);
}

InputEntry InputManager::create(const InputDef& def)
{
    checkWritable();
    const InputDef d = normalized(def);
    if (slot(d.id))
        throw InputConflict("input '" + d.id + "' already exists");
    auto user = userDefs();
    user.push_back(d);
    checkSet(user);
    persist(user, builtinStates());
    slots_.push_back(Slot{d, build(d)});
    if (started_ && d.enabled)
        slots_.back().input->start();
    changed();
    return entry(slots_.back());
}

InputEntry InputManager::replace(const std::string& id, const InputDef& def)
{
    checkWritable();
    Slot* s = slot(id);
    if (!s)
        throw InputNotFound("no input '" + id + "'");
    if (s->def.builtin)
        throw InputForbidden("input '" + id + "' is built in: it can only be enabled or disabled");
    const InputDef d = normalized(def);
    if (d.id != id)
        throw InputDefError("id must stay '" + id + "'");
    if (d == s->def)
        return entry(*s);  // unchanged: no write, no restart
    auto user = userDefs();
    for (auto& u : user)
        if (u.id == id)
            u = d;
    checkSet(user);
    persist(user, builtinStates());
    retire(*s, true);
    s->def = d;
    s->input = build(d);
    if (started_ && d.enabled)
        s->input->start();
    changed();
    return entry(*s);
}

InputEntry InputManager::setEnabled(const std::string& id, bool enabled)
{
    checkWritable();
    Slot* s = slot(id);
    if (!s)
        throw InputNotFound("no input '" + id + "'");
    if (s->def.enabled == enabled)
        return entry(*s);
    auto user = userDefs();
    auto builtin = builtinStates();
    if (s->def.builtin) {
        builtin[id] = enabled;
    } else {
        for (auto& u : user)
            if (u.id == id)
                u.enabled = enabled;
        checkSet(user);
    }
    persist(user, builtin);
    s->def.enabled = enabled;
    if (!enabled)
        s->input->stop();
    else if (started_)
        s->input->start();
    changed();
    return entry(*s);
}

void InputManager::remove(const std::string& id)
{
    checkWritable();
    Slot* s = slot(id);
    if (!s)
        throw InputNotFound("no input '" + id + "'");
    if (s->def.builtin)
        throw InputForbidden("input '" + id + "' is built in: it can only be enabled or disabled");
    auto user = userDefs();
    user.erase(std::remove_if(user.begin(), user.end(), [&](const InputDef& u) { return u.id == id; }), user.end());
    persist(user, builtinStates());
    retire(*s, false);
    slots_.erase(slots_.begin() + (s - slots_.data()));
    changed();
}

bool InputManager::anyTrackInputConnected() const
{
    for (const auto& s : slots_)
        if (s.def.enabled && isTrackType(s.def.type) && s.input->state().connected)
            return true;
    return false;
}

std::size_t InputManager::connectedCount() const
{
    std::size_t n = 0;
    for (const auto& s : slots_)
        if (s.def.enabled && s.input->state().connected)
            ++n;
    return n;
}

std::size_t InputManager::enabledCount() const
{
    std::size_t n = 0;
    for (const auto& s : slots_)
        if (s.def.enabled)
            ++n;
    return n;
}

std::vector<InputStatsLine> InputManager::statsLines() const
{
    std::vector<InputStatsLine> out;
    for (const auto& s : slots_) {
        const InputEntry e = entry(s);
        out.push_back(InputStatsLine{e.def.id, e.state.connected, e.state.counters.messages,
                                     e.state.counters.observations, e.state.counters.uplinks,
                                     e.state.counters.rejected});
    }
    return out;
}

const Input* InputManager::builtin(const std::string& id) const
{
    const Slot* s = slot(id);
    return s && s->def.builtin ? s->input.get() : nullptr;
}

InputManager::Slot* InputManager::slot(const std::string& id)
{
    for (auto& s : slots_)
        if (s.def.id == id)
            return &s;
    return nullptr;
}

const InputManager::Slot* InputManager::slot(const std::string& id) const
{
    for (const auto& s : slots_)
        if (s.def.id == id)
            return &s;
    return nullptr;
}

InputEntry InputManager::entry(const Slot& s) const
{
    InputEntry e{s.def, s.input->state()};
    const auto it = retired_.find(s.def.id);
    if (it != retired_.end())
        e.state.counters += it->second;
    return e;
}

void InputManager::checkWritable() const
{
    if (config_.replaying)
        throw InputConflict("replaying: inputs cannot be changed");
}

InputDef InputManager::normalized(const InputDef& def)
{
    // The rules live in parseInputDef: run the definition through it.
    InputDef user = def;
    user.builtin = false;
    const auto object = JsonObject::parse(inputDefJson(user));
    if (!object)
        throw InputDefError("definition cannot be encoded");
    return parseInputDef(*object);
}

void InputManager::checkSet(const std::vector<InputDef>& user) const
{
    std::vector<InputDef> all;
    for (std::size_t i = 0; i < builtin_count_; ++i)
        all.push_back(slots_[i].def);
    all.insert(all.end(), user.begin(), user.end());
    validateInputSet(all);
    if (config_.record_dir.empty())
        for (const auto& d : user)
            if (d.capture)
                throw InputDefError("capture needs RECORD_DIR");
}

std::vector<InputDef> InputManager::userDefs() const
{
    std::vector<InputDef> out;
    for (std::size_t i = builtin_count_; i < slots_.size(); ++i)
        out.push_back(slots_[i].def);
    return out;
}

std::map<std::string, bool> InputManager::builtinStates() const
{
    std::map<std::string, bool> out = file_builtin_;
    for (std::size_t i = 0; i < builtin_count_; ++i)
        out[slots_[i].def.id] = slots_[i].def.enabled;
    return out;
}

void InputManager::persist(const std::vector<InputDef>& user, const std::map<std::string, bool>& builtin_enabled)
{
    if (!watch_)
        return;
    InputsFile file;
    file.inputs = user;
    file.builtin_enabled = builtin_enabled;
    atomicWrite(config_.inputs_file, inputsFileJson(file));
    watch_->accept();
    file_builtin_ = builtin_enabled;
}

std::unique_ptr<Input> InputManager::build(const InputDef& def)
{
    auto input = makeInput(io_, def, config_);
    attach(*input);
    return input;
}

void InputManager::attach(Input& input)
{
    input.onObservation([this](const TrackObservation& obs) {
        Dispatch d(dispatching_);
        if (on_observation_)
            on_observation_(obs);
    });
    input.onUplink([this, id = input.def().id](std::chrono::system_clock::time_point t, const std::string& hex) {
        Dispatch d(dispatching_);
        if (on_uplink_)
            on_uplink_(id, t, hex);
    });
    input.onConnection([this](bool) {
        Dispatch d(dispatching_);
        changed();
    });
    // No onStatus: a device status changes device_status only, read through
    // state(); firing onChanged for it would resend the inputs list on every
    // status datagram or poll.
}

void InputManager::retire(Slot& s, bool keep_counters)
{
    s.input->stop();
    if (keep_counters)
        retired_[s.def.id] += s.input->state().counters;
    else
        retired_.erase(s.def.id);
    dispose(std::move(s.input));
}

void InputManager::dispose(std::unique_ptr<Input> input)
{
    if (dispatching_ == 0)
        return;  // nothing of it is on the stack: `input` goes now
    // Destroyed from the io_context, after the callback that stopped it returns.
    std::shared_ptr<Input> later(std::move(input));
    boost::asio::post(io_, [later]() {});
}

void InputManager::schedule()
{
    timer_.expires_after(config_.poll_interval);
    // A wait that completed before stop() or the destructor cancelled it still
    // runs: the weak token tells it whether the manager is still there.
    std::weak_ptr<bool> alive = alive_;
    timer_.async_wait([this, alive](const boost::system::error_code& ec) {
        if (ec || !alive.lock() || !started_)
            return;
        onTimer();
        // onTimer() runs input callbacks, which may destroy the manager.
        if (!alive.expired() && started_)
            schedule();
    });
}

void InputManager::onTimer()
{
    const auto now = std::chrono::system_clock::now();
    for (auto& s : slots_)
        s.input->tick(now);
    if (watch_ && !config_.replaying && watch_->poll())
        reloadFromDisk();
}

void InputManager::reloadFromDisk()
{
    const std::string& path = config_.inputs_file;
    if (!watch_->stamp().exists)
        return;  // gone (or renamed .rejected): keep running, keep watching
    std::string text;
    if (!readCapped(path, text)) {
        LOG_ERROR(path << ": cannot read: " << std::strerror(errno) << "; inputs unchanged");
        return;
    }
    InputsFile file;
    try {
        file = parseInputsFile(text);
        checkSet(file.inputs);
    } catch (const InputDefError& e) {
        rejectFile(e.what(), "inputs unchanged");
        return;
    }
    applyFile(file.inputs, file.builtin_enabled);
}

void InputManager::applyFile(const std::vector<InputDef>& user, const std::map<std::string, bool>& builtin_enabled)
{
    file_builtin_ = builtin_enabled;
    for (std::size_t i = 0; i < builtin_count_; ++i) {
        Slot& s = slots_[i];
        const auto it = builtin_enabled.find(s.def.id);
        const bool want = it != builtin_enabled.end() ? it->second : builtin_default_[s.def.id];
        if (want == s.def.enabled)
            continue;
        s.def.enabled = want;
        if (!want)
            s.input->stop();
        else if (started_)
            s.input->start();
    }

    std::map<std::string, Slot> old;
    for (std::size_t i = builtin_count_; i < slots_.size(); ++i)
        old.emplace(slots_[i].def.id, std::move(slots_[i]));
    slots_.resize(builtin_count_);

    // Stop everything that goes or changes first, so a port moved from one
    // input to another is free before the new one binds.
    for (auto& [id, s] : old) {
        const auto next = std::find_if(user.begin(), user.end(), [&](const InputDef& d) { return d.id == id; });
        if (next == user.end())
            retire(s, false);
        else if (!sameExceptEnabled(s.def, *next))
            retire(s, true);
        else if (!next->enabled)
            s.input->stop();
    }
    for (const auto& d : user) {
        const auto it = old.find(d.id);
        if (it != old.end() && it->second.input) {
            it->second.def = d;
            slots_.push_back(std::move(it->second));
        } else {
            slots_.push_back(Slot{d, build(d)});
        }
        if (started_ && d.enabled)
            slots_.back().input->start();
    }
    changed();
}

void InputManager::rejectFile(const std::string& why, const char* consequence)
{
    const std::string& path = config_.inputs_file;
    const std::string rejected = path + ".rejected";
    if (std::rename(path.c_str(), rejected.c_str()) == 0)
        LOG_ERROR(path << ": " << why << "; renamed to " << rejected << ", " << consequence);
    else
        LOG_ERROR(path << ": " << why << "; " << consequence << " (cannot rename to " << rejected << ": "
                       << std::strerror(errno) << ")");
    watch_->accept();
}

void InputManager::changed()
{
    if (on_changed_)
        on_changed_();
}

}  // namespace skycued
