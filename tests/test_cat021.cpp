#define BOOST_TEST_MODULE skycued_cat021
#include <boost/test/unit_test.hpp>

#include "asterix/cat021.hpp"
#include "asterix/cat021_encode.hpp"
#include "asterix/cat021_framer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

using namespace skycued::asterix;

namespace {

using Bytes = std::vector<uint8_t>;

// "c7 11 3b" -> {0xc7, 0x11, 0x3b}. Spaces are ignored.
Bytes hex(const std::string& s) {
  Bytes out;
  std::string digits;
  for (char c : s) {
    if (c != ' ') digits.push_back(c);
  }
  for (std::size_t i = 0; i + 1 < digits.size(); i += 2) {
    out.push_back(static_cast<uint8_t>(std::stoul(digits.substr(i, 2), nullptr, 16)));
  }
  return out;
}

Bytes join(std::initializer_list<Bytes> parts) {
  Bytes out;
  for (const Bytes& p : parts) out.insert(out.end(), p.begin(), p.end());
  return out;
}

// Hand-built FSPEC for the given FRNs, so decoder tests do not depend on the
// encoder.
Bytes fspec(const std::vector<int>& frns) {
  int max_frn = 0;
  for (int f : frns) max_frn = std::max(max_frn, f);
  const std::size_t n = static_cast<std::size_t>((max_frn + 6) / 7);
  Bytes out(n, 0);
  for (int f : frns) {
    const int idx = f - 1;
    out[static_cast<std::size_t>(idx / 7)] |= static_cast<uint8_t>(0x80 >> (idx % 7));
  }
  for (std::size_t i = 0; i + 1 < n; ++i) out[i] |= 0x01;
  return out;
}

Bytes block(const Bytes& records) {
  const std::size_t len = records.size() + 3;
  Bytes out{0x15, static_cast<uint8_t>(len >> 8), static_cast<uint8_t>(len & 0xFF)};
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 11
  // gcc 11 false positive: -Wstringop-overread ("reading 1 or more bytes from a
  // region of size 0") on the memcpy inlined from vector::insert, on a path
  // where records is empty and insert copies nothing.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overread"
#endif
  out.insert(out.end(), records.begin(), records.end());
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 11
#pragma GCC diagnostic pop
#endif
  return out;
}

DecodedBlock decode(const Bytes& b) { return decodeBlock(b.data(), b.size(), Edition::e2_6); }

Bytes slice(const Bytes& b, std::size_t off, std::size_t n) {
  return Bytes(b.begin() + static_cast<std::ptrdiff_t>(off),
               b.begin() + static_cast<std::ptrdiff_t>(off + n));
}

int32_t raw130(double deg) { return static_cast<int32_t>(std::lround(deg / kLsb130Deg)); }
int32_t raw131(double deg) { return static_cast<int32_t>(std::lround(deg / kLsb131Deg)); }

constexpr double kIcdLat = 39.933280851692;
constexpr double kIcdLon = -88.3812030404806;

// The vendor interface document's example record, rebuilt from the dissected values.
Cat021Record icdRecord() {
  Cat021Record r;
  r.sac = 0;
  r.sic = 0;
  Descriptor040 trd;
  trd.atp = 0;
  trd.has_ext1 = true;
  r.trd = trd;
  r.lat130 = raw130(kIcdLat);
  r.lon130 = raw130(kIcdLon);
  r.lat131 = raw131(kIcdLat);
  r.lon131 = raw131(kIcdLon);
  r.address = 0xA6452D;
  Quality090 q;
  q.nucr_nacv = 2;
  q.nucp_nic = 8;
  q.has_ext1 = true;
  q.nic_baro = true;
  q.sil = 3;
  q.nacp = 10;
  q.has_ext2 = true;
  q.sils = false;
  q.sda = 3;
  q.gva = 2;
  r.quality = q;
  Mops210 m;
  m.vn = 2;
  m.ltt = 2;
  r.mops = m;
  r.mode3a = 02001;
  r.fl145 = 1561;
  r.status = Status200{};
  r.bvr155 = Rate{false, 0};
  r.vector160 = Vector160{false, 1872, 16384};
  r.ident = "ATN3597";
  r.ecat = 3;
  r.aos008 = 0;
  return r;
}

// Offset of an item in an encoded record, found by walking the shared layout.
std::size_t itemOffset(const Bytes& rec, int frn) {
  detail::RecordLayout layout;
  BOOST_REQUIRE(detail::walkRecord(rec.data(), rec.size(), layout) == detail::WalkStatus::ok);
  for (const auto& it : layout.items) {
    if (it.frn == frn) return it.offset;
  }
  BOOST_FAIL("frn not present");
  return 0;
}

}  // namespace

BOOST_AUTO_TEST_CASE(edition_names) {
  BOOST_CHECK(parseEdition("2.2") == Edition::e2_2);
  BOOST_CHECK(parseEdition("2.6") == Edition::e2_6);
  BOOST_CHECK(parseEdition("2.7") == Edition::e2_7);
  BOOST_CHECK(!parseEdition("2.1"));
  BOOST_CHECK(!parseEdition("2.60"));
  BOOST_CHECK(!parseEdition(" 2.6"));
  BOOST_CHECK(!parseEdition(""));
  BOOST_CHECK_EQUAL(std::string(editionName(Edition::e2_5)), "2.5");
  BOOST_CHECK_EQUAL(std::string(editionName(Edition::e2_7)), "2.7");
}

