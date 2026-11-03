#include "asterix/cat021_encode.hpp"

#include <stdexcept>
#include <string>

namespace skycued {
namespace asterix {

namespace {

using Bytes = std::vector<uint8_t>;

[[noreturn]] void bad(const std::string& what) {
  throw std::invalid_argument("cat021 encode: " + what);
}

uint32_t check(uint64_t v, uint64_t max, const char* what) {
  if (v > max) bad(std::string(what) + " out of range");
  return static_cast<uint32_t>(v);
}

// Two's complement of v in `bits` bits, after a range check.
uint32_t checkSigned(int64_t v, int bits, const char* what) {
  const int64_t lim = int64_t{1} << (bits - 1);
  if (v < -lim || v >= lim) bad(std::string(what) + " out of range");
  return static_cast<uint32_t>(static_cast<uint64_t>(v) & ((uint64_t{1} << bits) - 1));
}

void put(Bytes& out, uint32_t v, int n) {
  for (int i = n - 1; i >= 0; --i) out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFFu));
}

uint8_t b(bool v, int pos) { return static_cast<uint8_t>(v ? (1u << (pos - 1)) : 0u); }

// Octets past the modelled extensions, with their FX bits recomputed.
void putMore(Bytes& out, const Bytes& more) {
  for (std::size_t i = 0; i < more.size(); ++i) {
    out.push_back(static_cast<uint8_t>((more[i] & 0xFEu) | (i + 1 < more.size() ? 1u : 0u)));
  }
}

Bytes encode040(const Descriptor040& d) {
  const bool ext2 = d.has_ext2 || !d.more.empty();
  const bool ext1 = d.has_ext1 || ext2;
  Bytes out;
  out.push_back(static_cast<uint8_t>((check(d.atp, 7, "040 ATP") << 5) |
                                     (check(d.arc, 3, "040 ARC") << 3) | b(d.rc, 3) |
                                     b(d.rab, 2) | b(ext1, 1)));
  if (ext1) {
    out.push_back(static_cast<uint8_t>(b(d.dcr, 8) | b(d.gbs, 7) | b(d.sim, 6) | b(d.tst, 5) |
                                       b(d.saa, 4) | (check(d.cl, 3, "040 CL") << 1) |
                                       b(ext2, 1)));
  }
  if (ext2) {
    out.push_back(static_cast<uint8_t>(b(d.llc, 7) | b(d.ipc, 6) | b(d.nogo, 5) | b(d.cpr, 4) |
                                       b(d.ldpj, 3) | b(d.rcf, 2) | b(!d.more.empty(), 1)));
  }
  putMore(out, d.more);
  return out;
}

Bytes encode090(const Quality090& q) {
  const bool ext2 = q.has_ext2 || !q.more.empty();
  const bool ext1 = q.has_ext1 || ext2;
  Bytes out;
  out.push_back(static_cast<uint8_t>((check(q.nucr_nacv, 7, "090 NUCr/NACv") << 5) |
                                     (check(q.nucp_nic, 15, "090 NUCp/NIC") << 1) | b(ext1, 1)));
  if (ext1) {
    out.push_back(static_cast<uint8_t>(b(q.nic_baro, 8) | (check(q.sil, 3, "090 SIL") << 5) |
                                       (check(q.nacp, 15, "090 NACp") << 1) | b(ext2, 1)));
  }
  if (ext2) {
    out.push_back(static_cast<uint8_t>(b(q.sils, 6) | (check(q.sda, 3, "090 SDA") << 3) |
                                       (check(q.gva, 3, "090 GVA") << 1) |
                                       b(!q.more.empty(), 1)));
  }
  putMore(out, q.more);
  return out;
}

Bytes encodeRate(const Rate& r, const char* what) {
  Bytes out;
  put(out, (r.re ? 0x8000u : 0u) | checkSigned(r.raw, 15, what), 2);
  return out;
}

Bytes encodeTime(const Time074& t, const char* what) {
  Bytes out;
  put(out, (check(t.fsi, 3, what) << 30) | check(t.frac, 0x3FFFFFFF, what), 4);
  return out;
}

uint32_t identCode(char c) {
  if (c >= 'A' && c <= 'Z') return static_cast<uint32_t>(c - 'A' + 1);
  if (c >= '0' && c <= '9') return static_cast<uint32_t>(c - '0' + 48);
  if (c == ' ') return 32;
  bad("ident character outside A-Z, 0-9 and space");
}

Bytes encode170(const std::string& ident) {
  if (ident.size() > 8) bad("ident longer than 8 characters");
  uint64_t v = 0;
  for (std::size_t i = 0; i < 8; ++i) v = (v << 6) | identCode(i < ident.size() ? ident[i] : ' ');
  Bytes out;
  for (int i = 5; i >= 0; --i) out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFFu));
  return out;
}

