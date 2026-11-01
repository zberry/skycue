#include "filters.hpp"

#include "geo.hpp"
#include "registration.hpp"

#include <cctype>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace skycued {

namespace {

bool isSpace(char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }

std::string trim(const std::string& s)
{
    size_t b = 0, e = s.size();
    while (b < e && isSpace(s[b]))
        ++b;
    while (e > b && isSpace(s[e - 1]))
        --e;
    return s.substr(b, e - b);
}

std::string upper(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

bool isHexEntry(const std::string& s)
{
    if (s.size() != 6)
        return false;
    for (char c : s)
        if (!std::isxdigit(static_cast<unsigned char>(c)))
            return false;
    return true;
}

bool isRegistrationEntry(const std::string& s)
{
    if (s.size() < 2 || s.size() > 10)
        return false;
    for (char c : s)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-')
            return false;
    return true;
}

// Removes a comment: '#' at the start of the line or after whitespace.
std::string stripComment(const std::string& line)
{
    for (size_t i = 0; i < line.size(); ++i)
        if (line[i] == '#' && (i == 0 || isSpace(line[i - 1])))
            return line.substr(0, i);
    return line;
}

} // namespace

IdList IdList::loadFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error(path + ": cannot open list file");
    std::ostringstream ss;
    ss << in.rdbuf();
    if (in.bad())
        throw std::runtime_error(path + ": error reading list file");
    return parse(ss.str(), path);
}

IdList IdList::parse(const std::string& text, const std::string& name)
{
    IdList list;
    std::istringstream in(text);
    std::string line;
    size_t lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        const std::string entry = trim(stripComment(line));
        if (entry.empty())
            continue;
        if (isHexEntry(entry)) {
            list.hex_.insert(static_cast<uint32_t>(std::stoul(entry, nullptr, 16)));
        } else if (isRegistrationEntry(entry)) {
            const std::string reg = upper(entry);
            list.registrations_.insert(reg);
            if (reg[0] == 'N') {
                if (auto icao = registration::icaoFromNNumber(reg))
                    list.n_number_icao_.insert(*icao);
            }
        } else {
            throw std::runtime_error(name + ":" + std::to_string(lineno) + ": bad entry '" + entry + "'");
        }
        ++list.entries_;
    }
    return list;
}

bool IdList::contains(skycue::TrackId id, const std::optional<std::string>& registration) const
{
    if (hex_.count(id.address))
        return true;
    if (id.type == skycue::AddressType::icao && n_number_icao_.count(id.address))
        return true;
    if (registration && !registrations_.empty() && registrations_.count(upper(trim(*registration))))
        return true;
    return false;
}

FilterSet::FilterSet(ReceiverPosition rx, FilterConfig config, IdList allow, IdList block)
    : rx_(rx), config_(config), allow_(std::move(allow)), block_(std::move(block))
{
}

void FilterSet::setLists(IdList allow, IdList block)
{
    allow_ = std::move(allow);
    block_ = std::move(block);
}

void FilterSet::setConfig(FilterConfig config) { config_ = config; }

FilterResult FilterSet::evaluate(const TrackState& t) const
{
    const TrackObservation& o = t.latest;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    FilterResult r{Decision::send, nan, nan, false};

    const bool has_position = o.lat && o.lon;
    if (has_position) {
        double tgt_alt_m = rx_.alt_m;  // on the ground, or no altitude reported
        if (!o.on_ground) {
            if (o.alt_geom_ft) {
                tgt_alt_m = geo::ftToM(*o.alt_geom_ft);
                r.alt_is_geom = true;
            } else if (o.alt_baro_ft) {
                tgt_alt_m = geo::ftToM(*o.alt_baro_ft);
            }
        }
        r.range_nm = geo::greatCircleNm(rx_.lat, rx_.lon, *o.lat, *o.lon);
        r.elevation_deg = geo::elevationDeg(rx_.lat, rx_.lon, rx_.alt_m, *o.lat, *o.lon, tgt_alt_m);
    }

    // 1. no position: never forwarded, whatever the lists say
    if (!has_position) {
        r.decision = Decision::no_position;
        return r;
    }
    // 2. block list
    if (block_.contains(t.id, o.registration)) {
        r.decision = Decision::blocked;
        return r;
    }
    // 3. allow list
    if (allow_.contains(t.id, o.registration)) {
        r.decision = Decision::allow_listed;
        return r;
    }
    // 4. range
    if (r.range_nm > config_.max_range_nm) {
        r.decision = Decision::out_of_range;
        return r;
    }
    // 5. elevation
    if (r.elevation_deg < config_.min_elevation_deg) {
        r.decision = Decision::below_elevation;
        return r;
    }
    // 6. send
    r.decision = Decision::send;
    return r;
}

} // namespace skycued
