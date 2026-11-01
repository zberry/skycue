#include <skycue/protocol.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace skycue {

static_assert(sizeof(float) == 4, "float must be 32-bit IEEE 754");

namespace {

// Presence bits for TrackUpdate optional fields, in wire order.
enum PresenceBit : unsigned {
    p_alt_baro = 0,
    p_alt_geom = 1,
    p_ground_speed = 2,
    p_track = 3,
    p_vertical_rate = 4,
    p_callsign = 5,
    p_squawk = 6,
    p_registration = 7,
    p_nic = 8,
    p_nac_p = 9,
    p_known_count = 10,
};

constexpr uint8_t kFlagKeyframe = 0x01;
constexpr uint8_t kFlagOnGround = 0x02;
constexpr uint8_t kFlagAltIsGeom = 0x04;

class Writer {
public:
    explicit Writer(FrameType type)
    {
        buf_.reserve(64);
        for (char c : kMagic)
            buf_.push_back(static_cast<uint8_t>(c));
        buf_.push_back(kVersion);
        buf_.push_back(static_cast<uint8_t>(type));
        u16(0); // length, patched in finish()
    }

    void u8(uint8_t v) { buf_.push_back(v); }
    void u16(uint16_t v)
    {
        buf_.push_back(static_cast<uint8_t>(v & 0xFFu));
        buf_.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    }
    void u32(uint32_t v)
    {
        for (unsigned i = 0; i < 4; ++i)
            buf_.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFFu));
    }
    void u64(uint64_t v)
    {
        for (unsigned i = 0; i < 8; ++i)
            buf_.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFFu));
    }
    void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }
    void f32(float v)
    {
        uint32_t bits = 0;
        std::memcpy(&bits, &v, sizeof bits);
        u32(bits);
    }
    void str(const std::string& s, size_t max_len)
    {
        size_t n = std::min(s.size(), max_len);
        u8(static_cast<uint8_t>(n));
        buf_.insert(buf_.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
    }

    std::vector<uint8_t> finish()
    {
        // Every payload this encoder builds is far below 64 KiB.
        size_t len = buf_.size() - kHeaderSize;
        buf_[6] = static_cast<uint8_t>(len & 0xFFu);
        buf_[7] = static_cast<uint8_t>((len >> 8) & 0xFFu);
        return std::move(buf_);
    }

private:
    std::vector<uint8_t> buf_;
};

// Bounds-checked little-endian reader. Every read returns false and leaves the
// output untouched if fewer bytes remain than requested.
class Reader {
public:
    explicit Reader(const std::vector<uint8_t>& v) : p_(v.data()), n_(v.size()) {}

    bool u8(uint8_t& v)
    {
        if (n_ - pos_ < 1)
            return false;
        v = p_[pos_++];
        return true;
    }
    bool u16(uint16_t& v)
    {
        uint64_t x = 0;
        if (!le(2, x))
            return false;
        v = static_cast<uint16_t>(x);
        return true;
    }
    bool u32(uint32_t& v)
    {
        uint64_t x = 0;
        if (!le(4, x))
            return false;
        v = static_cast<uint32_t>(x);
        return true;
    }
    bool u64(uint64_t& v) { return le(8, v); }
    bool i32(int32_t& v)
    {
        uint32_t u = 0;
        if (!u32(u))
            return false;
        v = static_cast<int32_t>(u);
        return true;
    }
    bool f32(float& v)
    {
        uint32_t bits = 0;
        if (!u32(bits))
            return false;
        std::memcpy(&v, &bits, sizeof v);
        return true;
    }
    bool str(std::string& s, size_t max_len)
    {
        uint8_t len = 0;
        if (!u8(len))
            return false;
        if (len > max_len || n_ - pos_ < len)
            return false;
        s.assign(reinterpret_cast<const char*>(p_ + pos_), len);
        pos_ += len;
        return true;
    }

private:
    bool le(size_t bytes, uint64_t& v)
    {
        if (n_ - pos_ < bytes)
            return false;
        uint64_t x = 0;
        for (size_t i = 0; i < bytes; ++i)
            x |= static_cast<uint64_t>(p_[pos_ + i]) << (8 * i);
        pos_ += bytes;
        v = x;
        return true;
    }