// RE holding only SGV (primary subfield bit 4).
Bytes encodeReSgv(const SurfaceVector& s) {
  Bytes sgv;
  put(sgv,
      (s.stp ? 0x8000u : 0u) | (s.hts ? 0x4000u : 0u) | (s.htt ? 0x2000u : 0u) |
          (s.hrd ? 0x1000u : 0u) | (check(s.gss_raw, 0x7FF, "SGV GSS") << 1) |
          (s.hgt_raw ? 1u : 0u),
      2);
  if (s.hgt_raw) sgv.push_back(static_cast<uint8_t>(check(*s.hgt_raw, 0x7F, "SGV HGT") << 1));
  Bytes out{static_cast<uint8_t>(2 + sgv.size()), 0x08};
  out.insert(out.end(), sgv.begin(), sgv.end());
  return out;
}

template <class A, class B>
bool pairSet(const std::optional<A>& a, const std::optional<B>& b, const char* what) {
  if (a.has_value() != b.has_value()) bad(std::string(what) + " must be set together");
  return a.has_value();
}

// The modelled field for `frn`, if set.
std::optional<Bytes> modelled(const Cat021Record& r, int frn) {
  Bytes out;
  switch (frn) {
    case 1:
      if (!pairSet(r.sac, r.sic, "sac and sic")) return std::nullopt;
      out = {*r.sac, *r.sic};
      return out;
    case 2:
      if (!r.trd) return std::nullopt;
      return encode040(*r.trd);
    case 5:
      if (!r.tod071) return std::nullopt;
      put(out, check(*r.tod071, 0xFFFFFF, "071"), 3);
      return out;
    case 6:
      if (!pairSet(r.lat130, r.lon130, "lat130 and lon130")) return std::nullopt;
      put(out, checkSigned(*r.lat130, 24, "130 LAT"), 3);
      put(out, checkSigned(*r.lon130, 24, "130 LON"), 3);
      return out;
    case 7:
      if (!pairSet(r.lat131, r.lon131, "lat131 and lon131")) return std::nullopt;
      put(out, static_cast<uint32_t>(*r.lat131), 4);
      put(out, static_cast<uint32_t>(*r.lon131), 4);
      return out;
    case 8:
      if (!r.tod072) return std::nullopt;
      put(out, check(*r.tod072, 0xFFFFFF, "072"), 3);
      return out;
    case 9:
      if (!r.air_speed) return std::nullopt;
      put(out, (r.air_speed->im ? 0x8000u : 0u) | check(r.air_speed->raw, 0x7FFF, "150"), 2);
      return out;
    case 10:
      if (!r.tas_kt) return std::nullopt;
      put(out, (r.tas_kt->re ? 0x8000u : 0u) | check(r.tas_kt->kt, 0x7FFF, "151"), 2);
      return out;
    case 11:
      if (!r.address) return std::nullopt;
      put(out, check(*r.address, 0xFFFFFF, "address"), 3);
      return out;
    case 12:
      if (!r.tod073) return std::nullopt;
      put(out, check(*r.tod073, 0xFFFFFF, "073"), 3);
      return out;
    case 13:
      if (!r.hp074) return std::nullopt;
      return encodeTime(*r.hp074, "074");
    case 14:
      if (!r.tod075) return std::nullopt;
      put(out, check(*r.tod075, 0xFFFFFF, "075"), 3);
      return out;
    case 15:
      if (!r.hp076) return std::nullopt;
      return encodeTime(*r.hp076, "076");
    case 16:
      if (!r.height140) return std::nullopt;
      put(out, static_cast<uint16_t>(*r.height140), 2);
      return out;
    case 17:
      if (!r.quality) return std::nullopt;
      return encode090(*r.quality);
    case 18:
      if (!r.mops) return std::nullopt;
      out.push_back(static_cast<uint8_t>(b(r.mops->vns, 7) | (check(r.mops->vn, 7, "210 VN") << 3) |
                                         check(r.mops->ltt, 7, "210 LTT")));
      return out;
    case 19:
      if (!r.mode3a) return std::nullopt;
      put(out, check(*r.mode3a, 07777, "mode3a"), 2);
      return out;
    case 21:
      if (!r.fl145) return std::nullopt;
      put(out, static_cast<uint16_t>(*r.fl145), 2);
      return out;
    case 22:
      if (!r.mag152) return std::nullopt;
      put(out, *r.mag152, 2);
      return out;
    case 23:
      if (!r.status) return std::nullopt;
      out.push_back(static_cast<uint8_t>(b(r.status->icf, 8) | b(r.status->lnav, 7) |
                                         b(r.status->me, 6) | (check(r.status->ps, 7, "200 PS") << 2) |
                                         check(r.status->ss, 3, "200 SS")));
      return out;
    case 24:
      if (!r.bvr155) return std::nullopt;
      return encodeRate(*r.bvr155, "155");
    case 25:
      if (!r.gvr157) return std::nullopt;
      return encodeRate(*r.gvr157, "157");
    case 26:
      if (!r.vector160) return std::nullopt;
      put(out, (r.vector160->re ? 0x80000000u : 0u) |
                   (check(r.vector160->gs_raw, 0x7FFF, "160 GS") << 16) | r.vector160->ta_raw,
          4);
      return out;
    case 28:
      if (!r.tod077) return std::nullopt;
      put(out, check(*r.tod077, 0xFFFFFF, "077"), 3);
      return out;
    case 29:
      if (!r.ident) return std::nullopt;
      return encode170(*r.ident);
    case 30:
      if (!r.ecat) return std::nullopt;
      out.push_back(*r.ecat);
      return out;
    case 36:
      if (!r.aos008) return std::nullopt;
      out.push_back(*r.aos008);
      return out;
    case 38:
      if (!r.amplitude132) return std::nullopt;
      out.push_back(static_cast<uint8_t>(*r.amplitude132));
      return out;
    case 48:
      if (!r.sgv) return std::nullopt;
      return encodeReSgv(*r.sgv);
    default:
      return std::nullopt;
  }
}

}  // namespace