// Encoder consistency check against the vendor interface document's Wireshark dissection: the ICD
// shows the example as an image, so this is NOT an independent byte vector. The
// FSPEC, the lengths and the 090/070/145/170/080 bytes are what the dissection
// shows. The values of 010/040/210/200/155/160/020/008 are our own choice.
BOOST_AUTO_TEST_CASE(icd_p14_example_reencoded) {
  const Bytes rec = encodeRecord(icdRecord());
  BOOST_CHECK_EQUAL(rec.size(), 50u);
  const Bytes fs = hex("c7 11 3b 69 c1 80");
  BOOST_CHECK(slice(rec, 0, 6) == fs);

  const Bytes blk = encodeBlock({icdRecord()});
  BOOST_CHECK_EQUAL(blk.size(), 53u);
  BOOST_CHECK(slice(blk, 0, 3) == hex("15 00 35"));

  BOOST_CHECK(slice(rec, itemOffset(rec, 17), 3) == hex("51 f5 1c"));
  BOOST_CHECK(slice(rec, itemOffset(rec, 19), 2) == hex("04 01"));
  BOOST_CHECK(slice(rec, itemOffset(rec, 21), 2) == hex("06 19"));
  BOOST_CHECK(slice(rec, itemOffset(rec, 29), 6) == hex("05 43 b3 d7 9d e0"));
  BOOST_CHECK(slice(rec, itemOffset(rec, 11), 3) == hex("a6 45 2d"));

  const DecodedBlock d = decode(blk);
  BOOST_REQUIRE(!d.error);
  BOOST_REQUIRE_EQUAL(d.records.size(), 1u);
  const Cat021Record& r = d.records[0];
  BOOST_REQUIRE(r.lat131 && r.lon131);
  BOOST_CHECK_SMALL(*r.lat131 * kLsb131Deg - kIcdLat, 1e-7);
  BOOST_CHECK_SMALL(*r.lon131 * kLsb131Deg - kIcdLon, 1e-7);
  BOOST_REQUIRE(r.fl145);
  BOOST_CHECK_CLOSE(*r.fl145 / 4.0, 390.25, 1e-9);
  BOOST_REQUIRE(r.mode3a);
  char sq[8];
  std::snprintf(sq, sizeof sq, "%04o", static_cast<unsigned>(*r.mode3a));
  BOOST_CHECK_EQUAL(std::string(sq), "2001");
  BOOST_REQUIRE(r.ident);
  BOOST_CHECK_EQUAL(*r.ident, "ATN3597 ");
  BOOST_REQUIRE(r.address);
  BOOST_CHECK_EQUAL(*r.address, 0xA6452Du);
  BOOST_REQUIRE(r.quality);
  BOOST_CHECK_EQUAL(r.quality->nacp, 10);
  BOOST_CHECK_EQUAL(r.quality->gva, 2);
  BOOST_CHECK(r.bytes == rec);
  BOOST_CHECK(r.opaque.empty());
}

BOOST_AUTO_TEST_CASE(round_trip_every_modelled_item) {
  Cat021Record in = icdRecord();
  in.sac = 12;
  in.sic = 34;
  Descriptor040 trd;
  trd.atp = 2;
  trd.arc = 1;
  trd.rc = true;
  trd.rab = false;
  trd.has_ext1 = true;
  trd.gbs = true;
  trd.saa = true;
  trd.cl = 2;
  trd.has_ext2 = true;
  trd.llc = true;
  trd.rcf = true;
  trd.more = {0x83, 0x02};  // ext 3 + ext 4 as received (FX on the first)
  in.trd = trd;
  in.tod071 = 1;
  in.tod072 = 2;
  in.tod073 = 0xFFFFFF;
  in.tod075 = 4;
  in.tod077 = 5;
  in.hp074 = Time074{1, 0x3FFFFFFF};
  in.hp076 = Time074{2, 12345};
  in.height140 = -240;
  in.air_speed = AirSpeed150{true, 820};
  in.tas_kt = TrueAirspeed151{true, 450};
  Quality090 q = *in.quality;
  q.more = {0x81, 0x00};
  in.quality = q;
  in.mops = Mops210{true, 3, 1};
  in.mag152 = 0xABCD;
  in.status = Status200{true, true, true, 5, 3};
  in.bvr155 = Rate{false, 16383};
  in.gvr157 = Rate{true, -16384};
  in.vector160 = Vector160{true, 0x7FFF, 0xFFFF};
  in.ecat = 21;
  in.aos008 = 0xA5;
  in.amplitude132 = -53;
  SurfaceVector sgv;
  sgv.hts = true;
  sgv.htt = true;
  sgv.hrd = true;
  sgv.gss_raw = 120;
  sgv.hgt_raw = 32;
  in.sgv = sgv;

  const Bytes rec = encodeRecord(in);
  BOOST_CHECK(slice(rec, itemOffset(rec, 48), 5) == hex("05 08 70 f1 40"));

  const DecodedBlock d = decode(encodeBlock({in}));
  BOOST_REQUIRE(!d.error);
  BOOST_REQUIRE_EQUAL(d.records.size(), 1u);
  const Cat021Record& r = d.records[0];
  BOOST_CHECK(r.bytes == rec);
  BOOST_CHECK(encodeRecord(r) == rec);

  BOOST_CHECK_EQUAL(*r.sac, 12);
  BOOST_CHECK_EQUAL(*r.sic, 34);
  BOOST_CHECK_EQUAL(r.trd->atp, 2);
  BOOST_CHECK_EQUAL(r.trd->arc, 1);
  BOOST_CHECK(r.trd->rc);
  BOOST_CHECK(!r.trd->rab);
  BOOST_CHECK(r.trd->gbs && r.trd->saa && !r.trd->sim && !r.trd->tst && !r.trd->dcr);
  BOOST_CHECK_EQUAL(r.trd->cl, 2);
  BOOST_CHECK(r.trd->has_ext2 && r.trd->llc && r.trd->rcf && !r.trd->ipc && !r.trd->nogo);
  BOOST_CHECK(r.trd->more == trd.more);
  BOOST_CHECK_EQUAL(*r.tod071, 1u);
  BOOST_CHECK_EQUAL(*r.tod072, 2u);
  BOOST_CHECK_EQUAL(*r.tod073, 0xFFFFFFu);
  BOOST_CHECK_EQUAL(*r.tod075, 4u);
  BOOST_CHECK_EQUAL(*r.tod077, 5u);
  BOOST_CHECK_EQUAL(r.hp074->fsi, 1);
  BOOST_CHECK_EQUAL(r.hp074->frac, 0x3FFFFFFFu);
  BOOST_CHECK_EQUAL(r.hp076->fsi, 2);
  BOOST_CHECK_EQUAL(r.hp076->frac, 12345u);
  BOOST_CHECK_EQUAL(*r.lat130, *in.lat130);
  BOOST_CHECK_EQUAL(*r.lon130, *in.lon130);
  BOOST_CHECK_EQUAL(*r.lat131, *in.lat131);
  BOOST_CHECK_EQUAL(*r.lon131, *in.lon131);
  BOOST_CHECK_EQUAL(*r.height140, -240);
  BOOST_CHECK(r.air_speed->im);
  BOOST_CHECK_EQUAL(r.air_speed->raw, 820);
  BOOST_CHECK_CLOSE(*r.air_speed->mach(), 0.82, 1e-9);
  BOOST_CHECK(!r.air_speed->iasKt());
  BOOST_CHECK(r.tas_kt->re);
  BOOST_CHECK_EQUAL(r.tas_kt->kt, 450);
  BOOST_CHECK(r.quality->more == q.more);
  BOOST_CHECK(r.mops->vns);
  BOOST_CHECK_EQUAL(r.mops->vn, 3);
  BOOST_CHECK_EQUAL(r.mops->ltt, 1);
  BOOST_CHECK_EQUAL(*r.mag152, 0xABCD);
  BOOST_CHECK(r.status->icf && r.status->lnav && r.status->me);
  BOOST_CHECK_EQUAL(r.status->ps, 5);
  BOOST_CHECK_EQUAL(r.status->ss, 3);
  BOOST_CHECK(!r.bvr155->re);
  BOOST_CHECK_EQUAL(r.bvr155->raw, 16383);
  BOOST_CHECK(r.gvr157->re);
  BOOST_CHECK_EQUAL(r.gvr157->raw, -16384);
  BOOST_CHECK(r.vector160->re);
  BOOST_CHECK_EQUAL(r.vector160->gs_raw, 0x7FFF);
  BOOST_CHECK_EQUAL(r.vector160->ta_raw, 0xFFFF);
  BOOST_CHECK_EQUAL(*r.ecat, 21);
  BOOST_CHECK_EQUAL(*r.aos008, 0xA5);
  BOOST_CHECK_EQUAL(*r.amplitude132, -53);
  BOOST_REQUIRE(r.sgv);
  BOOST_CHECK(!r.sgv->stp && r.sgv->hts && r.sgv->htt && r.sgv->hrd);
  BOOST_CHECK_EQUAL(r.sgv->gss_raw, 120);
  BOOST_CHECK_CLOSE(r.sgv->gss_raw * 0.125, 15.0, 1e-9);
  BOOST_REQUIRE(r.sgv->hgt_raw);
  BOOST_CHECK_EQUAL(*r.sgv->hgt_raw, 32);
  BOOST_CHECK_CLOSE(*r.sgv->hgt_raw * 2.8125, 90.0, 1e-9);
  // RE is kept whole in opaque as well (R10); nothing else is opaque.
  BOOST_REQUIRE_EQUAL(r.opaque.size(), 1u);
  BOOST_CHECK(r.opaque.at(48) == hex("05 08 70 f1 40"));

  // `in` had only sgv: the RE above was built from it. The decoded record has
  // both, and opaque[48] (here the same bytes) is what gets written.
  Cat021Record no_sgv = r;
  no_sgv.sgv.reset();
  BOOST_CHECK(encodeRecord(no_sgv) == rec);
}