    const uint8_t* p_;
    size_t n_;
    size_t pos_ = 0;
};

int32_t scaleDegrees(double deg, double limit)
{
    if (!std::isfinite(deg))
        return 0;
    deg = std::max(-limit, std::min(limit, deg));
    return static_cast<int32_t>(std::llround(deg * 1e7));
}

template <typename T> bool readOptional(Reader& r, std::optional<T>& out, bool (Reader::*fn)(T&))
{
    T v{};
    if (!(r.*fn)(v))
        return false;
    out = v;
    return true;
}

bool readOptionalStr(Reader& r, std::optional<std::string>& out, size_t max_len)
{
    std::string s;
    if (!r.str(s, max_len))
        return false;
    out = std::move(s);
    return true;
}

} // namespace

std::vector<uint8_t> encodeHello(const Hello& h)
{
    Writer w(FrameType::hello);
    w.u32(h.keyframe_interval_ms);
    w.u32(h.heartbeat_interval_ms);
    w.str(h.sender, kMaxSenderLen);
    return w.finish();
}

std::vector<uint8_t> encodeTrackUpdate(const Track& t)
{
    Writer w(FrameType::track_update);
    w.u32(t.id.address);
    w.u8(static_cast<uint8_t>(t.id.type));
    w.u8(static_cast<uint8_t>(t.source));
    uint8_t flags = 0;
    if (t.keyframe)
        flags |= kFlagKeyframe;
    if (t.on_ground)
        flags |= kFlagOnGround;
    if (t.alt_is_geom)
        flags |= kFlagAltIsGeom;
    w.u8(flags);
    w.u8(0); // reserved
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t.position_time.time_since_epoch()).count();
    w.u64(static_cast<uint64_t>(ms));
    w.i32(scaleDegrees(t.lat_deg, 90.0));
    w.i32(scaleDegrees(t.lon_deg, 180.0));

    uint16_t presence = 0;
    auto mark = [&presence](bool set, unsigned bit) {
        if (set)
            presence = static_cast<uint16_t>(presence | (1u << bit));
    };
    mark(t.alt_baro_ft.has_value(), p_alt_baro);
    mark(t.alt_geom_ft.has_value(), p_alt_geom);
    mark(t.ground_speed_kt.has_value(), p_ground_speed);
    mark(t.track_deg.has_value(), p_track);
    mark(t.vertical_rate_fpm.has_value(), p_vertical_rate);
    mark(t.callsign.has_value(), p_callsign);
    mark(t.squawk.has_value(), p_squawk);
    mark(t.registration.has_value(), p_registration);
    mark(t.nic.has_value(), p_nic);
    mark(t.nac_p.has_value(), p_nac_p);
    w.u16(presence);

    if (t.alt_baro_ft)
        w.i32(*t.alt_baro_ft);
    if (t.alt_geom_ft)
        w.i32(*t.alt_geom_ft);
    if (t.ground_speed_kt)
        w.f32(*t.ground_speed_kt);
    if (t.track_deg)
        w.f32(*t.track_deg);
    if (t.vertical_rate_fpm)
        w.f32(*t.vertical_rate_fpm);
    if (t.callsign)
        w.str(*t.callsign, kMaxCallsignLen);
    if (t.squawk)
        w.str(*t.squawk, kMaxSquawkLen);
    if (t.registration)
        w.str(*t.registration, kMaxRegistrationLen);
    if (t.nic)
        w.u8(*t.nic);
    if (t.nac_p)
        w.u8(*t.nac_p);
    return w.finish();
}

std::vector<uint8_t> encodeTrackDrop(const TrackDrop& d)
{
    Writer w(FrameType::track_drop);
    w.u32(d.id.address);
    w.u8(static_cast<uint8_t>(d.id.type));
    w.u8(static_cast<uint8_t>(d.reason));
    return w.finish();
}

std::vector<uint8_t> encodeHeartbeat(const Heartbeat& hb)
{
    Writer w(FrameType::heartbeat);
    w.u64(hb.sender_time_ms);
    return w.finish();
}

