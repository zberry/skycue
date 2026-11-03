#include "asterix/cat021.hpp"

#include <string>
#include <utility>

namespace skycued {
namespace asterix {

std::optional<Edition> parseEdition(const std::string& s) {
  static const std::pair<const char*, Edition> kNames[] = {
      {"2.2", Edition::e2_2}, {"2.3", Edition::e2_3}, {"2.4", Edition::e2_4},
      {"2.5", Edition::e2_5}, {"2.6", Edition::e2_6}, {"2.7", Edition::e2_7}};
  for (const auto& n : kNames) {
    if (s == n.first) return n.second;
  }
  return std::nullopt;
}

const char* editionName(Edition e) {
  switch (e) {
    case Edition::e2_2: return "2.2";
    case Edition::e2_3: return "2.3";
    case Edition::e2_4: return "2.4";
    case Edition::e2_5: return "2.5";
    case Edition::e2_6: return "2.6";
    case Edition::e2_7: return "2.7";
  }
  return "?";
}

namespace detail {

namespace {

// The ed. 2.2+ UAP length rules (reference §2, §3.31, §3.34, §3.42).
enum class Kind : uint8_t { fixed, extended, repetitive, compound, explicit_len, spare };

// fixed: len octets. repetitive: REP octet + REP x len. compound: len selects
// the subfield table in kCompounds.
struct Rule {
  Kind kind;
  uint8_t len;
};

constexpr Rule fx(uint8_t n) { return Rule{Kind::fixed, n}; }
constexpr Rule compound(uint8_t id) { return Rule{Kind::compound, id}; }
constexpr Rule kExt{Kind::extended, 0};
constexpr Rule kExplicit{Kind::explicit_len, 0};
constexpr Rule kSpare{Kind::spare, 0};

struct Compound {
  const Rule* subs;
  std::size_t n;
};

// 220 Met Information: WS, WD, TMP, TRB.
constexpr Rule kSub220[] = {fx(2), fx(2), fx(2), fx(1)};
// 110 Trajectory Intent: TIS (extended), TID (REP x 15).
constexpr Rule kSub110[] = {kExt, Rule{Kind::repetitive, 15}};
// 295 Data Ages: 23 one-octet subfields.
constexpr Rule kSub295[] = {fx(1), fx(1), fx(1), fx(1), fx(1), fx(1), fx(1), fx(1),
                            fx(1), fx(1), fx(1), fx(1), fx(1), fx(1), fx(1), fx(1),
                            fx(1), fx(1), fx(1), fx(1), fx(1), fx(1), fx(1)};
constexpr Compound kCompounds[] = {{kSub220, 4}, {kSub110, 2}, {kSub295, 23}};

constexpr int kMaxFrn = 49;
constexpr std::size_t kMaxFspec = 7;

// Indexed by FRN (0 unused).
constexpr Rule kItemRules[kMaxFrn + 1] = {
    kSpare,
    fx(2),        // 1  010
    kExt,         // 2  040
    fx(2),        // 3  161
    fx(1),        // 4  015
    fx(3),        // 5  071
    fx(6),        // 6  130
    fx(8),        // 7  131
    fx(3),        // 8  072
    fx(2),        // 9  150
    fx(2),        // 10 151
    fx(3),        // 11 080
    fx(3),        // 12 073
    fx(4),        // 13 074
    fx(3),        // 14 075
    fx(4),        // 15 076
    fx(2),        // 16 140
    kExt,         // 17 090
    fx(1),        // 18 210
    fx(2),        // 19 070
    fx(2),        // 20 230
    fx(2),        // 21 145
    fx(2),        // 22 152
    fx(1),        // 23 200
    fx(2),        // 24 155
    fx(2),        // 25 157
    fx(4),        // 26 160
    fx(2),        // 27 165
    fx(3),        // 28 077
    fx(6),        // 29 170
    fx(1),        // 30 020
    compound(0),  // 31 220
    fx(2),        // 32 146
    fx(2),        // 33 148
    compound(1),  // 34 110
    fx(1),        // 35 016
    fx(1),        // 36 008
    kExt,         // 37 271 (2.2+ form: FX-extended)
    fx(1),        // 38 132
    Rule{Kind::repetitive, 8},  // 39 250
    fx(7),        // 40 260
    fx(1),        // 41 400
    compound(2),  // 42 295
    kSpare,       // 43
    kSpare,       // 44
    kSpare,       // 45
    kSpare,       // 46
    kSpare,       // 47
    kExplicit,    // 48 RE
    kExplicit,    // 49 SP
};

using WS = WalkStatus;

WS fail(std::string* error_text, const std::string& what) {
  if (error_text) *error_text = what;
  return WS::error;
}

// Length of an FX chain (extended item or compound primary subfield).
WS measureFx(const uint8_t* p, std::size_t avail, std::size_t& len) {
  for (std::size_t i = 0; i < avail; ++i) {
    if ((p[i] & 0x01) == 0) {
      len = i + 1;
      return WS::ok;
    }
  }
  return WS::need_more;
}

WS measureRule(const Rule& r, const uint8_t* p, std::size_t avail, std::size_t& len,
               std::string* error_text);

WS measureCompound(const Compound& c, const uint8_t* p, std::size_t avail, std::size_t& len,
                   std::string* error_text) {
  std::size_t primary = 0;
  const WS st = measureFx(p, avail, primary);
  if (st != WS::ok) return st;
  std::size_t off = primary;
  for (std::size_t k = 0; k < primary; ++k) {
    for (std::size_t j = 0; j < 7; ++j) {
      if ((p[k] & (0x80u >> j)) == 0) continue;
      const std::size_t idx = 7 * k + j;
      if (idx >= c.n || c.subs[idx].kind == Kind::spare) {
        return fail(error_text, "compound subfield " + std::to_string(idx + 1) + " unknown");
      }
      std::size_t sub = 0;
      const WS s = measureRule(c.subs[idx], p + off, avail - off, sub, error_text);
      if (s != WS::ok) return s;
      off += sub;
    }
  }
  len = off;
  return WS::ok;
}

WS measureRule(const Rule& r, const uint8_t* p, std::size_t avail, std::size_t& len,
               std::string* error_text) {
  switch (r.kind) {
    case Kind::fixed:
      if (avail < r.len) return WS::need_more;
      len = r.len;
      return WS::ok;
    case Kind::extended:
      return measureFx(p, avail, len);
    case Kind::repetitive: {
      if (avail < 1) return WS::need_more;
      const std::size_t total = 1 + std::size_t{p[0]} * r.len;
      if (avail < total) return WS::need_more;
      len = total;
      return WS::ok;
    }
    case Kind::explicit_len:
      if (avail < 1) return WS::need_more;
      if (p[0] == 0) return fail(error_text, "explicit item with LEN 0");
      if (avail < p[0]) return WS::need_more;
      len = p[0];
      return WS::ok;
    case Kind::compound:
      return measureCompound(kCompounds[r.len], p, avail, len, error_text);
    case Kind::spare:
      break;
  }
  return fail(error_text, "spare item");
}

}  // namespace

WalkStatus measureItem(int frn, const uint8_t* p, std::size_t avail, std::size_t& len,
                       std::string* error_text) {
  if (frn < 1 || frn > kMaxFrn) {
    return fail(error_text, "FRN " + std::to_string(frn) + " out of range");
  }
  std::string why;
  const WS st = measureRule(kItemRules[frn], p, avail, len, &why);
  if (st == WS::error) return fail(error_text, "FRN " + std::to_string(frn) + ": " + why);
  return st;
}

WalkStatus walkRecord(const uint8_t* p, std::size_t avail, RecordLayout& out) {
  out = RecordLayout{};
  std::size_t fs = 0;
  for (;;) {
    if (fs == kMaxFspec) return fail(&out.error_text, "FSPEC longer than 7 octets");
    if (fs >= avail) return WS::need_more;
    if ((p[fs++] & 0x01) == 0) break;
  }
  out.fspec_size = fs;
  std::size_t off = fs;
  for (std::size_t k = 0; k < fs; ++k) {
    for (int j = 0; j < 7; ++j) {
      if ((p[k] & (0x80u >> j)) == 0) continue;
      const int frn = static_cast<int>(7 * k) + j + 1;
      std::size_t len = 0;
      const WS st = measureItem(frn, p + off, avail - off, len, &out.error_text);
      if (st != WS::ok) return st;
      out.items.push_back(ItemSpan{frn, off, len});
      off += len;
    }
  }
  if (out.items.empty()) return fail(&out.error_text, "FSPEC has no item");
  out.size = off;
  return WS::ok;
}

WalkStatus walkRecords(const uint8_t* p, std::size_t n, bool complete, Edition edition,
                       std::vector<RecordLayout>* layouts, std::string* error_text) {
  (void)edition;  // every supported edition shares the 2.2+ length rules
  std::size_t off = 0;
  while (off < n) {
    RecordLayout layout;
    const WS st = walkRecord(p + off, n - off, layout);
    if (st == WS::error) {
      return fail(error_text,
                  "record at body offset " + std::to_string(off) + ": " + layout.error_text);
    }
    if (st == WS::need_more) {
      if (!complete) return WS::need_more;
      return fail(error_text, "record at body offset " + std::to_string(off) + " runs past LEN");
    }
    off += layout.size;
    if (layouts) layouts->push_back(std::move(layout));
  }
  if (!complete) return WS::need_more;
  if (n == 0) return fail(error_text, "block has no record");
  return WS::ok;
}

}  // namespace detail

BlockSplit splitBlocks(const uint8_t* data, std::size_t size) {
  BlockSplit out;
  std::size_t off = 0;
  while (off + 3 <= size) {
    const std::size_t len = (std::size_t{data[off + 1]} << 8) | data[off + 2];
    if (len < 3 || off + len > size) {
      out.truncated = true;
      return out;
    }
    out.blocks.push_back(BlockSpan{off, len, data[off]});
    off += len;
  }
  if (off != size) out.truncated = true;
  return out;
}

namespace {

uint32_t be(const uint8_t* p, std::size_t n) {
  uint32_t v = 0;
  for (std::size_t i = 0; i < n; ++i) v = (v << 8) | p[i];
  return v;
}

bool bit(uint8_t octet, int b) { return ((octet >> (b - 1)) & 1u) != 0; }

uint8_t field(uint8_t octet, int hi, int lo) {
  return static_cast<uint8_t>((octet >> (lo - 1)) & ((1u << (hi - lo + 1)) - 1));
}

int32_t signExtend(uint32_t v, int bits) {
  const uint32_t sign = 1u << (bits - 1);
  return static_cast<int32_t>(v ^ sign) - static_cast<int32_t>(sign);
}

Rate decodeRate(const uint8_t* p) {
  const uint32_t v = be(p, 2);
  return Rate{(v & 0x8000u) != 0, static_cast<int16_t>(signExtend(v & 0x7FFFu, 15))};
}

Time074 decodeTime(const uint8_t* p) {
  const uint32_t v = be(p, 4);
  return Time074{static_cast<uint8_t>(v >> 30), v & 0x3FFFFFFFu};
}

Descriptor040 decode040(const uint8_t* p, std::size_t n) {
  Descriptor040 d;
  d.atp = field(p[0], 8, 6);
  d.arc = field(p[0], 5, 4);
  d.rc = bit(p[0], 3);
  d.rab = bit(p[0], 2);
  if (n >= 2) {
    d.has_ext1 = true;
    d.dcr = bit(p[1], 8);
    d.gbs = bit(p[1], 7);
    d.sim = bit(p[1], 6);
    d.tst = bit(p[1], 5);
    d.saa = bit(p[1], 4);
    d.cl = field(p[1], 3, 2);
  }
  if (n >= 3) {
    d.has_ext2 = true;
    d.llc = bit(p[2], 7);
    d.ipc = bit(p[2], 6);
    d.nogo = bit(p[2], 5);
    d.cpr = bit(p[2], 4);
    d.ldpj = bit(p[2], 3);
    d.rcf = bit(p[2], 2);
  }
  if (n > 3) d.more.assign(p + 3, p + n);
  return d;
}

Quality090 decode090(const uint8_t* p, std::size_t n) {
  Quality090 q;
  q.nucr_nacv = field(p[0], 8, 6);
  q.nucp_nic = field(p[0], 5, 2);
  if (n >= 2) {
    q.has_ext1 = true;
    q.nic_baro = bit(p[1], 8);
    q.sil = field(p[1], 7, 6);
    q.nacp = field(p[1], 5, 2);
  }
  if (n >= 3) {
    q.has_ext2 = true;
    q.sils = bit(p[2], 6);
    q.sda = field(p[2], 5, 4);
    q.gva = field(p[2], 3, 2);
  }
  if (n > 3) q.more.assign(p + 3, p + n);
  return q;
}

// ICAO 6-bit characters (reference §3.29).
char identChar(uint32_t code) {
  if (code >= 1 && code <= 26) return static_cast<char>('A' + (code - 1));
  if (code == 32) return ' ';
  if (code >= 48 && code <= 57) return static_cast<char>('0' + (code - 48));
  return '?';
}

std::string decode170(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 6; ++i) v = (v << 8) | p[i];
  std::string s(8, ' ');
  for (int i = 0; i < 8; ++i) {
    s[static_cast<std::size_t>(i)] =
        identChar(static_cast<uint32_t>((v >> (42 - 6 * i)) & 0x3Fu));
  }
  return s;
}

// RE (§3.43): LEN, then a compound whose primary subfields are BPS 2, SH 2,
// NAV 1, GAO 1, SGV (extended), ... Only SGV is decoded. Anything that does not
// fit stops RE decoding without error: RE is skipped by its LEN anyway.
std::optional<SurfaceVector> decodeReSgv(const uint8_t* p, std::size_t n) {
  if (n < 2) return std::nullopt;
  const uint8_t* q = p + 1;
  const std::size_t avail = n - 1;
  std::size_t primary = 0;
  while (primary < avail && (q[primary] & 0x01) != 0) ++primary;
  if (primary == avail) return std::nullopt;
  ++primary;
  if (!bit(q[0], 4)) return std::nullopt;
  std::size_t off = primary;
  static const std::size_t kBefore[] = {2, 2, 1, 1};  // BPS SH NAV GAO
  for (int i = 0; i < 4; ++i) {
    if (bit(q[0], 8 - i)) off += kBefore[i];
  }
  if (off + 2 > avail) return std::nullopt;
  const uint32_t v = be(q + off, 2);
  SurfaceVector s;
  s.stp = (v & 0x8000u) != 0;
  s.hts = (v & 0x4000u) != 0;
  s.htt = (v & 0x2000u) != 0;
  s.hrd = (v & 0x1000u) != 0;
  s.gss_raw = static_cast<uint16_t>((v >> 1) & 0x7FFu);
  if ((v & 0x01u) != 0) {
    if (off + 3 > avail) return std::nullopt;
    s.hgt_raw = static_cast<uint8_t>(q[off + 2] >> 1);
  }
  return s;
}

Cat021Record decodeRecord(const uint8_t* rec, const detail::RecordLayout& layout) {
  Cat021Record r;
  r.bytes.assign(rec, rec + layout.size);
  for (const detail::ItemSpan& it : layout.items) {
    const uint8_t* p = rec + it.offset;
    const std::size_t n = it.size;
    switch (it.frn) {
      case 1:
        r.sac = p[0];
        r.sic = p[1];
        break;
      case 2: r.trd = decode040(p, n); break;
      case 5: r.tod071 = be(p, 3); break;
      case 6:
        r.lat130 = signExtend(be(p, 3), 24);
        r.lon130 = signExtend(be(p + 3, 3), 24);
        break;
      case 7:
        r.lat131 = static_cast<int32_t>(be(p, 4));
        r.lon131 = static_cast<int32_t>(be(p + 4, 4));
        break;
      case 8: r.tod072 = be(p, 3); break;
      case 9: {
        const uint32_t v = be(p, 2);
        r.air_speed = AirSpeed150{(v & 0x8000u) != 0, static_cast<uint16_t>(v & 0x7FFFu)};
        break;
      }
      case 10: {
        const uint32_t v = be(p, 2);
        r.tas_kt = TrueAirspeed151{(v & 0x8000u) != 0, static_cast<uint16_t>(v & 0x7FFFu)};
        break;
      }
      case 11: r.address = be(p, 3); break;
      case 12: r.tod073 = be(p, 3); break;
      case 13: r.hp074 = decodeTime(p); break;
      case 14: r.tod075 = be(p, 3); break;
      case 15: r.hp076 = decodeTime(p); break;
      case 16: r.height140 = static_cast<int16_t>(be(p, 2)); break;
      case 17: r.quality = decode090(p, n); break;
      case 18: r.mops = Mops210{bit(p[0], 7), field(p[0], 6, 4), field(p[0], 3, 1)}; break;
      case 19: r.mode3a = static_cast<uint16_t>(be(p, 2) & 0x0FFFu); break;
      case 21: r.fl145 = static_cast<int16_t>(be(p, 2)); break;
      case 22: r.mag152 = static_cast<uint16_t>(be(p, 2)); break;
      case 23:
        r.status = Status200{bit(p[0], 8), bit(p[0], 7), bit(p[0], 6), field(p[0], 5, 3),
                             field(p[0], 2, 1)};
        break;
      case 24: r.bvr155 = decodeRate(p); break;
      case 25: r.gvr157 = decodeRate(p); break;
      case 26: {
        const uint32_t v = be(p, 4);
        r.vector160 = Vector160{(v >> 31) != 0, static_cast<uint16_t>((v >> 16) & 0x7FFFu),
                                static_cast<uint16_t>(v & 0xFFFFu)};
        break;
      }
      case 28: r.tod077 = be(p, 3); break;
      case 29: r.ident = decode170(p); break;
      case 30: r.ecat = p[0]; break;
      case 36: r.aos008 = p[0]; break;
      case 38: r.amplitude132 = static_cast<int8_t>(p[0]); break;
      case 48:
        r.opaque[48].assign(p, p + n);
        if (auto sgv = decodeReSgv(p, n)) r.sgv = *sgv;
        break;
      default: r.opaque[it.frn].assign(p, p + n); break;
    }
  }
  return r;
}

}  // namespace

DecodedBlock decodeBlock(const uint8_t* block, std::size_t size, Edition edition) {
  DecodedBlock out;
  if (size < 3) {
    out.error = true;
    out.error_text = "block shorter than its header";
    return out;
  }
  const std::size_t len = (std::size_t{block[1]} << 8) | block[2];
  if (block[0] != 21) {
    out.error = true;
    out.error_text = "not a CAT 21 block";
    return out;
  }
  if (len != size) {
    out.error = true;
    out.error_text = "LEN does not match the block size";
    return out;
  }
  std::vector<detail::RecordLayout> layouts;
  const detail::WalkStatus st =
      detail::walkRecords(block + 3, size - 3, true, edition, &layouts, &out.error_text);
  std::size_t off = 3;
  for (const detail::RecordLayout& l : layouts) {
    out.records.push_back(decodeRecord(block + off, l));
    off += l.size;
  }
  out.error = st != detail::WalkStatus::ok;
  return out;
}

}  // namespace asterix
}  // namespace skycued