std::vector<uint8_t> encodeRecord(const Cat021Record& r) {
  for (const auto& kv : r.opaque) {
    if (kv.first < 1 || kv.first > 49) bad("opaque FRN " + std::to_string(kv.first) + " out of range");
  }
  Bytes fspec(7, 0);
  Bytes body;
  int max_frn = 0;
  for (int frn = 1; frn <= 49; ++frn) {
    std::optional<Bytes> item = modelled(r, frn);
    if (!item) {
      const auto it = r.opaque.find(frn);
      if (it == r.opaque.end()) continue;
      std::size_t len = 0;
      std::string why;
      if (detail::measureItem(frn, it->second.data(), it->second.size(), len, &why) !=
              detail::WalkStatus::ok ||
          len != it->second.size()) {
        bad("opaque FRN " + std::to_string(frn) + " bytes do not match its length rule" +
            (why.empty() ? "" : " (" + why + ")"));
      }
      item = it->second;
    }
    const int idx = frn - 1;
    fspec[static_cast<std::size_t>(idx / 7)] |= static_cast<uint8_t>(0x80u >> (idx % 7));
    body.insert(body.end(), item->begin(), item->end());
    max_frn = frn;
  }
  if (max_frn == 0) bad("record has no item");
  const std::size_t n = static_cast<std::size_t>((max_frn + 6) / 7);
  fspec.resize(n);
  for (std::size_t i = 0; i + 1 < n; ++i) fspec[i] |= 0x01;
  fspec.insert(fspec.end(), body.begin(), body.end());
  return fspec;
}

std::vector<uint8_t> encodeBlock(const std::vector<Cat021Record>& records) {
  Bytes body;
  for (const Cat021Record& r : records) {
    const Bytes rec = encodeRecord(r);
    body.insert(body.end(), rec.begin(), rec.end());
  }
  const std::size_t len = body.size() + 3;
  if (len > 0xFFFF) bad("block longer than 65535 octets");
  Bytes out{0x15};
  put(out, static_cast<uint32_t>(len), 2);
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

}  // namespace asterix
}  // namespace skycued
