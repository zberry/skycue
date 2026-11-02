#include "fisb/global_block.hpp"

// Layouts follow docs/fisb-reference.md §5, which documents fisb-decode's
// level0/apdu_global_block.py and level2/msgBlock.py, utilities.py
// (BSD-2-Clause Plus Patent, see NOTICE) and, for structure only,
// mutability's extract_nexrad.c (GPL; nothing copied). Reimplemented.

namespace skycued {
namespace fisb {

namespace {

constexpr uint32_t kRowBlocks = 450;          // high-res blocks per latitude ring (§5.2)
constexpr uint32_t kHighLatBlock = 405000;    // 900 * 450: first block at 60°N (§5.2, §10.4)
constexpr uint32_t kMaxNorthArcmin = 90 * 60;

// Scale factor -> lattice divisor and block-number offset. FD
// msgBlock.py:93-104 (reference §5.2): row, col = divmod(bn - off, off + 450),
// col /= div, with (off, div) = (0, 1) high, (1800, 5) medium, (3600, 9) low.
constexpr uint32_t kDiv[3] = {1, 5, 9};
constexpr uint32_t kOff[3] = {0, 1800, 3600};

bool isNexrad(uint16_t pid) { return pid == 63 || pid == 64; }
bool hasAltitudeCode(uint16_t pid) { return pid == 70 || pid == 71 || pid == 90 || pid == 91; }

// Where a valid block sits in the high-res numbering, for the empty-list walk.
struct Lattice {
    uint32_t row_start;  // block number of high-res column 0 on this row
    uint32_t index;      // column index on this scale's lattice
    uint32_t count;      // lattice columns per row
    uint32_t step;       // block numbers per lattice column
};

// Validates bn against its scale's lattice and returns its placement; the
// high-latitude doubling (§5.2: 96' blocks, even numbers only, [U]) applies
// when high_lat.
std::optional<Lattice> lattice(uint32_t bn, uint8_t scale, bool high_lat)
{
    if (scale > 2)
        return std::nullopt;  // 3 = reserved (§5.1)
    const uint32_t div = kDiv[scale], off = kOff[scale];
    if (bn < off)
        return std::nullopt;
    const uint32_t r = (bn - off) % (off + kRowBlocks);
    if (r % div != 0 || r >= kRowBlocks)
        return std::nullopt;  // not on the medium/low lattice (tg30: none, §5.2 [V])
    // off and off + 450 are multiples of 450, so r is also bn's high-res
    // column (bn % 450).
    const uint32_t step = high_lat ? 2 * div : div;
    return Lattice{bn - r, r / step, kRowBlocks / step, step};
}

}  // namespace

std::optional<BlockGeom> blockGeometry(uint32_t bn, uint8_t scale, bool experimental_high_lat)
{
    // At or above 60°N for every scale: medium row k >= 180 starts at block
    // 406800, low row k >= 100 at 408600, and the last row below 60°N ends
    // under 405000 for both. So bn >= 405000 is the test (§5.2).
    const bool high_lat = bn >= kHighLatBlock;
    if (high_lat && !experimental_high_lat)
        return std::nullopt;
    auto lat = lattice(bn, scale, high_lat);
    if (!lat)
        return std::nullopt;
    const uint32_t div = kDiv[scale];
    // North edge: the top of the high-res row holding bn, (bn/450 + 1) * 4'.
    // A medium/low block spans div high-res rows downward from there. This
    // is MUT's construction (extract_nexrad.c:126-140) and equals FD's
    // [k * 4' * div, (k+1) * 4' * div) for on-lattice blocks (§5.2).
    const uint32_t north = (bn / kRowBlocks + 1) * 4;
    if (north > kMaxNorthArcmin)
        return std::nullopt;
    // West edge: high-res column * 48' east of Greenwich (§5.2); at >= 60°N
    // the column is rounded down to the doubled lattice (MUT bn &= ~1,
    // :121-124, [U]).
    const uint32_t col = lat->index * lat->step;
    double west = col * 48.0 / 60.0;
    if (west > 180.0)
        west -= 360.0;
    // Bin size (§5.2 table; FD utilities.py:1242-1276): height 1'/5'/9',
    // width 1.5'/7.5'/13.5', doubled at >= 60°N.
    const double dlat = div / 60.0;
    const double dlon = 1.5 * div * (high_lat ? 2 : 1) / 60.0;
    return BlockGeom{north / 60.0, west, dlat, dlon};
}

namespace {

// Run decoders (§5.3, §5.5). Each fills exactly 128 bins from p[3..n) or
// fails; a run past 128, a truncated run or unused bytes all fail.
class Runs {
public:
    Runs(const uint8_t* p, std::size_t n) : p_(p), n_(n) {}

    bool done() const { return total_ == 128; }
    bool more(std::size_t k) const { return pos_ + k <= n_; }
    uint8_t byte(std::size_t k = 0) const { return p_[pos_ + k]; }
    void advance(std::size_t k) { pos_ += k; }

    bool add(uint32_t count, uint8_t value)
    {
        if (count == 0 || total_ + count > 128)
            return false;
        for (uint32_t i = 0; i < count; ++i)
            bins_[total_++] = value;
        return true;
    }

