#include "skycue/net/framer.hpp"

#include "skycue/net/buffer.hpp"

#include <algorithm>

namespace skycue::net {

Buf makeBuf(const uint8_t* data, std::size_t n)
{
    return std::make_shared<const std::vector<uint8_t>>(data, data + n);
}

Buf makeBuf(std::string_view s)
{
    const auto* p = reinterpret_cast<const uint8_t*>(s.data());
    return makeBuf(p, s.size());
}

LineFramer::LineFramer(std::size_t max_line) : max_line_(max_line) {}

void LineFramer::reset()
{
    partial_.clear();
    discarding_ = false;
}

void LineFramer::feed(const uint8_t* data, std::size_t n, const Out& out)
{
    const uint8_t* const end = data + n;
    while (data != end) {
        const uint8_t* nl = std::find(data, end, static_cast<uint8_t>('\n'));
        const bool complete = nl != end;
        if (!discarding_) {
            for (const uint8_t* p = data; p != nl; ++p) {
                if (*p == '\r')
                    continue;
                if (partial_.size() >= max_line_) {
                    discarding_ = true;
                    partial_.clear();
                    ++rejected_;
                    break;
                }
                partial_.push_back(*p);
            }
        }
        if (complete) {
            if (!discarding_) {
                if (!partial_.empty())
                    out(partial_.data(), partial_.size());
            }
            partial_.clear();
            discarding_ = false;
            data = nl + 1;
        } else {
            data = end;
        }
    }
}

} // namespace skycue::net