// opaque[48] wins over sgv (an RE may carry more than SGV).
BOOST_AUTO_TEST_CASE(re_with_tnh_round_trips) {
  // RE LEN 7: primary 0A (SGV b4, TNH b2), SGV 70 f1 40 (3 octets), TNH 40 00.
  const Bytes re = hex("07 0a 70 f1 40 40 00");
  const Bytes rec = join({fspec({1, 48}), hex("01 02"), re});
  const DecodedBlock d = decode(block(rec));
  BOOST_REQUIRE(!d.error);
  const Cat021Record& r = d.records.at(0);
  BOOST_REQUIRE(r.sgv);
  BOOST_CHECK_EQUAL(r.sgv->gss_raw, 120);
  BOOST_CHECK_EQUAL(*r.sgv->hgt_raw, 32);
  BOOST_CHECK(r.opaque.at(48) == re);
  BOOST_CHECK(encodeRecord(r) == r.bytes);

  // Both set, with a different sgv: the output RE is opaque[48].
  Cat021Record both = r;
  both.sgv = SurfaceVector{true, false, false, false, 7, {}};
  const Bytes out = encodeRecord(both);
  BOOST_CHECK(out == rec);
  BOOST_CHECK(slice(out, itemOffset(out, 48), re.size()) == re);
}

BOOST_AUTO_TEST_CASE(air_speed_ias) {
  const Bytes blk = block(join({fspec({1, 9, 10}), hex("00 01"), hex("10 00"), hex("01 c2")}));
  const DecodedBlock d = decode(blk);
  BOOST_REQUIRE(!d.error);
  BOOST_REQUIRE_EQUAL(d.records.size(), 1u);
  const Cat021Record& r = d.records[0];
  BOOST_REQUIRE(r.air_speed);
  BOOST_CHECK(!r.air_speed->im);
  BOOST_CHECK(!r.air_speed->mach());
  BOOST_CHECK_CLOSE(*r.air_speed->iasKt(), 0x1000 * kKtPerIas150, 1e-9);
  BOOST_REQUIRE(r.tas_kt);
  BOOST_CHECK(!r.tas_kt->re);
  BOOST_CHECK_EQUAL(r.tas_kt->kt, 450);
}

// Every item that is only length-walked, then RE (FRN 48) after it so a wrong
// length shows up as a wrong SGV.
BOOST_AUTO_TEST_CASE(opaque_items_length_skipped) {
  Bytes tid30(30, 0x5A);
  Bytes mb24(24, 0x3C);
  const std::vector<std::pair<int, Bytes>> cases = {
      {3, hex("0f ff")},                                   // 161
      {4, hex("07")},                                      // 015
      {20, hex("ff 9c")},                                  // 230
      {27, hex("03 ff")},                                  // 165
      {31, hex("a0 00 64 ff ec")},                         // 220 WS + TMP
      {32, hex("c0 10")},                                  // 146
      {33, hex("80 20")},                                  // 148
      {34, join({hex("c0 81 00 02"), tid30})},             // 110 TIS 2 + TID REP 2
      {35, hex("08")},                                     // 016
      {37, hex("21 a0")},                                  // 271 + ext 1
      {39, join({hex("03"), mb24})},                       // 250 REP 3
      {40, hex("e2 00 00 00 00 00 00")},                   // 260
      {41, hex("05")},                                     // 400
      {42, hex("41 08 0d 0e")},                            // 295, 2 primary octets
  };
  const Bytes re = hex("05 08 70 f1 40");
  for (const auto& c : cases) {
    BOOST_TEST_CONTEXT("frn " << c.first) {
      const Bytes rec = join({fspec({1, c.first, 48}), hex("01 02"), c.second, re});
      const DecodedBlock d = decode(block(rec));
      BOOST_REQUIRE(!d.error);
      BOOST_REQUIRE_EQUAL(d.records.size(), 1u);
      const Cat021Record& r = d.records[0];
      BOOST_CHECK_EQUAL(*r.sac, 1);
      BOOST_CHECK_EQUAL(*r.sic, 2);
      BOOST_REQUIRE(r.opaque.count(c.first));
      BOOST_CHECK(r.opaque.at(c.first) == c.second);
      BOOST_REQUIRE(r.sgv);
      BOOST_CHECK_EQUAL(r.sgv->gss_raw, 120);
      BOOST_CHECK(r.opaque.at(48) == re);
      BOOST_CHECK(r.bytes == rec);
      BOOST_CHECK(encodeRecord(r) == rec);
    }
  }
  // SP (FRN 49) of 4 octets, then a second record that must start right after.
  const Bytes rec1 = join({fspec({1, 49}), hex("01 02"), hex("04 11 22 33")});
  const Bytes rec2 = join({fspec({1}), hex("03 04")});
  const DecodedBlock d = decode(block(join({rec1, rec2})));
  BOOST_REQUIRE(!d.error);
  BOOST_REQUIRE_EQUAL(d.records.size(), 2u);
  BOOST_CHECK(d.records[0].opaque.at(49) == hex("04 11 22 33"));
  BOOST_CHECK(d.records[0].bytes == rec1);
  BOOST_CHECK_EQUAL(*d.records[1].sac, 3);
  BOOST_CHECK_EQUAL(*d.records[1].sic, 4);
}

