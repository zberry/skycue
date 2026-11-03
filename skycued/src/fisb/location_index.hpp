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

class LocationIndex {
public:
    // Feeds one TWGO record (airport-style location only). Point shapes, and circle/prism centres (points[0]),
    // are point samples, averaged. A polygon's centroid (vertex mean, a closing vertex equal to the first one
    // excluded) is used only while the id has no point sample and only when every vertex is within
    // kCentroidMaxRadiusNm of it; polylines are ignored. Returns true when the id is new.
    bool learn(const TwgoRecord& record);
    // fisb-locations.json: {"version":1,"locations":{"KPRG":[39.699097,-87.670212,3]}} (6 decimals, ids sorted).
    // The third value is the point-sample count; 0 marks a polygon centroid.
    std::string learnedJson() const;
    void loadLearned(const std::string& text);  // replaces the learned set; throws std::runtime_error naming the problem
    // Operator CSV "id,lat,lon": '#' comments, blank lines and an exact "id,lat,lon" header skipped; id upper-cased,
    // [A-Z0-9]{2,5}; lat -90..90, lon -180..180; any other line skipped and counted. Returns entries loaded.
    // Replaces the operator set.
    std::size_t loadOperatorCsv(const std::string& text);
    std::size_t operatorRejected() const;
    std::size_t operatorRejectedFirstLine() const;  // 1-based; 0 when none rejected
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
    std::map<std::string, Learned> learned_;
    std::map<std::string, std::pair<double, double>> operator_;
    std::size_t rejected_ = 0, rejected_first_line_ = 0;
    bool dirty_ = false;
};

}  // namespace fisb
}  // namespace skycued
