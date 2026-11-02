#pragma once

// JSON for FIS-B items, built with JsonWriter. The field names here are the
// one definition shared by the fisb recording stream (docs/recording-format.md)
// and the web API (docs/web-api.md "FIS-B records (draft)"). Times are
// ISO 8601 UTC with milliseconds; coordinates are degrees with up to 6
// decimals; absent optionals are omitted.

#include "fisb/fisb_store.hpp"
#include "fisb/products.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace skycued {
namespace fisb {

// "metar" | "taf" | "pirep" | "winds" | "other".
const char* textKindName(TextRecord::Kind kind);
// "polygon" | "polyline" | "point" | "circle" | "prism".
const char* shapeGeomName(Shape::Geom geom);

// [[value,count],...] covering the 128 bins in order.
std::string rleJson(const std::array<uint8_t, 128>& bins);

// {"key","kind","station","issued","expires","period_h"?,"text"}
std::string textRecordJson(const TextRecord& r);
// {"geom","points":[[lat,lon],...],"radius_nm"?,"alt_low_ft"?,"alt_high_ft"?,"agl"}
std::string shapeJson(const Shape& s);
// {"key","product","name","report","location","label"?,"text"?,"has_graphics","complete",
//  "cancelled","start"?,"end"?,"received","last_received","expires","shapes":[...]}
std::string twgoRecordJson(const TwgoRecord& r);
// {"key","product","name","altitude_code"?,"product_time","updated","published"?,"expires",
//  "bins_per_block":128,"block_count","evicted",
//  "blocks":[{"block","lat0","lon0","dlat","dlon","rle":[[v,n],...]},...]}
std::string mosaicJson(const Mosaic& m);
// The same without "blocks" (what a radar/raster notification carries).
std::string mosaicHeadJson(const Mosaic& m);
// {"enabled","text_records","twgo_records","mosaics","mosaic_blocks","stations","radar_age_s"?}
std::string summaryJson(const FisbSummary& s);

// One line of the fisb recording stream (no trailing newline):
// {"time":<time>,"type":"fisb_text"|"fisb_twgo"|"fisb_radar"|"fisb_raster"|"fisb_expired", ...}
// followed by the item's fields (textRecordJson / twgoRecordJson / mosaicJson
// with every block), or for fisb_expired {"key","product","reason","expires"}.
std::string eventRecordJson(const FisbEvent& e, TimePoint time);

}  // namespace fisb
}  // namespace skycued
