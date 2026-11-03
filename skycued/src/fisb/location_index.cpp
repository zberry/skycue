#include "fisb/location_index.hpp"

#include "geo.hpp"
#include "json_lite.hpp"
#include "json_nesting.hpp"

#include <boost/property_tree/json_parser.hpp>

#include <charconv>
#include <cmath>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace skycued {
namespace fisb {

namespace {

// fisb-locations.json nests 3 levels; anything deeper is not ours.
constexpr int kMaxLearnedDepth = 8;

bool upperOrDigit(char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }

bool validPosition(double lat, double lon)
{
    return std::isfinite(lat) && std::isfinite(lon) && lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180;
}

// The whole of `text` as a finite number, locale-independent.
std::optional<double> parseNumber(const std::string& text)
{
    if (text.empty())
        return std::nullopt;
    std::istringstream in(text);
    in.imbue(std::locale::classic());
    double v = 0;
    in >> v;
    if (in.fail() || in.peek() != std::char_traits<char>::eof() || !std::isfinite(v))
        return std::nullopt;
    return v;
}

std::optional<uint32_t> parseCount(const std::string& text)
{
    uint32_t v = 0;
    const char* end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, v);
    if (text.empty() || result.ec != std::errc() || result.ptr != end)
        return std::nullopt;
    return v;
}

std::string fixed6(double v)
{
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out.setf(std::ios::fixed);
    out.precision(6);
    out << v;
    return out.str();
}

std::string trim(const std::string& s)
{
    const auto b = s.find_first_not_of(" \t\r");
    if (b == std::string::npos)
        return {};
    return s.substr(b, s.find_last_not_of(" \t\r") - b + 1);
}

// The vertex mean of a polygon (a closing vertex equal to the first one
// excluded), when every vertex is within kCentroidMaxRadiusNm of it.
std::optional<std::pair<double, double>> smallCentroid(const Shape& s)
{
    std::size_t n = s.points.size();
    if (n >= 2 && s.points.back() == s.points.front())
        --n;
    if (n == 0)
        return std::nullopt;
    double lat = 0, lon = 0;
    for (std::size_t i = 0; i < n; ++i) {
        lat += s.points[i].first;
        lon += s.points[i].second;
    }
    lat /= static_cast<double>(n);
    lon /= static_cast<double>(n);
    if (!validPosition(lat, lon))
        return std::nullopt;
    for (std::size_t i = 0; i < n; ++i)
        if (!(geo::greatCircleNm(lat, lon, s.points[i].first, s.points[i].second) <= kCentroidMaxRadiusNm))
            return std::nullopt;
    return std::make_pair(lat, lon);
}

}  // namespace

bool airportStyleId(const std::string& id)
{
    if (id.size() != 4)
        return false;
    for (char c : id)
        if (!upperOrDigit(c))
            return false;
    return id[0] == 'K' || id[0] == 'C' || (id[0] == 'P' && (id[1] == 'A' || id[1] == 'H'));
}

bool LocationIndex::learn(const TwgoRecord& record)
{
    if (!airportStyleId(record.location))
        return false;
    std::vector<std::pair<double, double>> samples;
    std::optional<std::pair<double, double>> centroid;
    for (const Shape& s : record.shapes) {
        if (s.points.empty())
            continue;
        if (s.geom == Shape::point || s.geom == Shape::circle || s.geom == Shape::prism) {
            if (validPosition(s.points[0].first, s.points[0].second))
                samples.push_back(s.points[0]);
        } else if (s.geom == Shape::polygon && !centroid) {
            centroid = smallCentroid(s);
        }
    }
    auto it = learned_.find(record.location);
    const bool is_new = it == learned_.end();
    if (samples.empty() && (!centroid || (!is_new && it->second.samples > 0)))
        return false;
    if (is_new)
        it = learned_.emplace(record.location, Learned{}).first;
    Learned& e = it->second;
    if (samples.empty()) {
        if (is_new || e.lat != centroid->first || e.lon != centroid->second) {
            e.lat = centroid->first;
            e.lon = centroid->second;
            dirty_ = true;
        }
        return is_new;
    }
    for (const auto& p : samples) {
        // A centroid entry has 0 samples, so its first point replaces it.
        const uint32_t n = e.samples < kMaxSamples ? e.samples + 1 : kMaxSamples;
        e.lat += (p.first - e.lat) / n;
        e.lon += (p.second - e.lon) / n;
        e.samples = n;
    }
    dirty_ = true;
    return is_new;
}

