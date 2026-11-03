#pragma once

// ASTERIX category 021 (ADS-B target reports) decoder, editions 2.2-2.7
// (docs/asterix-cat021-reference.md §1-§5). Pure functions, no I/O. All six
// editions use the same length rules (the reference's "2.2+" family): newer
// FX extension octets are kept as received in `more`.

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace skycued {
namespace asterix {

enum class Edition : uint8_t { e2_2, e2_3, e2_4, e2_5, e2_6, e2_7 };

// "2.2" .. "2.7" exactly; anything else is nullopt.
std::optional<Edition> parseEdition(const std::string& s);
const char* editionName(Edition e);

// LSBs of the raw values below.
constexpr double kLsb130Deg = 180.0 / (1 << 23);
constexpr double kLsb131Deg = 180.0 / (1u << 30);
constexpr double kTodLsbS = 1.0 / 128;
constexpr int kFt145 = 25;
constexpr double kFt140 = 6.25;
constexpr double kFpm155 = 6.25;  // 155 and 157
constexpr double kKtPerGs160 = 3600.0 / 16384;
constexpr double kDegPerTa160 = 360.0 / 65536;
constexpr double kKtPerIas150 = 3600.0 / 16384;
constexpr double kMachPer150 = 0.001;

// I021/040 Target Report Descriptor (§3.2).
struct Descriptor040 {
  uint8_t atp = 0, arc = 0;
  bool rc = false, rab = false;
  bool has_ext1 = false;
  bool dcr = false, gbs = false, sim = false, tst = false, saa = false;
  uint8_t cl = 0;
  bool has_ext2 = false;
  bool llc = false, ipc = false, nogo = false, cpr = false, ldpj = false, rcf = false;
  std::vector<uint8_t> more;  // extension octets past ext 2, as received
};

// I021/090 Quality Indicators (§3.17).
struct Quality090 {
  uint8_t nucr_nacv = 0, nucp_nic = 0;
  bool has_ext1 = false;
  bool nic_baro = false;
  uint8_t sil = 0, nacp = 0;
  bool has_ext2 = false;
  bool sils = false;
  uint8_t sda = 0, gva = 0;
  std::vector<uint8_t> more;  // extension octets past ext 2, as received
};

// I021/210 MOPS Version (§3.18).
struct Mops210 {
  bool vns = false;
  uint8_t vn = 0, ltt = 0;
};

// I021/200 Target Status (§3.23).
struct Status200 {
  bool icf = false, lnav = false, me = false;
  uint8_t ps = 0, ss = 0;
};

// I021/155 and I021/157: b16 RE, b15-1 s15 (× kFpm155 ft/min).
struct Rate {
  bool re = false;
  int16_t raw = 0;
};

// I021/160 Airborne Ground Vector (§3.26).
struct Vector160 {
  bool re = false;
  uint16_t gs_raw = 0, ta_raw = 0;
};

// I021/074 and I021/076: FSI and fractional second (LSB 2^-30 s).
struct Time074 {
  uint8_t fsi = 0;
  uint32_t frac = 0;
};

// RE/SGV Surface Ground Vector (§3.43): GSS LSB 1/8 kt, HGT LSB 45/16°.
struct SurfaceVector {
  bool stp = false, hts = false, htt = false, hrd = false;
  uint16_t gss_raw = 0;
  std::optional<uint8_t> hgt_raw;
};

// I021/150 Air Speed (§3.9): IM 0 = IAS, IM 1 = Mach.
struct AirSpeed150 {
  bool im = false;
  uint16_t raw = 0;
  std::optional<double> iasKt() const {
    return im ? std::nullopt : std::optional<double>(raw * kKtPerIas150);
  }
  std::optional<double> mach() const {
    return im ? std::optional<double>(raw * kMachPer150) : std::nullopt;
  }
};

// I021/151 True Airspeed (§3.10): RE 1 = above range, kt holds the maximum.
struct TrueAirspeed151 {
  bool re = false;
  uint16_t kt = 0;
};

struct Cat021Record {
  std::optional<uint8_t> sac, sic;                              // 010
  std::optional<Descriptor040> trd;                             // 040
  std::optional<uint32_t> tod071, tod072, tod073, tod075, tod077;  // raw 1/128 s
  std::optional<Time074> hp074, hp076;
  std::optional<int32_t> lat130, lon130;  // s24, kLsb130Deg
  std::optional<int32_t> lat131, lon131;  // s32, kLsb131Deg
  std::optional<AirSpeed150> air_speed;   // 150
  std::optional<TrueAirspeed151> tas_kt;  // 151
  std::optional<uint32_t> address;        // 080, 24 bits
  std::optional<int16_t> height140;       // × kFt140
  std::optional<Quality090> quality;      // 090
  std::optional<Mops210> mops;            // 210
  std::optional<uint16_t> mode3a;         // 070, 12 bits (4 octal digits)
  std::optional<int16_t> fl145;           // × kFt145 ft
  std::optional<uint16_t> mag152;         // 360/2^16 °
  std::optional<Status200> status;        // 200
  std::optional<Rate> bvr155, gvr157;
  std::optional<Vector160> vector160;
  std::optional<std::string> ident;  // 170, 8 chars, invalid codes as '?'
  std::optional<uint8_t> ecat;       // 020
  std::optional<uint8_t> aos008;     // 008, raw
  std::optional<int8_t> amplitude132;  // dBm
  std::optional<SurfaceVector> sgv;    // from RE (FRN 48)
  // FRN -> the item's bytes, for every present item not modelled above, plus
  // RE (48) whole even when sgv is decoded.
  std::map<int, std::vector<uint8_t>> opaque;
  // The whole record as received (decoder only; the encoder ignores it).
  std::vector<uint8_t> bytes;
};

// One CAT/LEN data block inside a buffer.
struct BlockSpan {
  std::size_t offset, size;
  uint8_t cat;
};
struct BlockSplit {
  std::vector<BlockSpan> blocks;
  bool truncated = false;  // LEN < 3, LEN past the end, or trailing bytes
};
// Frames a UDP datagram (reference §6). Never decodes.
BlockSplit splitBlocks(const uint8_t* data, std::size_t size);

struct DecodedBlock {
  std::vector<Cat021Record> records;
  bool error = false;
  std::string error_text;
};
// `block` is one whole CAT 21 block whose LEN equals `size`. On a length
// inconsistency the records decoded so far are kept, `error` is set and the
// rest of the block is discarded (reference §1).
DecodedBlock decodeBlock(const uint8_t* block, std::size_t size, Edition edition);

namespace detail {

// The length walk shared by decodeBlock, the encoder's opaque check and the
// stream framer, so they cannot disagree on what a valid block is.
enum class WalkStatus : uint8_t {
  ok,
  error,      // length inconsistency (bad FSPEC, spare FRN, unmeasurable item)
  need_more,  // ran out of bytes before the length was known
};

struct ItemSpan {
  int frn;
  std::size_t offset, size;  // relative to the record start
};
struct RecordLayout {
  std::size_t size = 0;
  std::size_t fspec_size = 0;
  std::vector<ItemSpan> items;  // in FRN order
  std::string error_text;
};

// Measures the item `frn` (1..49) starting at p.
WalkStatus measureItem(int frn, const uint8_t* p, std::size_t avail, std::size_t& len,
                       std::string* error_text = nullptr);

// Walks one record (FSPEC and items) starting at p.
WalkStatus walkRecord(const uint8_t* p, std::size_t avail, RecordLayout& out);

// Walks the records of a block body (the bytes after CAT/LEN). With
// `complete`, the records must end exactly at n and need_more is reported as
// an error. Without it, the body is a prefix of a block still arriving:
// need_more means no error so far. A body with no record is an error. When
// `layouts` is given, it receives the layout of every record walked cleanly
// before the result (decodeBlock decodes exactly those). When `walked` is
// given, it receives the number of bytes of those records (where a later walk
// of the same prefix can resume).
WalkStatus walkRecords(const uint8_t* p, std::size_t n, bool complete, Edition edition,
                       std::vector<RecordLayout>* layouts = nullptr,
                       std::string* error_text = nullptr, std::size_t* walked = nullptr);

}  // namespace detail

}  // namespace asterix
}  // namespace skycued
