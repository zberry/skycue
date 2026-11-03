#pragma once

// Positions for the location ids FIS-B text products name (design §3.2):
// learned from TWGO graphics (a NOTAM-D/TFR point, or a small polygon's
// centroid), persisted as fisb-locations.json, and merged over an optional
// operator CSV. Pure data: the caller reads and writes the files.

#include "fisb/fisb_store.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>

namespace skycued {
namespace fisb {

// 4 characters [A-Z0-9]: K???, C???, PA??, PH?? (spec §3.2 "airport-style").
bool airportStyleId(const std::string& id);
constexpr double kCentroidMaxRadiusNm = 10;   // a larger polygon's centroid is not an airport position
constexpr uint32_t kMaxSamples = 1000;        // the running mean stops weighting new samples past this
// Memory bounds: new ids past these are counted and ignored (a WARN once per index).
constexpr std::size_t kMaxLearnedIds = 20000;
constexpr std::size_t kMaxOperatorRows = 50000;

class LocationIndex {
public:
    // Feeds one TWGO record (airport-style location only). Point shapes, and circle/prism centres (points[0]),
    // are point samples, averaged. A polygon's centroid (vertex mean, a closing vertex equal to the first one
    // excluded) is used only while the id has no point sample and only when every vertex is within
    // kCentroidMaxRadiusNm of it; polylines are ignored. Returns true when the id is new.
    // Longitudes are averaged as wrapped deltas, so samples either side of +-180 average near it.
    bool learn(const TwgoRecord& record);
    std::size_t learnedOverflow() const;  // new ids ignored past kMaxLearnedIds since the last loadLearned()
    // fisb-locations.json: {"version":1,"locations":{"KPRG":[39.699097,-87.670212,3]}} (6 decimals, ids sorted).
    // The third value is the point-sample count; 0 marks a polygon centroid.
    std::string learnedJson() const;
    // Replaces the learned set; throws std::runtime_error naming the problem (more than kMaxLearnedIds ids too),
    // leaving the index unchanged. PropertyTree keeps every scalar as text, so quoted numerals are accepted.
    void loadLearned(const std::string& text);
    // Operator CSV "id,lat,lon": '#' comments, blank lines and an exact "id,lat,lon" header skipped; id upper-cased,
    // [A-Z0-9]{2,5}; lat -90..90, lon -180..180; any other line skipped and counted. Returns entries loaded.
    // Replaces the operator set. A leading UTF-8 BOM and CRLF line ends are accepted.
    std::size_t loadOperatorCsv(const std::string& text);
    std::size_t operatorRejected() const;
    std::size_t operatorRejectedFirstLine() const;  // 1-based; 0 when none rejected
    std::size_t operatorOverflow() const;           // new ids ignored past kMaxOperatorRows
    std::optional<std::pair<double, double>> find(const std::string& id) const;   // learned wins over operator
    std::map<std::string, std::pair<double, double>> merged() const;
    std::size_t learnedCount() const;
    bool dirty() const;      // learned set changed since markSaved()/loadLearned()
    void markSaved();

private:
    struct Learned {
        double lat = 0, lon = 0;
        uint32_t samples = 0;  // point samples in the mean (capped at kMaxSamples); 0: a polygon centroid
    };
    static std::string rendering(const Learned& e);  // the entry's text in learnedJson()

    std::map<std::string, Learned> learned_;
    std::map<std::string, std::pair<double, double>> operator_;
    std::size_t rejected_ = 0, rejected_first_line_ = 0, operator_overflow_ = 0;
    std::size_t learned_overflow_ = 0;
    bool dirty_ = false;
};

// Which positions the fisb_location items have announced. A learned position is announced again only once it is
// kLocationMoveDeg or more (in latitude, or in wrapped longitude) from the last one announced, so a mean that
// drifts in many small steps is still announced once the drift adds up.
constexpr double kLocationMoveDeg = 1e-4;

class LocationAnnouncer {
public:
    // After LocationIndex::learn: `before`/`after` are find(id) around it, `is_new` its result. True when `after`
    // is to be announced (an id never announced counts as announced at `before`, which clients GET); records it.
    bool update(const std::string& id, const std::optional<std::pair<double, double>>& before,
                const std::pair<double, double>& after, bool is_new);

private:
    std::map<std::string, std::pair<double, double>> sent_;  // bounded by the learned ids
};

}  // namespace fisb
}  // namespace skycued