BOOST_AUTO_TEST_CASE(fx_chains_every_length) {
  // 040 with 1..5 octets, followed by 080.
  for (std::size_t n = 1; n <= 5; ++n) {
    BOOST_TEST_CONTEXT("040 octets " << n) {
      Bytes item(n, 0);
      for (std::size_t i = 0; i < n; ++i) item[i] = static_cast<uint8_t>(i + 1 < n ? 0x01 : 0x00);
      item[0] |= 0x60;  // ATP 3
      const Bytes rec = join({fspec({2, 11}), item, hex("12 34 56")});
      const DecodedBlock d = decode(block(rec));
      BOOST_REQUIRE(!d.error);
      BOOST_REQUIRE_EQUAL(d.records.size(), 1u);
      const Cat021Record& r = d.records[0];
      BOOST_CHECK_EQUAL(r.trd->atp, 3);
      BOOST_CHECK_EQUAL(r.trd->has_ext1, n >= 2);
      BOOST_CHECK_EQUAL(r.trd->has_ext2, n >= 3);
      BOOST_CHECK_EQUAL(r.trd->more.size(), n > 3 ? n - 3 : 0);
      BOOST_CHECK_EQUAL(*r.address, 0x123456u);
      BOOST_CHECK(encodeRecord(r) == rec);
    }
  }
  // 090 with 1..8 octets, followed by 210.
  for (std::size_t n = 1; n <= 8; ++n) {
    BOOST_TEST_CONTEXT("090 octets " << n) {
      Bytes item(n, 0);
      for (std::size_t i = 0; i < n; ++i) item[i] = static_cast<uint8_t>(i + 1 < n ? 0x01 : 0x00);
      item[0] |= 0x1E;  // NUCp/NIC 15
      const Bytes rec = join({fspec({17, 18}), item, hex("12")});
      const DecodedBlock d = decode(block(rec));
      BOOST_REQUIRE(!d.error);
      BOOST_REQUIRE_EQUAL(d.records.size(), 1u);
      const Cat021Record& r = d.records[0];
      BOOST_CHECK_EQUAL(r.quality->nucp_nic, 15);
      BOOST_CHECK_EQUAL(r.quality->has_ext1, n >= 2);
      BOOST_CHECK_EQUAL(r.quality->has_ext2, n >= 3);
      BOOST_CHECK_EQUAL(r.quality->more.size(), n > 3 ? n - 3 : 0);
      BOOST_CHECK_EQUAL(r.mops->vn, 2);
      BOOST_CHECK(encodeRecord(r) == rec);
    }
  }
  // FSPEC of 1..7 octets: one item in the last octet each time.
  const std::vector<std::pair<int, Bytes>> last = {
      {1, hex("01 02")},          {8, hex("00 00 80")},  {15, hex("40 00 00 01")},
      {22, hex("00 5a")},         {29, hex("05 43 b3 d7 9d e0")},
      {36, hex("a5")},            {48, hex("05 08 70 f1 40")},
  };
  for (std::size_t k = 0; k < last.size(); ++k) {
    BOOST_TEST_CONTEXT("fspec octets " << k + 1) {
      const Bytes fs = fspec({last[k].first});
      BOOST_CHECK_EQUAL(fs.size(), k + 1);
      const Bytes rec = join({fs, last[k].second});
      const DecodedBlock d = decode(block(rec));
      BOOST_REQUIRE(!d.error);
      BOOST_REQUIRE_EQUAL(d.records.size(), 1u);
      BOOST_CHECK(d.records[0].bytes == rec);
      BOOST_CHECK(encodeRecord(d.records[0]) == rec);
    }
  }
  // An FSPEC that wants an 8th octet is an error.
  const DecodedBlock d = decode(block(hex("01 01 01 01 01 01 01 80 00")));
  BOOST_CHECK(d.error);
  BOOST_CHECK(d.records.empty());
}

BOOST_AUTO_TEST_CASE(empty_fspec_is_error) {
  const DecodedBlock d = decode(block(join({fspec({1}), hex("01 02"), hex("00")})));
  BOOST_CHECK(d.error);
  BOOST_CHECK_EQUAL(d.records.size(), 1u);
  // An all-zero first octet is fine when a later octet has a presence bit (072).
  const DecodedBlock d2 = decode(block(hex("01 80 00 00 00")));
  BOOST_CHECK(!d2.error);
  BOOST_CHECK_EQUAL(d2.records.size(), 1u);
  BOOST_CHECK(decode(block(hex("01 00"))).error);
}

BOOST_AUTO_TEST_CASE(spare_frn_set_discards_rest_of_block) {
  const Bytes good1 = join({fspec({1}), hex("01 01")});
  const Bytes bad = join({hex("01 01 01 01 01 01 80"), hex("00 00")});  // FRN 43
  const Bytes good3 = join({fspec({1}), hex("03 03")});
  const DecodedBlock d = decode(block(join({good1, bad, good3})));
  BOOST_CHECK(d.error);
  BOOST_CHECK(!d.error_text.empty());
  BOOST_REQUIRE_EQUAL(d.records.size(), 1u);
  BOOST_CHECK_EQUAL(*d.records[0].sac, 1);
}

BOOST_AUTO_TEST_CASE(two_records_per_block) {
  Cat021Record a = icdRecord();
  Cat021Record b = icdRecord();
  b.address = 0x000001;
  b.ident = "N12345";
  const Bytes blk = encodeBlock({a, b});
  BOOST_CHECK_EQUAL(blk.size(), 3u + 50u + 50u);
  const DecodedBlock d = decode(blk);
  BOOST_REQUIRE(!d.error);
  BOOST_REQUIRE_EQUAL(d.records.size(), 2u);
  BOOST_CHECK_EQUAL(*d.records[0].address, 0xA6452Du);
  BOOST_CHECK_EQUAL(*d.records[1].address, 0x000001u);
  BOOST_CHECK_EQUAL(*d.records[1].ident, "N12345  ");
}