std::string LocationIndex::learnedJson() const
{
    JsonWriter locations;
    for (const auto& [id, e] : learned_)
        locations.addRaw(id.c_str(), "[" + fixed6(e.lat) + "," + fixed6(e.lon) + "," + std::to_string(e.samples) + "]");
    JsonWriter doc;
    doc.add("version", int64_t{1});
    doc.addRaw("locations", locations.str());
    return doc.str();
}

void LocationIndex::loadLearned(const std::string& text)
{
    if (text.empty())
        throw std::runtime_error("empty file");
    if (!nestingWithin(text, kMaxLearnedDepth))
        throw std::runtime_error("nested too deeply");
    boost::property_tree::ptree tree;
    try {
        std::istringstream in(text);
        boost::property_tree::read_json(in, tree);
    } catch (const boost::property_tree::json_parser_error& e) {
        throw std::runtime_error("not JSON: " + e.message() + " at line " + std::to_string(e.line()));
    }
    if (!tree.data().empty())
        throw std::runtime_error("not a JSON object");
    const auto version = tree.get_child_optional("version");
    if (!version || !version->empty())
        throw std::runtime_error("no \"version\"");
    if (version->data() != "1")
        throw std::runtime_error("unsupported version " + version->data());
    const auto locations = tree.get_child_optional("locations");
    if (!locations || !locations->data().empty())
        throw std::runtime_error("no \"locations\" object");
    std::map<std::string, Learned> loaded;
    for (const auto& [id, value] : *locations) {
        if (!airportStyleId(id))
            throw std::runtime_error("\"locations\": bad id \"" + id + "\"");
        std::vector<std::string> fields;
        for (const auto& [key, element] : value) {
            if (!key.empty() || !element.empty())
                break;
            fields.push_back(element.data());
        }
        if (!value.data().empty() || fields.size() != 3 || value.size() != 3)
            throw std::runtime_error(id + ": not [lat,lon,count]");
        const auto lat = parseNumber(fields[0]);
        const auto lon = parseNumber(fields[1]);
        const auto count = parseCount(fields[2]);
        if (!lat || !lon || !validPosition(*lat, *lon))
            throw std::runtime_error(id + ": bad position");
        if (!count || *count > kMaxSamples)
            throw std::runtime_error(id + ": bad sample count");
        loaded[id] = Learned{*lat, *lon, *count};
    }
    learned_ = std::move(loaded);
    dirty_ = false;
}

std::size_t LocationIndex::loadOperatorCsv(const std::string& text)
{
    operator_.clear();
    rejected_ = 0;
    rejected_first_line_ = 0;
    std::istringstream in(text);
    std::string raw;
    for (std::size_t line_no = 1; std::getline(in, raw); ++line_no) {
        const std::string line = trim(raw);
        if (line.empty() || line[0] == '#' || line == "id,lat,lon")
            continue;
        std::vector<std::string> fields;
        std::istringstream cells(line);
        for (std::string cell; std::getline(cells, cell, ',');)
            fields.push_back(trim(cell));
        bool ok = fields.size() == 3 && line.back() != ',';
        std::string id = ok ? fields[0] : std::string();
        for (char& c : id)
            if (c >= 'a' && c <= 'z')
                c = static_cast<char>(c - 'a' + 'A');
        ok = ok && id.size() >= 2 && id.size() <= 5;
        for (char c : id)
            ok = ok && upperOrDigit(c);
        const auto lat = ok ? parseNumber(fields[1]) : std::nullopt;
        const auto lon = ok ? parseNumber(fields[2]) : std::nullopt;
        if (!lat || !lon || !validPosition(*lat, *lon)) {
            if (++rejected_ == 1)
                rejected_first_line_ = line_no;
            continue;
        }
        operator_[id] = {*lat, *lon};
    }
    return operator_.size();
}

std::size_t LocationIndex::operatorRejected() const { return rejected_; }

std::size_t LocationIndex::operatorRejectedFirstLine() const { return rejected_first_line_; }

std::optional<std::pair<double, double>> LocationIndex::find(const std::string& id) const
{
    if (const auto it = learned_.find(id); it != learned_.end())
        return std::make_pair(it->second.lat, it->second.lon);
    if (const auto it = operator_.find(id); it != operator_.end())
        return it->second;
    return std::nullopt;
}

std::map<std::string, std::pair<double, double>> LocationIndex::merged() const
{
    auto out = operator_;
    for (const auto& [id, e] : learned_)
        out[id] = {e.lat, e.lon};
    return out;
}

std::size_t LocationIndex::learnedCount() const { return learned_.size(); }

bool LocationIndex::dirty() const { return dirty_; }

void LocationIndex::markSaved() { dirty_ = false; }

}  // namespace fisb
}  // namespace skycued