    // All 128 bins and every byte used.
    bool complete() const { return total_ == 128 && pos_ == n_; }
    const std::array<uint8_t, 128>& bins() const { return bins_; }

private:
    const uint8_t* p_;
    std::size_t n_;
    std::size_t pos_ = 3;
    uint32_t total_ = 0;
    std::array<uint8_t, 128> bins_{};
};

bool decodeRuns(uint16_t pid, Runs& r)
{
    while (!r.done()) {
        if (!r.more(1))
            return false;
        const uint8_t b = r.byte();
        uint32_t count;
        uint8_t value;
        if (isNexrad(pid)) {
            // RRRRRIII: run (b>>3)+1, intensity b&7 (§5.3; FD nextradRL).
            count = (b >> 3) + 1u;
            value = b & 7;
            r.advance(1);
        } else if (pid == 103) {
            // NNNNPSSS: N+1 bins, value = P and S (low nibble), 8 -> 0;
            // 0xF8 is 32 bins (§5.5; FD lightningRL :304-313,365-372).
            count = b == 0xF8 ? 32u : (b >> 4) + 1u;
            value = b & 0x0F;
            if (value == 8)
                value = 0;
            r.advance(1);
        } else if (pid == 70 || pid == 71) {
            // count byte + 1, then the ddsssppp value byte (§5.5; FD icingRL).
            if (!r.more(2))
                return false;
            count = b + 1u;
            value = r.byte(1);
            r.advance(2);
        } else {
            // 84, 90, 91: CCCCVVVV, CCCC == 0xE takes the count from the next
            // byte + 1 (§5.5; FD turbRL).
            value = b & 0x0F;
            if ((b >> 4) == 0x0E) {
                if (!r.more(2))
                    return false;
                count = r.byte(1) + 1u;
                r.advance(2);
            } else {
                count = (b >> 4) + 1u;
                r.advance(1);
            }
        }
        if (!r.add(count, value))
            return false;
    }
    return r.complete();
}

}  // namespace

std::optional<DecodedBlocks> parseGlobalBlockProduct(const ApduHeader& hdr, const uint8_t* p, std::size_t n,
                                                     bool experimental, GlobalBlockStats* stats)
{
    GlobalBlockStats local;
    GlobalBlockStats& st = stats ? *stats : local;
    const uint16_t pid = hdr.product_id;
    if (productFamily(pid) != ProductFamily::global_block)
        return std::nullopt;
    auto reject = [&]() -> std::optional<DecodedBlocks> {
        ++st.rejected;
        return std::nullopt;
    };
    if (p == nullptr || n < 4)
        return reject();  // 3-byte block reference + at least one data byte

    // Block reference indicator (§5.1): E, then NS/SF or the altitude code.
    DecodedBlocks out;
    out.product_id = pid;
    out.conus = pid == 64;
    const bool rle = (p[0] & 0x80) != 0;
    const uint32_t bn = (static_cast<uint32_t>(p[0] & 0x0F) << 16) | (static_cast<uint32_t>(p[1]) << 8) | p[2];
    if (hasAltitudeCode(pid)) {
        // Implicitly medium scale, northern hemisphere (§5.1; FD :84-102).
        out.scale = 1;
        out.altitude_code = static_cast<uint8_t>((p[0] >> 4) & 7);
    } else {
        if (p[0] & 0x40)
            return reject();  // southern hemisphere: untested (§5.2)
        out.scale = static_cast<uint8_t>((p[0] >> 4) & 3);
    }

    const bool high_lat = bn >= kHighLatBlock;
    if (high_lat && !experimental) {
        ++st.high_lat_skipped;  // §10.4 [U]: neither sample has data there
        return std::nullopt;
    }
    auto geom = blockGeometry(bn, out.scale, experimental);
    if (!geom)
        return reject();  // reserved scale, off-lattice, beyond 90°N

    if (rle) {
        Runs r(p, n);
        if (!decodeRuns(pid, r))
            return reject();
        RadarBlock b;
        b.block = bn;
        b.lat0 = geom->lat0;
        b.lon0 = geom->lon0;
        b.dlat = geom->dlat;
        b.dlon = geom->dlon;
        b.bins = r.bins();
        out.blocks.push_back(b);
        ++st.rle_blocks;
        return out;
    }

    // Empty-block list (§5.4): byte 3 = bits for b+4..b+1 in its high nibble
    // (bit4 = b+1) and L in its low nibble; then L bytes, LSB first, for
    // b+5, b+6, ... The header block is always empty. Read all L bytes
    // (MUT/STX skip the last one and emit nothing for L = 0).
    const std::size_t len = p[3] & 0x0F;
    if (n != 4 + len)
        return reject();
    auto lat = lattice(bn, out.scale, high_lat);
    if (!lat)
        return reject();  // cannot happen once blockGeometry accepted bn
    // Offsets wrap within the latitude row (§5.4, §10.3; MUT :213-240) and
    // step by the scale's lattice spacing (FD msgBlock.py:214-251: 5 medium,
    // 9 low). At >= 60°N the doubled lattice is used ([U]).
    auto blockAt = [&](uint32_t offset) {
        return lat->row_start + ((lat->index + offset) % lat->count) * lat->step;
    };
    out.empty_blocks.push_back(blockAt(0));  // == bn except an odd number at >= 60°N
    for (unsigned bit = 0; bit < 4; ++bit)
        if (p[3] & (0x10u << bit))
            out.empty_blocks.push_back(blockAt(1 + bit));
    for (std::size_t i = 0; i < len; ++i)
        for (unsigned bit = 0; bit < 8; ++bit)
            if (p[4 + i] & (1u << bit))
                out.empty_blocks.push_back(blockAt(static_cast<uint32_t>(5 + 8 * i + bit)));
    ++st.empty_lists;
    if (len == 0)
        ++st.empty_lists_l0;
    st.empty_blocks += out.empty_blocks.size();
    return out;
}

}  // namespace fisb
}  // namespace skycued