BOOST_AUTO_TEST_CASE(two_blocks_per_datagram) {
  Cat021Record b = icdRecord();
  b.address = 0x000002;
  const Bytes dg = join({encodeBlock({icdRecord()}), encodeBlock({b, b})});
  const BlockSplit s = splitBlocks(dg.data(), dg.size());
  BOOST_CHECK(!s.truncated);
  BOOST_REQUIRE_EQUAL(s.blocks.size(), 2u);
  BOOST_CHECK_EQUAL(s.blocks[0].offset, 0u);
  BOOST_CHECK_EQUAL(s.blocks[0].size, 53u);
  BOOST_CHECK_EQUAL(s.blocks[0].cat, 21);
  BOOST_CHECK_EQUAL(s.blocks[1].offset, 53u);
  BOOST_CHECK_EQUAL(s.blocks[1].size, 103u);
  const DecodedBlock d0 = decodeBlock(dg.data() + s.blocks[0].offset, s.blocks[0].size, Edition::e2_5);
  const DecodedBlock d1 = decodeBlock(dg.data() + s.blocks[1].offset, s.blocks[1].size, Edition::e2_5);
  BOOST_REQUIRE(!d0.error && !d1.error);
  BOOST_CHECK_EQUAL(d0.records.size(), 1u);
  BOOST_REQUIRE_EQUAL(d1.records.size(), 2u);
  BOOST_CHECK_EQUAL(*d1.records[1].address, 2u);
}

BOOST_AUTO_TEST_CASE(truncated_len_drops_rest) {
  const Bytes good = encodeBlock({icdRecord()});
  {  // LEN past the end
    const Bytes dg = join({good, hex("15 00 ff 01 02"), good});
    const BlockSplit s = splitBlocks(dg.data(), dg.size());
    BOOST_CHECK(s.truncated);
    BOOST_CHECK_EQUAL(s.blocks.size(), 1u);
  }
  {  // LEN 2
    const Bytes dg = join({good, hex("15 00 02"), good});
    const BlockSplit s = splitBlocks(dg.data(), dg.size());
    BOOST_CHECK(s.truncated);
    BOOST_CHECK_EQUAL(s.blocks.size(), 1u);
  }
  {  // trailing bytes shorter than a header
    const Bytes dg = join({good, hex("15 00")});
    const BlockSplit s = splitBlocks(dg.data(), dg.size());
    BOOST_CHECK(s.truncated);
    BOOST_CHECK_EQUAL(s.blocks.size(), 1u);
  }
  {  // other categories are framed too, with their CAT
    const Bytes dg = join({hex("30 00 04 aa"), good});
    const BlockSplit s = splitBlocks(dg.data(), dg.size());
    BOOST_CHECK(!s.truncated);
    BOOST_REQUIRE_EQUAL(s.blocks.size(), 2u);
    BOOST_CHECK_EQUAL(s.blocks[0].cat, 48);
    BOOST_CHECK_EQUAL(s.blocks[1].cat, 21);
  }
}

BOOST_AUTO_TEST_CASE(item_past_len_is_error) {
  // 131 needs 8 octets, only 4 remain before LEN.
  const DecodedBlock d = decode(block(join({fspec({7}), hex("01 02 03 04")})));
  BOOST_CHECK(d.error);
  BOOST_CHECK(d.records.empty());
  // Records after a good one: the good one is kept.
  const DecodedBlock d2 =
      decode(block(join({fspec({1}), hex("01 02"), fspec({7}), hex("01 02 03 04")})));
  BOOST_CHECK(d2.error);
  BOOST_CHECK_EQUAL(d2.records.size(), 1u);
  // A block whose LEN does not match its size is an error.
  Bytes blk = encodeBlock({icdRecord()});
  blk.push_back(0);
  BOOST_CHECK(decode(blk).error);
}

BOOST_AUTO_TEST_CASE(negative_lat_lon_rates) {
  Cat021Record in;
  in.address = 0x7C0001;
  in.lat130 = raw130(-33.9);
  in.lon130 = raw130(-151.2);
  in.lat131 = raw131(-33.9);
  in.lon131 = raw131(-151.2);
  in.bvr155 = Rate{false, static_cast<int16_t>(-1500 / kFpm155)};
  in.gvr157 = Rate{false, -1};
  const Bytes rec = encodeRecord(in);
  BOOST_CHECK(slice(rec, itemOffset(rec, 24), 2) == hex("7f 10"));
  BOOST_CHECK(slice(rec, itemOffset(rec, 25), 2) == hex("7f ff"));
  const DecodedBlock d = decode(block(rec));
  BOOST_REQUIRE(!d.error);
  const Cat021Record& r = d.records.at(0);
  BOOST_CHECK_SMALL(*r.lat130 * kLsb130Deg + 33.9, 2e-5);
  BOOST_CHECK_SMALL(*r.lon130 * kLsb130Deg + 151.2, 2e-5);
  BOOST_CHECK_SMALL(*r.lat131 * kLsb131Deg + 33.9, 1e-7);
  BOOST_CHECK_SMALL(*r.lon131 * kLsb131Deg + 151.2, 1e-7);
  BOOST_CHECK_EQUAL(r.bvr155->raw, -240);
  BOOST_CHECK_CLOSE(r.bvr155->raw * kFpm155, -1500.0, 1e-9);
  BOOST_CHECK_EQUAL(r.gvr157->raw, -1);
  BOOST_CHECK_CLOSE(r.gvr157->raw * kFpm155, -6.25, 1e-9);

  // b16 RE set: re true, value sign-extended from bit 15.
  const DecodedBlock d2 = decode(block(join({fspec({24}), hex("ff 10")})));
  BOOST_REQUIRE(!d2.error);
  BOOST_CHECK(d2.records.at(0).bvr155->re);
  BOOST_CHECK_EQUAL(d2.records.at(0).bvr155->raw, -240);
}

BOOST_AUTO_TEST_CASE(ident_invalid_code_reads_question_mark) {
  // codes 0, 1 (A), 2 (B), 3 (C), then 4 spaces (32)
  // 000000 000001 000010 000011 100000 100000 100000 100000
  const DecodedBlock d = decode(block(join({fspec({29}), hex("00 10 83 82 08 20")})));
  BOOST_REQUIRE(!d.error);
  BOOST_CHECK_EQUAL(*d.records.at(0).ident, "?ABC    ");
}

