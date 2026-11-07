#pragma once

// Framers cut a byte stream (or a datagram) into messages. A framer is fed
// whatever the transport delivered and calls out() once per complete message.
// The pointer given to out() is valid only during the call.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace skycue::net {

class Framer {
public:
    virtual ~Framer() = default;
    using Out = std::function<void(const uint8_t*, std::size_t)>;
    virtual void feed(const uint8_t* data, std::size_t n, const Out& out) = 0;
    virtual void reset() = 0; // drop any partial message (stream closed)
};

// '\n'-delimited, every '\r' dropped, empty lines skipped. A line longer than
// max_line is dropped whole (and counted), the lines after it are kept.
class LineFramer final : public Framer {
public:
    explicit LineFramer(std::size_t max_line = 65536);
    void feed(const uint8_t* data, std::size_t n, const Out& out) override;
    void reset() override;
    uint64_t rejected() const { return rejected_; }

private:
    std::size_t max_line_;
    std::vector<uint8_t> partial_;
    bool discarding_ = false;
    uint64_t rejected_ = 0;
};

// One datagram is one message.
class DatagramFramer final : public Framer {
public:
    void feed(const uint8_t* data, std::size_t n, const Out& out) override { out(data, n); }
    void reset() override {}
};

} // namespace skycue::net