DecodeStatus decodeFrame(const uint8_t* data, size_t len, Frame& out, size_t& consumed)
{
    consumed = 0;
    if (data == nullptr || len < kHeaderSize)
        return DecodeStatus::need_more;
    for (size_t i = 0; i < sizeof kMagic; ++i) {
        if (data[i] != static_cast<uint8_t>(kMagic[i]))
            return DecodeStatus::bad_magic;
    }
    if (data[4] != kVersion)
        return DecodeStatus::bad_version;
    size_t payload_len = static_cast<size_t>(data[6]) | (static_cast<size_t>(data[7]) << 8);
    if (len - kHeaderSize < payload_len)
        return DecodeStatus::need_more;

    out.type = static_cast<FrameType>(data[5]);
    out.payload.assign(data + kHeaderSize, data + kHeaderSize + payload_len);
    consumed = kHeaderSize + payload_len;
    return DecodeStatus::ok;
}

bool decodeHello(const Frame& f, Hello& out)
{
    if (f.type != FrameType::hello)
        return false;
    Reader r(f.payload);
    return r.u32(out.keyframe_interval_ms) && r.u32(out.heartbeat_interval_ms) && r.str(out.sender, kMaxSenderLen);
}

bool decodeTrackUpdate(const Frame& f, Track& out)
{
    if (f.type != FrameType::track_update)
        return false;
    Reader r(f.payload);
    uint32_t address = 0;
    uint8_t type = 0, source = 0, flags = 0, reserved = 0;
    uint64_t ms = 0;
    int32_t lat = 0, lon = 0;
    uint16_t presence = 0;
    if (!(r.u32(address) && r.u8(type) && r.u8(source) && r.u8(flags) && r.u8(reserved) && r.u64(ms) &&
          r.i32(lat) && r.i32(lon) && r.u16(presence)))
        return false;

    Track t;
    t.id = TrackId{address, static_cast<AddressType>(type)};
    t.source = static_cast<Source>(source);
    t.keyframe = (flags & kFlagKeyframe) != 0;
    t.on_ground = (flags & kFlagOnGround) != 0;
    t.alt_is_geom = (flags & kFlagAltIsGeom) != 0;
    t.position_time = std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::milliseconds(static_cast<int64_t>(ms))));
    t.lat_deg = lat / 1e7;
    t.lon_deg = lon / 1e7;

    // Optional fields appear in bit order. A set bit this version does not know
    // means the rest of the payload has a layout we cannot parse: keep what has
    // been read so far and stop.
    for (unsigned bit = 0; bit < 16; ++bit) {
        if ((presence & (1u << bit)) == 0)
            continue;
        bool ok = false;
        switch (bit) {
        case p_alt_baro: ok = readOptional(r, t.alt_baro_ft, &Reader::i32); break;
        case p_alt_geom: ok = readOptional(r, t.alt_geom_ft, &Reader::i32); break;
        case p_ground_speed: ok = readOptional(r, t.ground_speed_kt, &Reader::f32); break;
        case p_track: ok = readOptional(r, t.track_deg, &Reader::f32); break;
        case p_vertical_rate: ok = readOptional(r, t.vertical_rate_fpm, &Reader::f32); break;
        case p_callsign: ok = readOptionalStr(r, t.callsign, kMaxCallsignLen); break;
        case p_squawk: ok = readOptionalStr(r, t.squawk, kMaxSquawkLen); break;
        case p_registration: ok = readOptionalStr(r, t.registration, kMaxRegistrationLen); break;
        case p_nic: ok = readOptional(r, t.nic, &Reader::u8); break;
        case p_nac_p: ok = readOptional(r, t.nac_p, &Reader::u8); break;
        default: out = std::move(t); return true; // unknown bit
        }
        if (!ok)
            return false;
    }
    out = std::move(t);
    return true;
}

bool decodeTrackDrop(const Frame& f, TrackDrop& out)
{
    if (f.type != FrameType::track_drop)
        return false;
    Reader r(f.payload);
    uint32_t address = 0;
    uint8_t type = 0, reason = 0;
    if (!(r.u32(address) && r.u8(type) && r.u8(reason)))
        return false;
    out.id = TrackId{address, static_cast<AddressType>(type)};
    out.reason = static_cast<DropReason>(reason);
    return true;
}

bool decodeHeartbeat(const Frame& f, Heartbeat& out)
{
    if (f.type != FrameType::heartbeat)
        return false;
    Reader r(f.payload);
    return r.u64(out.sender_time_ms);
}

} // namespace skycue