// Characters outside the ICAO 6-bit set are written as space, never thrown on,
// so a decoded ident with '?' re-encodes.
BOOST_AUTO_TEST_CASE(encode_maps_bad_ident_chars_to_space) {
  Cat021Record r;
  r.ident = "ab?C-1";
  const Bytes rec = encodeRecord(r);
  const DecodedBlock d = decode(block(rec));
  BOOST_REQUIRE(!d.error);
  BOOST_CHECK_EQUAL(*d.records.at(0).ident, "   C 1  ");

  const DecodedBlock q = decode(block(join({fspec({29}), hex("00 10 83 82 08 20")})));
  BOOST_REQUIRE(!q.error);
  BOOST_CHECK_EQUAL(*q.records.at(0).ident, "?ABC    ");
  const Bytes again = encodeRecord(q.records.at(0));
  BOOST_CHECK(slice(again, itemOffset(again, 29), 6) == hex("80 10 83 82 08 20"));  // code 0 -> space (32)
}

BOOST_AUTO_TEST_CASE(encode_rejects_bad_values) {
  Cat021Record r;
  r.address = 0x1000000;
  BOOST_CHECK_THROW(encodeRecord(r), std::invalid_argument);
  r = Cat021Record{};
  r.mode3a = 010000;
  BOOST_CHECK_THROW(encodeRecord(r), std::invalid_argument);
  r = Cat021Record{};
  r.ident = "ABCDEFGHI";
  BOOST_CHECK_THROW(encodeRecord(r), std::invalid_argument);
  r = Cat021Record{};
  BOOST_CHECK_THROW(encodeRecord(r), std::invalid_argument);  // nothing to encode
  r.opaque[3] = hex("01");  // 161 is 2 octets
  BOOST_CHECK_THROW(encodeRecord(r), std::invalid_argument);
  r.opaque.clear();
  r.opaque[44] = hex("00");  // spare FRN
  BOOST_CHECK_THROW(encodeRecord(r), std::invalid_argument);
}

// Garbage, then two valid blocks A and B (the ICD record, so A starts
// 15 00 35 c7 11 3b 69 c1 80 00 00 ...). Byte walk:
//   off 0  `00`: CAT 0 is never a category; drop it and leave sync (run 1, 1 byte).
//   off 1  `15 ff 7e`: CAT 21, LEN 65406, incomplete. The buffered bytes after the
//          header are A's: FSPEC 15 00 (FRN 4, 6), 015 = 35, 130 = c7 11 3b 69 c1
//          80, then A's `00` as the next FSPEC, which has no presence bit: a length
//          error, so drop now (2 bytes).
//   off 2  `ff`, off 3 `7e`: not CAT 21 out of sync; drop (4 bytes).
//   off 4  A: complete, walks cleanly: emitted, back in sync. Then B.
BOOST_AUTO_TEST_CASE(framer_resync_after_garbage) {
  const Bytes a = encodeBlock({icdRecord()});
  const Bytes stream = join({hex("00 15 ff 7e"), a, a});
  Cat021StreamFramer f(Edition::e2_5);
  std::vector<Bytes> out;
  f.feed(stream.data(), stream.size(), out);
  BOOST_REQUIRE_EQUAL(out.size(), 2u);
  BOOST_CHECK(out[0] == a);
  BOOST_CHECK(out[1] == a);
  BOOST_CHECK_EQUAL(f.droppedRuns(), 1u);
  BOOST_CHECK_EQUAL(f.droppedBytes(), 4u);
  BOOST_CHECK_EQUAL(f.otherCategoryBlocks(), 0u);
  BOOST_CHECK(f.inSync());
}

BOOST_AUTO_TEST_CASE(framer_split_across_feeds) {
  const Bytes a = encodeBlock({icdRecord()});
  Cat021StreamFramer f(Edition::e2_6);
  std::vector<Bytes> out;
  for (std::size_t i = 0; i < a.size(); ++i) {
    f.feed(&a[i], 1, out);
    BOOST_CHECK_EQUAL(out.size(), i + 1 == a.size() ? 1u : 0u);
  }
  BOOST_REQUIRE_EQUAL(out.size(), 1u);
  BOOST_CHECK(out[0] == a);
  BOOST_CHECK_EQUAL(f.droppedBytes(), 0u);

  // reset() discards a half-read block.
  out.clear();
  f.feed(a.data(), 20, out);
  f.reset();
  f.feed(a.data(), a.size(), out);
  BOOST_REQUIRE_EQUAL(out.size(), 1u);
  BOOST_CHECK(out[0] == a);
  BOOST_CHECK_EQUAL(f.droppedBytes(), 0u);
  BOOST_CHECK(f.inSync());
}

BOOST_AUTO_TEST_CASE(framer_skips_other_category_in_sync) {
  const Bytes a = encodeBlock({icdRecord()});
  const Bytes cat48 = hex("30 00 06 01 02 03");
  const Bytes stream = join({a, cat48, a});
  Cat021StreamFramer f(Edition::e2_6, true);
  std::vector<Bytes> out;
  // Split inside the CAT 48 block so the skip spans two feeds.
  f.feed(stream.data(), a.size() + 4, out);
  f.feed(stream.data() + a.size() + 4, stream.size() - a.size() - 4, out);
  BOOST_CHECK_EQUAL(out.size(), 2u);
  BOOST_CHECK_EQUAL(f.otherCategoryBlocks(), 1u);
  BOOST_CHECK_EQUAL(f.droppedRuns(), 0u);
  BOOST_CHECK_EQUAL(f.droppedBytes(), 0u);
}

// By default (a CAT 21-only feed) another CAT is garbage: its 6 bytes are
// dropped one at a time in one run, and both CAT 21 blocks still come out.
BOOST_AUTO_TEST_CASE(framer_default_drops_other_category) {
  const Bytes a = encodeBlock({icdRecord()});
  const Bytes stream = join({a, hex("30 00 06 01 02 03"), a});
  Cat021StreamFramer f(Edition::e2_6);
  std::vector<Bytes> out;
  f.feed(stream.data(), stream.size(), out);
  BOOST_CHECK_EQUAL(out.size(), 2u);
  BOOST_CHECK_EQUAL(f.otherCategoryBlocks(), 0u);
  BOOST_CHECK_EQUAL(f.skippedBytes(), 0u);
  BOOST_CHECK_EQUAL(f.droppedRuns(), 1u);
  BOOST_CHECK_EQUAL(f.droppedBytes(), 6u);
  BOOST_CHECK(f.inSync());
}

// A, then a CAT 21 block whose only record has an empty FSPEC, then B. In sync
// the bad block is complete but fails the walk: drop its `15` and leave sync
// (run 1). Its remaining 5 bytes (00 06 00 00 00) are not CAT 21: dropped one by
// one (6 bytes in all). B walks cleanly and is emitted.
BOOST_AUTO_TEST_CASE(framer_bad_cat21_block_resyncs) {
  const Bytes a = encodeBlock({icdRecord()});
  const Bytes bad = hex("15 00 06 00 00 00");
  const Bytes stream = join({a, bad, a});
  Cat021StreamFramer f(Edition::e2_6);
  std::vector<Bytes> out;
  f.feed(stream.data(), stream.size(), out);
  BOOST_CHECK_EQUAL(out.size(), 2u);
  BOOST_CHECK_EQUAL(f.droppedRuns(), 1u);
  BOOST_CHECK_EQUAL(f.droppedBytes(), 6u);
  BOOST_CHECK(f.inSync());
}

// The framer only walks lengths; every block it passes must decode cleanly.
BOOST_AUTO_TEST_CASE(framer_passed_blocks_always_decode) {
  std::mt19937 rng(20261102);
  auto pick = [&rng](uint32_t n) { return static_cast<uint32_t>(rng() % n); };
  Bytes stream;
  std::vector<Bytes> good;
  for (int i = 0; i < 2000; ++i) {
    Cat021Record r = icdRecord();
    r.address = pick(0x1000000);
    if (pick(2)) r.sgv = SurfaceVector{false, true, false, true, static_cast<uint16_t>(pick(2048)), {}};
    if (pick(2)) r.tod073 = pick(0x1000000);
    Bytes blk = pick(3) == 0 ? encodeBlock({r, r}) : encodeBlock({r});
    switch (pick(4)) {
      case 0: {  // garbage run
        const uint32_t n = 1 + pick(24);
        for (uint32_t k = 0; k < n; ++k) stream.push_back(static_cast<uint8_t>(pick(256)));
        break;
      }
      case 1:  // a block cut short (a sender that died mid-write)
        if (pick(2)) {
          const std::size_t cut = 1 + pick(static_cast<uint32_t>(blk.size() - 1));
          stream.insert(stream.end(), blk.begin(), blk.begin() + static_cast<std::ptrdiff_t>(cut));
            break;
        }
        // or a corrupted block (one byte past the header changed)
        { const auto flipMask_ = static_cast<uint8_t>(1 + pick(255)); auto& flipCell_ = blk[3 + pick(static_cast<uint32_t>(blk.size() - 3))]; flipCell_ = static_cast<uint8_t>(flipCell_ ^ flipMask_); }
        stream.insert(stream.end(), blk.begin(), blk.end());
        break;
      default:
        stream.insert(stream.end(), blk.begin(), blk.end());
        good.push_back(blk);
        break;
    }
  }
  std::vector<std::size_t> lost_by_setting;
  for (bool accept_other : {false, true}) {
  BOOST_TEST_CONTEXT("accept_other_categories " << accept_other) {
    Cat021StreamFramer f(Edition::e2_6, accept_other);
    std::vector<Bytes> out;
    std::mt19937 chunks(97);  // the same chunking for both settings
    std::size_t off = 0;
    while (off < stream.size()) {
      const std::size_t n = std::min<std::size_t>(1 + chunks() % 97, stream.size() - off);
      f.feed(stream.data() + off, n, out);
      off += n;
    }
    // Match the good blocks, in stream order, against what came out.
    std::size_t next = 0, lost = 0, lost_bytes = 0, false_bytes = 0;
    for (std::size_t g = 0; g < good.size(); ++g) {
      std::size_t k = next;
      while (k < out.size() && out[k] != good[g]) ++k;
      if (k < out.size()) {
        for (std::size_t u = next; u < k; ++u) false_bytes += out[u].size();
        next = k + 1;
      } else {
        ++lost;
        lost_bytes += good[g].size();
      }
    }
    for (std::size_t u = next; u < out.size(); ++u) false_bytes += out[u].size();
    BOOST_TEST_MESSAGE("accept_other " << accept_other << ": good " << good.size() << " out " << out.size() << " lost " << lost
                               << " runs " << f.droppedRuns() << " dropped " << f.droppedBytes()
                               << " other " << f.otherCategoryBlocks() << " skipped " << f.skippedBytes()
                               << " false_bytes "
                               << false_bytes);
    // Where the bytes of lost good blocks went. Only three things consume input:
    // a counted dropped byte, a counted in-sync other-category skip, and an
    // emitted block. A lost block's bytes that were emitted belong to emitted
    // blocks that are not good blocks (damage that happened to walk cleanly).
    // So this is exact accounting, not a tolerance: nothing else may lose data.
    BOOST_CHECK_LE(lost_bytes, f.droppedBytes() + f.skippedBytes() + false_bytes);
    // And every input byte is accounted for (the rest is the buffered tail).
    std::size_t out_bytes = 0;
    for (const Bytes& b : out) out_bytes += b.size();
    BOOST_CHECK_LE(out_bytes + f.droppedBytes() + f.skippedBytes(), stream.size());
    BOOST_CHECK_LT(stream.size() - (out_bytes + f.droppedBytes() + f.skippedBytes()), 65536u);
    // Corrupted or cut blocks that still walk cleanly are passed (they decode).
    BOOST_CHECK_GT(false_bytes, 0u);
    lost_by_setting.push_back(lost);
    if (!accept_other) {
      BOOST_CHECK_EQUAL(f.otherCategoryBlocks(), 0u);
      BOOST_CHECK_EQUAL(f.skippedBytes(), 0u);
    }
    BOOST_CHECK_GT(f.droppedRuns(), 0u);
    for (const Bytes& b : out) {
      const DecodedBlock d = decode(b);
      BOOST_CHECK(!d.error);
      BOOST_CHECK(!d.records.empty());
    }
  }
  }
  // A CAT 21-only feed loses far fewer good blocks when other CATs are garbage.
  BOOST_CHECK_LT(lost_by_setting[0] * 3, lost_by_setting[1]);
}


BOOST_AUTO_TEST_CASE(unknown_compound_subfield_is_error) {
  const std::vector<std::pair<int, Bytes>> cases = {
      {31, hex("08")},           // 220: b4 is spare
      {34, hex("20")},           // 110: b6 is spare
      {42, hex("01 01 01 20")},  // 295: subfield 24 does not exist
  };
  for (const auto& c : cases) {
    BOOST_TEST_CONTEXT("frn " << c.first) {
      const DecodedBlock d = decode(block(join({fspec({1, c.first}), hex("01 02"), c.second})));
      BOOST_CHECK(d.error);
      BOOST_CHECK(d.records.empty());
      BOOST_CHECK(d.error_text.find("compound subfield") != std::string::npos);
    }
  }
}

BOOST_AUTO_TEST_CASE(explicit_len_zero_is_error) {
  for (int frn : {48, 49}) {
    BOOST_TEST_CONTEXT("frn " << frn) {
      const DecodedBlock d = decode(block(join({fspec({1, frn}), hex("01 02"), hex("00")})));
      BOOST_CHECK(d.error);
      BOOST_CHECK(d.records.empty());
    }
  }
}

BOOST_AUTO_TEST_CASE(repetitive_and_explicit_past_len_are_errors) {
  // 250 REP 5 needs 40 octets, 8 remain.
  BOOST_CHECK(decode(block(join({fspec({39}), hex("05"), Bytes(8, 0x11)}))).error);
  // RE LEN 9, 3 octets remain.
  BOOST_CHECK(decode(block(join({fspec({48}), hex("09 08 70")}))).error);
  // A good record before the bad one is kept.
  const DecodedBlock d =
      decode(block(join({fspec({1}), hex("01 02"), fspec({39}), hex("02"), Bytes(15, 0)})));
  BOOST_CHECK(d.error);
  BOOST_CHECK_EQUAL(d.records.size(), 1u);
}

BOOST_AUTO_TEST_CASE(decode_rejects_other_category_and_empty_block) {
  Bytes blk = encodeBlock({icdRecord()});
  blk[0] = 48;
  BOOST_CHECK(decode(blk).error);
  const DecodedBlock d = decode(hex("15 00 03"));
  BOOST_CHECK(d.error);
  BOOST_CHECK(d.records.empty());
  BOOST_CHECK(decode(hex("15 00")).error);
}

// Out of sync (after a dropped `00`), a good block that arrives in pieces is
// waited for and then passed out; the walk resumes across feeds.
BOOST_AUTO_TEST_CASE(framer_out_of_sync_block_across_feeds) {
  Cat021Record two = icdRecord();
  const Bytes a = encodeBlock({two, two, two});
  const Bytes stream = join({hex("00"), a});
  Cat021StreamFramer f(Edition::e2_6);
  std::vector<Bytes> out;
  const std::size_t cuts[] = {4, 5, 30, 60, 110, 140};
  std::size_t off = 0;
  for (std::size_t c : cuts) {
    f.feed(stream.data() + off, c - off, out);
    off = c;
    BOOST_CHECK(out.empty());
    BOOST_CHECK(!f.inSync());
  }
  f.feed(stream.data() + off, stream.size() - off, out);
  BOOST_REQUIRE_EQUAL(out.size(), 1u);
  BOOST_CHECK(out[0] == a);
  BOOST_CHECK_EQUAL(f.droppedRuns(), 1u);
  BOOST_CHECK_EQUAL(f.droppedBytes(), 1u);
  BOOST_CHECK(f.inSync());
}

// In sync, a CAT 21 header whose buffered prefix already fails the walk is
// dropped at once instead of waited for. After A comes `15 00 ff` (LEN 255);
// the next bytes are A's again: FSPEC 15 00 (FRN 4, 6), 015 = 35, 130 = c7 11
// 3b 69 c1 80, then A's `00` (SAC) as an FSPEC with no item: error after only
// 12 buffered bytes. `00` and `ff` are then dropped out of sync.
BOOST_AUTO_TEST_CASE(framer_in_sync_bad_prefix_drops_at_once) {
  const Bytes a = encodeBlock({icdRecord()});
  const Bytes stream = join({a, hex("15 00 ff"), a, a});
  Cat021StreamFramer f(Edition::e2_6);
  std::vector<Bytes> out;
  const std::size_t first = a.size() + 3 + 20;  // far less than LEN 255
  f.feed(stream.data(), first, out);
  BOOST_CHECK_EQUAL(out.size(), 1u);
  BOOST_CHECK_EQUAL(f.droppedBytes(), 3u);
  BOOST_CHECK_EQUAL(f.droppedRuns(), 1u);
  f.feed(stream.data() + first, stream.size() - first, out);
  BOOST_CHECK_EQUAL(out.size(), 3u);
  BOOST_CHECK_EQUAL(f.droppedBytes(), 3u);
  BOOST_CHECK(f.inSync());
}

// With accept_other_categories, other-category blocks are skipped in sync only
// up to LEN 4096.
BOOST_AUTO_TEST_CASE(framer_other_category_len_limit) {
  const Bytes a = encodeBlock({icdRecord()});
  {
    Bytes cat48 = hex("30 10 00");  // LEN 4096: skipped
    cat48.resize(4096, 0x15);
    const Bytes stream = join({a, cat48, a});
    Cat021StreamFramer f(Edition::e2_6, true);
    std::vector<Bytes> out;
    f.feed(stream.data(), stream.size(), out);
    BOOST_CHECK_EQUAL(out.size(), 2u);
    BOOST_CHECK_EQUAL(f.otherCategoryBlocks(), 1u);
    BOOST_CHECK_EQUAL(f.skippedBytes(), 4096u);
    BOOST_CHECK_EQUAL(f.droppedBytes(), 0u);
  }
  {
    // LEN 4097: drop `30` and leave sync; `10 01` are dropped out of sync.
    const Bytes stream = join({a, hex("30 10 01"), a});
    Cat021StreamFramer f(Edition::e2_6, true);
    std::vector<Bytes> out;
    f.feed(stream.data(), stream.size(), out);
    BOOST_CHECK_EQUAL(out.size(), 2u);
    BOOST_CHECK_EQUAL(f.otherCategoryBlocks(), 0u);
    BOOST_CHECK_EQUAL(f.droppedRuns(), 1u);
    BOOST_CHECK_EQUAL(f.droppedBytes(), 3u);
    BOOST_CHECK(f.inSync());
  }
}

// Endless garbage that always looks like the start of a CAT 21 block of the
// largest LEN: the framer holds at most one block's bytes, never the stream.
BOOST_AUTO_TEST_CASE(framer_buffer_bounded_on_garbage) {
  for (bool other : {false, true}) {
    Cat021StreamFramer f(Edition::e2_6, other);
    std::vector<Bytes> out;
    Bytes junk(4096);
    for (std::size_t i = 0; i < junk.size(); ++i)
      junk[i] = static_cast<uint8_t>(i % 3 == 0 ? 0x15 : 0xff);  // CAT 21, LEN 0xffff, again and again
    for (int round = 0; round < 256; ++round) {  // 1 MiB
      f.feed(junk.data(), junk.size(), out);
      BOOST_TEST(f.buffered() <= 65535u);
    }
  }
}
