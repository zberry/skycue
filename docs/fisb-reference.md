# FIS-B decoding reference (UAT 978 MHz ground uplink → products)

Scope: turning dump978 `+<hex>;` uplink lines (432 bytes after FEC) into NEXRAD imagery, text
weather, NOTAM/TFR text and shapes, SUA, and other products. Target: C++17 + Boost, offline.

RTCA DO-282B (UAT MOPS) and DO-358B (FIS-B MOPS) are paywalled and were **not read**. Every
layout here comes from reading open implementations, then checking it against real frames
(the sample file and fisb-decode's `tg30.978`). Each claim gives its source. **[V]** means the
claim was verified by decoding data. **[U]** means it could not be verified, or the sources
disagree.

Sources:

| Tag | Repo | Language | Licence (read from LICENSE) |
|---|---|---|---|
| MUT | https://github.com/mutability/dump978 (`uat_decode.c/.h`, `extract_nexrad.c`, `uat2text.c`) | C | **GPL-2.0-or-later**, not BSD-2 (file headers say "GNU General Public License ... version 2 ... or any later") |
| FA  | https://github.com/flightaware/dump978 | C++ (Boost) | BSD-2-Clause (© Oliver Jowett 2015, FlightAware 2019) |
| STX | https://github.com/cyoung/stratux `uatparse/` | Go | BSD-3-Clause (© Christopher Young) |
| FD  | https://github.com/rand-projects/fisb-decode | Python 3 | BSD-2-Clause Plus Patent (© Michael Barnes 2020-21); "roughly follows DO-358B" (README.MD:6) |

Licence consequence: MUT is GPL. Use it as a structural reference and mirror its field names,
but do not copy its code into a non-GPL codebase. FD (BSD-2+Patent) and STX (BSD-3) are
permissive. FD is the most complete and DO-358B-aware decoder, so treat it as the primary
semantic reference.

---

## 1. Ground uplink frame (DO-282B), 432 bytes

The receiver demodulates and Reed-Solomon-corrects the uplink as 6 interleaved RS blocks of
72 data bytes (576 data bits per block, 6 blocks per frame). Deinterleaving is
`blockdata[i] = raw[i*6 + block]`. The hex that dump978 prints is the deinterleaved 432-byte
payload. The line format is `+<864 hex>;rs=N;rssi=X;t=<unix>.<ms>;`. Older dump978 builds and
the sample print only `+<hex>;`, with **no `t=`** **[V]**.

### 1.1 UAT-specific header (bytes 0-7). Bits are MSB-first; "b5.0" = byte 5, bit 0 (LSB).

| Field | Bits | Extraction (MUT `uat_decode.c:564-586`, STX `uatparse.go:516-543`, FD `ground_uplink_message.py:300-391`) |
|---|---|---|
| GS latitude (23 bits) | b0..b2.1 | `raw_lat = b0<<15 \| b1<<7 \| b2>>1`; `lat = raw*360/2^24`; if `lat>90` then `lat -= 180` |
| GS longitude (24 bits) | b2.0..b5.1 | `raw_lon = (b2&1)<<23 \| b3<<15 \| b4<<7 \| b5>>1`; `lon = raw*360/2^24`; if `>180` then `-= 360` |
| position_valid | b5.0 | `b5 & 0x01`. **Always 0 in practice.** MUT decodes the position anyway (`uat_decode.c:568-571`). FD says it has "never seen it a one", even after DO-358B (`ground_uplink_message.py:302-313`). Sample: 704/704 are 0 **[V]**. Do not gate on it. |
| utc_coupled | b6.7 | `b6 & 0x80` |
| reserved | b6.6 | FD `reserved_7_2` |
| app_data_valid | b6.5 | `b6 & 0x20`. If 0, ignore the application data (MUT `uat_decode.c:588`; FD: "Message should be ignored if this is 0") |
| slot_id (5) | b6.4-0 | `b6 & 0x1f`. Transmission time slot = slot_id+1. MSO = slot_id*22 (FD :352-379) |
| tisb_site_id (4) | b7.7-4 | `b7 >> 4`. Maps to the data-channel block / station tier (FD :385-440, table from SRT-047 rev 5) |
| reserved (4) | b7.3-0 | FD `reserved_8_58` |

Bytes 8..431 are the **424-byte application data**.

### 1.2 Information frames inside the application data
Each frame has a 2-byte header followed by `length` data bytes:
```
byte0: L8 L7 L6 L5 L4 L3 L2 L1     byte1: L0 r r r  T3 T2 T1 T0
length = (byte0<<1) | (byte1>>7)   (9 bits, data bytes only, excludes the 2 header bytes)
reserved = (byte1>>4)&7            (FD: reserved_2_24)
type   = byte1 & 0x0F
```
(MUT `uat_decode.c:598-599`; FD `ground_uplink_message.py:431-438`.)

| Frame type | Meaning | Source |
|---|---|---|
| 0 | FIS-B APDU | all |
| 1 | Reserved for developmental use | MUT `info_frame_type_names` :887-904 |
| 2-13 | Reserved | MUT |
| 14 | **Current Report List (CRL)**. MUT calls it reserved; FD decodes it (`crl_frame.py`) | FD :545 |
| 15 | TIS-B/ADS-R service status | MUT, FD `service_status_frame.py` |

**Iteration and stop rule.** Start with `p = 0` in app_data. Loop while `p+2 <= 424`:
- Read `length` and `type`.
- If `p+2+length > 424`, the frame overruns, so stop.
- If `length == 0`, stop.
- Otherwise the frame data is `[p+2, p+2+length)`. Advance `p += 2+length`.

The implementations differ on the zero-length test. MUT stops only on `length==0 && type==0`
(`uat_decode.c:605`). STX (`uatparse.go:563`) and FD (`ground_uplink_message.py:434`) stop
on `length==0` alone. Zero padding gives 0/0 either way. Use `length==0`, because a zero-length
frame carries nothing. MUT caps the loop at `UPLINK_MAX_INFO_FRAMES = 424/6` (`uat_decode.h:139`).
Frames never span uplinks, so every APDU is wholly inside one 432-byte uplink.

Sample (704 uplinks): frame types {0: 563, 15: 2}. tg30 (963 uplinks): {0: 6169, 14: 17, 15: 46} **[V]**.

Mirror these MUT structs (`uat_decode.h:141-187`):
`struct uat_uplink_mdb { position_valid, utc_coupled, app_data_valid; double lat, lon; uint8_t slot_id, tisb_site_id; uint8_t app_data[424]; unsigned num_info_frames; struct uat_uplink_info_frame info_frames[UPLINK_MAX_INFO_FRAMES]; }`,
`struct uat_uplink_info_frame { is_fisb; uint16_t length; uint8_t type; uint8_t *data; struct fisb_apdu fisb; }`,
`struct fisb_apdu { a_flag, g_flag, p_flag, s_flag, monthday_valid, seconds_valid; uint16_t product_id; uint8_t month, day, hours, minutes, seconds; uint16_t length; uint8_t *data; }`.
Functions: `uat_decode_uplink_mdb()` and `uat_decode_info_frame()` (static, :495).
**Gap:** MUT has no segmentation fields. When `s_flag=1`, its `fisb.data` points at the
segmentation header, which is wrong. Add `product_file_id`, `product_file_length` and
`apdu_number`.

---

## 2. FIS-B APDU header
Bit offsets are from the MSB of APDU byte 0 (FD `apdu_frame.py:178-315` normalises the header to
a 65-bit string; MUT `uat_decode.c:495-562`).

| Bits | Field | Notes |
|---|---|---|
| 0 | A flag | `d0&0x80`. FD: DO-358A says ignore it; DO-358B treats A/G/P as reserved (`apdu_frame.py:255-259`) |
| 1 | G flag | `d0&0x40` |
| 2 | P flag | `d0&0x20` |
| 3-13 | Product ID (11 bits) | `((d0&0x1f)<<6) \| (d1>>2)` |
| 14 | S flag (segmented) | `(d1>>1)&1` |
| 15-16 | T-opt | `((d1&1)<<1) \| (d2>>7)` |
| then | Time fields by T-opt (below) | |
| then, if S=1 | Product file ID (10), product file length (9) = number of segments, APDU number (9) = segment index, **1-based** | 28 bits; FD `apdu_frame.py:312-314`, `Unsegmenter.py:50` |
| | Payload starts at the next byte boundary: `ceil(bits/8)` | FD `apdu_frame.py:241` |

Time fields:

| T-opt | MUT meaning | Fields | Header bits (S=0 / S=1) | Payload offset (S=0 / S=1) |
|---|---|---|---|---|
| 0 | hh:mm | hours 5 (17-21), minutes 6 (22-27) | 28 / 56 | **4 / 7** |
| 1 | hh:mm:ss | + seconds 6 | 34 / 62 | 5 / 8 |
| 2 | mm/dd hh:mm | month 4 (17-20), day 5 (21-25), hours 5 (26-30), minutes 6 (31-36) | 37 / 65 | **5 / 9** |
| 3 | mm/dd hh:mm:ss | + seconds 6 | 43 / 71 | 6 / 9 |

MUT extraction for T-opt 2: `month=(d2&0x78)>>3; day=((d2&7)<<2)|(d3>>6); hours=(d3&0x3e)>>1; minutes=((d3&1)<<5)|(d4>>3)` (:534-537).

**[U] T-opt 1/3 conflict.** FD reads 9 month/day bits whenever `T-opt>=1` and never reads
seconds (`apdu_frame.py:222-231`). MUT and STX treat bit 16 as "seconds present". Only T-opt
0 and 2 appear in the data: sample {0: 424, 2: 139}, tg30 {0: 5199, 2: 970} **[V]**.
Implement 0 and 2. Log and skip 1 and 3.

Which products use which T-opt in the data **[V]**:
- T-opt 2 (month/day): TWGO products (8, 11-17).
- T-opt 0: 413 and the global-block products.

Segmented offsets are confirmed on the sample: a segmented product 8 has payload offset 9,
`(file_id, n_segments, apdu_no) = (739, 3, 1)` and `(739, 3, 2)` **[V]**.

### 2.1 Reassembling segmented products (FD `level1/Unsegmenter.py`)
- Key on `(product_id, product_file_id)` (:45). Keep a vector of `product_file_length` slots.
  Fill slot `apdu_number-1`. Ignore duplicate slots (:88-90). An index past the end is an error (:73).
- When every slot is filled, concatenate: segment 1's whole payload, then each later segment's
  payload **minus its first 6 bytes**. Every segment repeats the 6-byte TWGO header
  (`consolidateFrames` :148-162). Sample check: segment 2's payload starts `2210000000ff`, the same
  TWGO header **[V]**. Decode the result as one TWGO (§6).
- Timeout: drop incomplete sets after `SEGMENT_EXPIRE_TIME = 60` min (`level1Config.py`).
  The source repeats every ~10 min for NOTAMs, so 60 min means about 6 retransmission chances.
- Segmentation seen: sample product 8 only; tg30 products 8 and 14 (G-AIRMET) **[V]**. FD only
  reassembles TWGO. No segmented 413 or global-block APDUs were seen.

---

## 3. Product IDs and payload formats

| ID | Product | Payload family | Seen in sample / tg30 [V] |
|---|---|---|---|
| 8 | NOTAM-D, NOTAM-FDC, NOTAM-TFR, "FIS-B product unavailable" | TWGO, text (fmt 2) and/or graphic (fmt 8) | 64 / 421 |
| 11 | AIRMET | TWGO text + graphic | 2 / 232 |
| 12 | SIGMET, Convective SIGMET (WST) | TWGO text + graphic | 2 / 20 |
| 13 | SUA status (legacy) | TWGO text only, pipe-delimited | 71 / 35 |
| 14 | G-AIRMET | TWGO graphic only, with object qualifiers | 0 / 262 |
| 15 | CWA | TWGO text + graphic | 0 / 0 |
| 16 | NOTAM-TRA | TWGO text + graphic (always both); overlay operator 1 allowed | 0 / 0 |
| 17 | NOTAM-TMOA | as 16 | 0 / 0 |
| 63 | NEXRAD Regional (global block, 8-level) | Global block, scale 0 (high res) | 200 (all empty) / 898 |
| 64 | NEXRAD CONUS | Global block, scale 1 (medium res) | 0 / 1087 |
| 70 / 71 | Icing low / high | Global block, 2-byte runs | 0 / 369, 117 |
| 84 | Cloud tops | Global block, turbulence-style runs | 0 / 714 |
| 90 / 91 | Turbulence low / high | Global block, nibble runs + escape | 0 / 440, 220 |
| 103 | Lightning | Global block, nibble runs | 0 / 100 |
| 413 | Generic text: METAR/SPECI, TAF/TAF.AMD/TAF COR, PIREP, WINDS | Raw DLAC text | 224 / 1254 |

Product family dispatch comes from FD `apdu_frame.py:76-77,140-153`. FD raises
`BadApduProductIdException` for any other ID. MUT `get_fisb_product_name` (:697-754) lists the
older DO-267 catalogue: 0-7/20-27 text, 51-62 NEXRAD variants, 351-353 status, 401-413
generic, 2000-2005 developmental. Handle any other ID as "other product": keep the header,
time and raw bytes, and count it.

Mix of 413 report types (sample): METAR 147, WINDS 35, TAF 29, PIREP 6, TAF.AMD 4, SPECI 3 **[V]**.
Which TWGO products are text only, graphic only, or both is listed in FD `apdu_twgo.py:16-45`.

---

## 4. DLAC 6-bit text
Characters are packed MSB-first, 4 characters per 3 bytes:
`c0=b0>>2; c1=(b0&3)<<4|b1>>4; c2=(b1&0xf)<<2|b2>>6; c3=b2&0x3f`
(MUT `decode_dlac` :650-695; FD `dlacToText` :59-103; STX `dlac_decode` :81-112).

| Code | Char | Code | Char | Code | Char |
|---|---|---|---|---|---|
| 0 | ETX (end of text; 0x03) | 31 | `\|` (DLAC "CC", change cipher, unused) | 44-47 | `, - . /` |
| 1-26 | `A`-`Z` | 32 | space | 48-57 | `0`-`9` |
| 27 | NC null char (MUT emits 0x1A; FD drops it) | 33-37 | `! " # $ %` | 58-63 | `: ; < = > ?` |
| 28 | **TAB**: the next 6-bit code is the number of spaces | 38-43 | `& ' ( ) * +` | | |
| 29 | RS record separator (0x1E) | | | | |
| 30 | LF newline | | | | |

The MUT alphabet string is
`"\x03" "ABCDEFGHIJKLMNOPQRSTUVWXYZ\x1A\t\x1E\n| !\"#$%&'()*+,-./0123456789:;<=>?"` (:648).
FD uses `~` for 0, 27 and 29, then strips every `~` (`utilities.py:22,103`).

TAB handling (all three implementations): on code 28, set `tab=true`. The next code `n` is
not a character: emit `n` spaces and clear the flag. FD's comment says 1-64 spaces, but the
code emits `n` (0-63) spaces. The tab count can straddle the 3-byte group, so the state must
persist across groups. FD has a `DLAC_4BIT_HACK` (`level0Config.py:116`, default False): it
masks the count to 4 bits because "Test groups only seem to use 4 bits rather than 6 for tab"
(`utilities.py:160-162`) **[U]**.

Termination:
- Text ends at ETX (0), and is padded with ETX or NC to the byte boundary.
- The sample texts end with LF then RS (DLAC codes 30, 29, i.e. `"\n\x1e"`) before the ETX
  padding, not `"\n\x03"` as an earlier version of this note said: all 224 of the sample's 413
  texts do **[V]** (`test_fisb_frame`).
- In 413, multiple reports could be split on RS, falling back to ETX (MUT :832-834;
  STX `formatDLACData`).
- In practice one 413 APDU holds one report: FD decodes the whole APDU as one string
  (`apdu_413.py`).
- TWGO text records are delimited by their `record length` field, not by ETX.

Pitfall: when `bytelen` is not a multiple of 3, a naive bit-reader emits a final garbage
character from the padding bits. Stop at ETX or when fewer than 6 bits remain.

---

## 5. Global block representation (63, 64, 70, 71, 84, 90, 91, 103)

### 5.1 Block reference indicator (payload bytes 0-2)
```
byte0: E  NS SF1 SF0 BN19 BN18 BN17 BN16     byte1: BN15..BN8   byte2: BN7..BN0
E  = element id: 1 = run-length block, 0 = empty-block bitmap
NS = 1 for the southern hemisphere (mirror about the equator)
SF = scale factor: 0 high, 1 medium, 2 low, 3 reserved
BN = block number (20 bits) = ((b0&0x0f)<<16)|(b1<<8)|b2
```
(MUT `extract_nexrad.c:143-156`; STX `nexrad.go`; FD `apdu_global_block.py:59-114`.)

For icing and turbulence (70, 71, 90, 91), bits 6-4 are an **altitude code**, not NS/SF. Scale
is implicitly 1 (medium) and the hemisphere is implicitly north (FD :84-102). FD's altitudes:
low `= n*2000 + 2000` ft, high `= n*2000 + 18000` ft (:94-100). The docstring at :486-487 gives
`(n+1)*2000` and `18000 + n*2000` (n 0-3). Both give the same low-level value; the high-level
range differs, so **[U]**. In tg30 the 70/71/90/91 block numbers fall on the medium-res grid
**[V]**.

### 5.2 Block number to geographic area
Facts, all consistent between MUT `block_location` (:61-141) and FD `blockNumberToLatLong` /
`alternateBlockNumber` (`level2/utilities.py:1182-1286`, `msgBlock.py:26-108`):
- High-res base grid: 450 blocks per latitude ring, each 48′ of longitude × 4′ of latitude.
  Block 0 has its SW corner at (0°N, 0°E). Numbers increase eastward, then northward.
  `row = bn / 450`, `col = bn % 450`.
  South edge `= row*4′`, west edge `= col*48′` east of Greenwich (0..360; subtract 360 when > 180).
- At and above 60°N (`bn >= 405000 = 900*450`), blocks are 96′ wide, and only even block
  numbers are used. MUT does `bn &= ~1` (:121-124).
- Each block is 32 (longitude) × 4 (latitude) bins, ordered W→E then N→S from the NW corner
  (MUT :48-51,90-103).
- Bin size:

  | Scale | Bin height | Bin width < 60° | Bin width ≥ 60° | Block span < 60° |
  |---|---|---|---|---|
  | 0 high | 1′ | 1.5′ | 3′ | 4′ × 48′ |
  | 1 medium | 5′ | 7.5′ | 15′ | 20′ × 4° |
  | 2 low | 9′ | 13.5′ | 27′ | 36′ × 7.2° |

  (FD :1242-1276; MUT uses scale ×5 and ×9, :114-130.)
- Medium and low blocks reuse the high-res numbering, on a coarser lattice.
  FD: `row, col = divmod(bn - off, off + 450)`, `col /= div`, where `(off, div) = (1800, 5)`
  for medium and `(3600, 9)` for low (`msgBlock.py:93-104`).
  So a medium block number is `1800 + 2250*k + 5*c`. It covers latitude `[20′k, 20′(k+1))`
  and longitude `[4°c, 4°(c+1))`.
  MUT gets the same box differently: NW corner `= high-res row top`, height `20′`
  (`extract_nexrad.c:126-140`).
  tg30 CONUS 64 blocks satisfy `(row-4)%5==0` and `col%5==0` **[V]**.
- Southern hemisphere (NS=1): MUT mirrors, so `latN = -row*4′` (:134-137). Not relevant to US
  data; untested.

### 5.3 NEXRAD run-length bins (E=1, products 63/64)
Each byte after byte 2 is `RRRRR III`: `run = (b>>3)+1` (1-32), `intensity = b & 7`. Runs
total exactly 128 bins (MUT :175-185; FD `nextradRL` :243-259 raises if more than 128). All
503 regional and 1032 CONUS run-length blocks in tg30 total exactly 128 **[V]**.

Intensity table (FD `apdu_global_block.py:208-229`):

| Level | Regional 63 (dBZ) | CONUS 64 | Description |
|---|---|---|---|
| 0 | < 5 | **no data** | hardly noticeable |
| 1 | 5-19 | < 20 dBZ | very light |
| 2 | 20-29 | same as regional | light to moderate |
| 3 | 30-39 | " | moderate |
| 4 | 40-44 | " | moderate to heavy |
| 5 | 45-49 | " | heavy |
| 6 | 50-54 | " | very heavy |
| 7 | ≥ 55 | " | extreme |

### 5.4 Empty-block bitmap (E=0)
```
byte3: b+4 b+3 b+2 b+1 | L(4)          (high nibble: bit4=b+1 ... bit7=b+4)
byte4..byte3+L: bit0=b+5 ... bit7=b+12, then b+13...  (LSB first within each byte)
payload length = 4 + L                  [V: 1025 empty-block lists in sample+tg30]
```
- The header block `b` itself is **always empty**.
- A set bit means "this block is empty".
- Offsets stay **on the same latitude row**: they wrap modulo the row, they do not just add to
  the block number. MUT: `row_x = (row_offset + 8*i + j - 3) % row_size`, with
  `row_size` 450, or 225 at or above 60° (:213-240).
- FD's `emptyBlockBitmap` (:141-172) returns the bits in this same order.
- Medium scale: FD steps the block number by **5** per bit (9 for low), and by 2 at or above
  405000 for medium (`msgBlock.py:214-251`).

**Bug in MUT and STX:** they loop `for (i=0; i<L; ++i)`, with i=0 being the synthesised
byte 3. They therefore never read the last bitmap byte (`data[3+L]`), and **emit nothing when
L=0**, which is the most common case (415 of 1025). They also step by 1 at every scale. Follow
FD. **[V]** structurally, from payload lengths.

The 1025 and 415 are **lists** (APDUs), and combined across both fixtures: the 2015 sample
alone has 200 lists holding 1342 empty blocks, 56 of the lists with L=0; tg30 has 825 lists
(359 with L=0). `test_fisb_blocks` pins both the per-fixture and the combined figures **[V]**.

What value an empty block gets:
- MUT empirically uses 0 for regional and **1** for CONUS ("valid data, but no precipitation",
  :255-259).
- FD fills 0 for both (`msgBlock.py:210`), but its own table says CONUS 0 = "no data".
- **[U]** Recommended: regional = 0 and CONUS = 1, so an empty CONUS block is not drawn as missing.

### 5.5 Other global-block run-length encodings (FD only; unverified against a spec)
- **Lightning 103** (`lightningRL` :261-400): byte `NNNN P SSS`.
  - `bins = N+1`; P = polarity (1 positive); S = strike count.
  - Strike codes: 0, 1, 2, 3-5, 6-10, 11-15, >15, 7 = no data.
  - Special case: `0xF8` means 32 bins, and is seen as `F8F8F8F8` for an empty block (:304-313,365-372).
  - Value 8 maps to 0.
  - FD treats any total other than 128 as an error.
  - **[U]** Neither fixture carries a lightning run-length block (tg30 has none; the sample has
    no product 103), so the 128-bin sum rule for lightning is FD's, unverified on real data.
    skycue's decoder applies it, as for every global block, and covers it with hand vectors only.
- **Icing 70/71** (`icingRL` :402-479): 2 bytes per run, `count = b0+1`, then the value byte
  `ddsssppp`.
  - dd = SLD: ≤5%, ≤50%, >50%, no data.
  - sss = severity: none, trace, light, moderate, severe, heavy, -, no data.
  - ppp = probability: ≤5, ≤20, ≤30, ≤40, ≤80(?), ≤60(?), >80, no data. The table order is
    odd; check it.
- **Turbulence 90/91 and cloud tops 84** (`turbRL` :481-535): byte `CCCC VVVV`.
  - If `CCCC == 0xE`, then `count = next_byte + 1` (2-byte run); otherwise `count = CCCC+1`.
  - Turbulence V: EDR bins, 0 = <7, 14 = ≥98, 15 = no data.
  - The cloud-top value → altitude mapping was not extracted **[U]**.

### 5.6 Regional (63) vs CONUS (64)
- Scale: tg30 shows 63 always at scale 0 and 64 always at scale 1; the sample's 63 blocks are
  scale 0 **[V]**.
- Level 0: means "<5 dBZ" for 63 and "no data" for 64 (§5.3).
- Coverage: regional is a roughly circular area around the ground station; CONUS is a
  rectangle (FD `document.rst:1124-1129`).
- Expiry: both 75 min (FD `level2Config.py`, citing the standard).
- Image assembly: FD's harvest lets a regional image merge older blocks no more than 10 min
  older than the newest (`document.rst:1118-1121`). It waits `IMAGE_QUIET_SECONDS=10` with no
  new blocks before rendering, to avoid partial frames.
- One image = all blocks sharing the same product ID and APDU hh:mm.

---

## 6. Text With Graphic Overlay (TWGO): products 8, 11-17
All offsets below were validated: on the sample and tg30, every graphic record decoded to
exactly its `record length`, and the polygons land in plausible places (California for the
2015 sample, the Midwest/East for tg30) **[V]**.

### 6.1 TWGO payload header (6 bytes) (FD `apdu_twgo.py:84-105`; STX `decodeAirmet` :252-263)
```
b0: record_format(4) | product_version(4)   format 2 = unformatted DLAC text, 8 = graphical overlay
                                             (STX: 0 none, 1 ASCII, 3 DLAC+dict, 4 ASN.1 — unused)
b1: record_count(4) | reserved(4)
b2-b4: location identifier, DLAC (4 chars); all-ETX for none
b5: record_reference_point; 0x00 = facility, 0xFF = external; FD: ignore other values
```
A text TWGO and a graphic TWGO are **separate APDUs** with the same report number. Pair them
(§6.5).

### 6.2 Text record (format 2) (FD `textRecords` :120-177; STX :285-305)
```
r0-r1: text_record_length (16 bits, includes this 5-byte header)
report_number (14) = (r2<<6) | (r3>>2)
report_year (7)    = ((r3&3)<<5) | (r4>>3)     -- 2 digits; 1 digit for NOTAM-TFR/-D per FD comment [U]
report_status      = (r4>>2)&1                 -- 1 active, 0 cancelled
reserved (2)       = r4&3
text: DLAC, (length-5) bytes
```
**FD bug:** it reads the status from `ba[4]` rather than `ba[ros+4]` (`apdu_twgo.py:162`).
This only matters when there are multiple text records, and FD rejects more than one anyway
(`TwgoMatcher.py:97-99`).
Sample: SUA record length 68, report 9294, year 15, status 1 **[V]**.

### 6.3 Graphic overlay record (format 8) (FD `graphicRecords` :179-508; STX :306-477)
```
overlay_record_length (10) = (g0<<2)|(g1>>6)       -- bytes, whole record
report_number (14)        = ((g1&0x3f)<<8)|g2
report_year (7)           = g3>>1
rec_applicability_start_year (2) = ((g3&1)<<1)|(g4>>7)   } year offsets for NOTAMs (FD :232-240)
rec_applicability_end_year (2)   = (g4>>5)&3             }
overlay_record_id (4)     = ((g4>>1)&0xf)+1
object_label_flag         = g4&1   -> 0: 2-byte numeric label; 1: 9-byte DLAC label (12 chars, e.g. "KSCK")
--- after the label:
flags: element_flag(1) qualifier_flag(1) param_flag(1) object_element(5)
        element: 0 TFR,1 TURB,2 LLWS,3 SFC,4 ICING,5 FRZLVL,6 IFR,7 MTN
type/status: object_type(4) object_status(4)   type 0 airport, 14 airspace; status 13 cancelled, 15 in effect
[object qualifier, 3 bytes]   FD: only if qualifier_flag && product==14 (bitmap, DO-358B Table A-54);
                              STX: whenever qualifier_flag.  [U] qualifier_flag never set elsewhere in the data
[param type(5)+value(11), 2 bytes]  if param_flag; FD: "the entire record should be ignored"
applicability: options(2) date_time_format(2) geometry(4)
        options: 0 none (UFN), 1 start only, 2 end only, 3 both
        dt fmt: 0 none, 1 month+day+hour+min (4 bytes), 2 day+hour+min (3), 3 hour+min (2)
                -- one plain binary byte per field; DO-358B: discard if options!=0 and fmt is 0 or 2 (FD :376-379)
        geometry: 3 polygon MSL, 4 polygon AGL, 7 circular prism MSL, 8 prism AGL,
                  9 point AGL, 10 point MSL, 11 polyline MSL, 12 polyline AGL; 5/6 obsolete ellipses
overlay_operator(2) | vertex_count-1 (6)    operator 0 independent, 1 "dependent, combine" (TRA/TMOA); 2/3 unsupported
[start time][stop time]  per options/fmt
vertices × vertex_count
```
**6-byte vertex** (geometries 3, 4, 9-12), 48 bits (FD `decode6ByteVertex` :510-541):
`lon19 = v0<<11 | v1<<3 | v2>>5`; `lat19 = (v2&0x1f)<<14 | v3<<6 | v4>>2`; `alt10 = (v4&3)<<8 | v5`.
Convert with `deg = raw*360/2^19` (if `>180`, `-360`; for lat, if `>90`, `-180`), and
`alt_ft = alt10*100`. **Longitude comes before latitude.**

**14-byte circular prism** (7, 8), 112 bits (FD `decode14ByteVertex` :543-608):
- 18-bit fields, scaled `360/2^18`, in this order: lon_bot, lat_bot, lon_top, lat_top.
- `z_bot7 = v9>>1`, `z_top7 = (v9&1)<<6 | v10>>2`, both ×500 ft.
- `r_major9 = (v10&3)<<7 | v11>>1`, `r_minor9 = (v11&1)<<8 | v12`, both ×0.2 NM.
- `alpha8 = v13`.
- STX multiplies z_bot by **5** (`uatparse.go:457`). This is a probable STX bug; use 500.
- FD only accepts a simple circle: bot==top, r_major==r_minor, alpha==0 (`level2/utilities.py:1094-1098`).

Examples decoded from the captures **[V]**:
- Sample NOTAM-D KSCK (product 8, report 12045; the sample carries several KSCK graphics, and
  these values are that report's): label "KSCK", type 0/status 15, options 3, fmt 1,
  geometry 9 (point AGL). Start 1/23 16:18, stop 1/24 23:59. Point (37.894, -121.238), 0 ft.
- Sample SIGMET (product 12): type 14/status 15, geometry 3. Window 1/24 02:23-06:23, which
  matches the "VALID UNTIL 240623" text. Closed 6-vertex polygon at 10000 ft.
- Sample AIRMET (product 11): 14 vertices: a 7-point ring at 43000 ft, then the same ring at
  18000 ft. These are top and bottom altitudes, not a 14-point polygon.
- Sample product 8, report 6095: geometry 7 (circular prism MSL), 0-18000 ft, radius 10 NM.
  (An earlier version of this note placed the prism in tg30; tg30's product-8 graphics are
  190 points, geometry 9, and 1 AGL polygon, geometry 4, with no prism.)
- tg30 G-AIRMET (product 14): flags 0x81 (element 1 = TURB).

### 6.4 Geometry post-processing (FD `level2/utilities.py:861-1040`)
- **Polygons with two altitudes:** the ring is repeated, closing back to its first vertex
  each time. Split it at the closure (`polygonAppendCheck` :752-803).
- **More than 64 vertices:** a long polygon or polyline is split across consecutive records
  with the same geometry. Merge them:
  - Polylines share the junction point, so drop the duplicate.
  - Polygons are merged until the ring closes.
  - See `geometryPrePass` :897-944 and `polyAppend`.
- **Circles and points with more than one vertex:** split into one shape per vertex
  (`duplicatePointsAndCircles`).
- **Overlay operator 1 with 2 records (TRA/TMOA):** combine them into one shape. Take the
  altitudes and AGL/MSL type from each (`geometryOverlayOperatorPass` :946-1006).

### 6.5 Identity, pairing, cancellation, updates
- Unique key (FD `TwgoMatcher.py:112-113`):
  `product_id-report_year-report_number-location-month`. Location is needed because NOTAM-D/FDC
  report numbers repeat across locations (`msg8_16_17.py:100-117`).
- Report-number ranges for product 8/16/17 (FD `msg8_16_17.py:84-90`):

  | Range | Product |
  |---|---|
  | 0001-9999 | TFR, FDC |
  | 10000-11999 | FIS-B unavailable |
  | 12001-12999 | NOTAM-D |
  | 13001-15999 | TRA/TMOA, split into SUAE, SUAC and SUAW service areas |

- Pairing rules (FD `TwgoMatcher.py:8-52`):
  - A text part is published at once.
  - A graphic part waits for its text part. Unmatched parts are held for
    `TWGO_EXPIRE_TIME = 720` min (`level1Config.py`).
  - If the text changes, drop the old graphic and republish.
- Cancellation:
  - Text `report_status = 0`, or graphic `object_status = 13`.
  - Publish the cancellation at once and remove the item.
  - FIS-B repeats a cancellation for 2 retransmission intervals. FD keeps it 30 min
    (`level2Config.py CANCEL_EXPIRATION_TIME`).
- NOTAM-TFR alternation:
  - The text is sent only every other cycle. The other cycles carry an **empty-text active
    record**, which is a "renew-only" keep-alive and does not replace the text
    (`msg8_16_17.py:146-151`, `TwgoMatcher.py:151-153`).
  - TFR text is not in standard NOTAM format: regex `^NOTAM-TFR ([0-9]/[0-9]{4}) `, often a
    glob of text containing "(INCMPL)" (:176-183,289-298).
- CRL frames (type 14):
  - These list the reports currently being broadcast, so you can tell when an item has
    disappeared or when you have every part.
  - Header: `product_id(11) = b0<<3|b1>>5`, `tfr_flag = b1.4`, `overflow = b1.1` (more than 138
    entries), `location_flag = b1.0`, `range_nm = b2*5`.
  - If location_flag, a 3-byte DLAC location follows and the count is in b6; otherwise the
    count is in b3.
  - Each entry is 3 bytes: `year_or_month(7) = e0&0x7f` (month for TRA/TMOA), `text_flag = e1.7`,
    `graphic_flag = e1.6`, `report_number(14) = (e1&0x3f)<<8|e2` (FD `crl_frame.py:41-106`).

### 6.6 Text formats seen (decoded from the captures) [V]
- NOTAM-D (product 8): `NOTAM-D KLHM.01/124 230125Z !RIU 01/124 LHM RWY 15 PAPI OUT OF SERVICE 1501230125-1501300122`.
  The validity is `YYMMDDhhmm-YYMMDDhhmm|PERM`, optionally suffixed `EST`. FD regexes are at
  `msg8_16_17.py:18-35`. FD's PERM placeholder is 2038-01-01.
- SUA (product 13), pipe-delimited:
  `SUA 241445 3556903|24878|P|R|3202 LOW|1501241445|1501250130|000|180|A|Y||3202LOW||`.
  Field meanings are not documented in these repos. FD `msg13.py` parses it **[U]**.
- AIRMET and SIGMET text are FAA bulletins, e.g. `SIGMET KSFO 240223 SIGMET NOVEMBER 1 VALID UNTIL 240623 ...`.
- FIS-B unavailable: `FIS-B ddhhmmZ <centers> <product> PRODUCT ...`. It expires 20 min after
  last reception (`msg8_16_17.py:39-42`).

---

## 7. Timing
- **APDU time.** For T-opt 0 the APDU carries only hh:mm. The meaning depends on the product:
  observation or valid time for weather and images, transmission or creation time for TWGO
  **[U]**.
  - FD resolves the date by inserting hh:mm into the receive date. It tries day −1, 0 and +1
    and picks the one closest to the receive time, favouring the past on a tie
    (`iso8601FromApduHourMins`, `level2/utilities.py:242-314`).
  - For T-opt 2 (month/day), it picks the year −1/0/+1 closest to the receive time
    (`componentsToIso8601Referenced` :72-110).
  - In report text, an hour of 24 means 00 of the next day (`case24Hour` :129-154).
  - Year digits: one digit is resolved as 5 years back / 4 forward (`singleDigitYear` :396-448);
    two digits as 50 years back / 49 forward (`doubleDigitYear` :450-497).
- **Receive time.** dump978-fa's `t=` field gives the receive time; tg30 has it on 963/963
  lines **[V]**. **The sample has no `t=`, so there is no receive date at all.** The decoder
  needs an externally supplied reference time (CLI option or file metadata). In the sample it
  can be inferred from the T-opt 2 month/day (all January) and from NOTAM `YYMMDD` dates
  (2015-01).
- "Time of transmission" is not in the APDU. Approximate it with the receive time; the UAT
  frame is in UTC slot `slot_id` within that second.
- Report times inside the text (METAR `ddhhmmZ`, TAF issue time and `ddhh/ddhh` validity,
  PIREP `/TM`) are the authoritative times for those products (FD `msg413.py:20-40` regexes).
- WINDS forecast hour (6, 12 or 24) is derived from the product-available time and the valid
  time using `WIND_MATRIX` (`msg413.py:42-58,300-390`).
- Expiry values used by FD (`level2Config.py`; "Standard says" is FD's attribution, not mine):

  | Product | Expiry |
  |---|---|
  | NEXRAD regional / CONUS | 75 min |
  | Lightning | 75 min |
  | Icing, turbulence, cloud tops | 105 min |
  | METAR | 120 min after observation (no standard value) |
  | TAF | end of its validity period |
  | PIREP | at least 75 min after last reception per the standard; FD default 120 min after report time |
  | TWGO without a stop time | at least 60 min after last reception (FD default 70: TFR text arrives every other 10-min cycle) |
  | Cancellations | 30 min |
  | FIS-B unavailable | 20 min |
  | Service status | 40 s |
  | Level-3 dedup hash | 45 min |

- Transmission and update intervals (e.g. regional NEXRAD every 2.5 min, CONUS every 15 min)
  are in the FAA AIM §7-1 FIS-B table and SRT-047. Those documents were not available when this was written
  (faa.gov refused automated download), so the intervals are **[U]**.

---

## 8. Reference implementations: what each covers

| | Uplink header/frames | APDU header | Segmentation | DLAC | 413 | TWGO | Global block | CRL |
|---|---|---|---|---|---|---|---|---|
| MUT `uat_decode.c`, `uat2text.c` (61 lines, CLI front end), `extract_nexrad.c` | yes | yes (incl. T-opt 1/3) | **no** | yes | text split RS/ETX | no (hex dump) | 63/64 only, with the empty-block bug §5.4 | no |
| FA dump978 (C++/Boost) | **no**: demodulation, FEC, raw hex output, downlink ADS-B decode; `uplink_feedback` is a downlink field | no | no | no | no | no | no | no |
| STX `uatparse` | yes | yes; `s_f` parsed but marked TODO (:53) | no | yes | yes | `decodeAirmet` exists but is **commented out** (:502-505); z_bot×5 bug | 63/64 (same bug as MUT) | no |
| FD | yes | yes (DO-358B) | yes | yes | yes, plus parsing | full, plus geometry passes | all 8 products | yes |

- FD architecture to copy: level0 (bits → dict), level1 (unsegment, TWGO match), level2
  (semantic messages, expiry), level3 (dedup, SHA-224 of message JSON, `Deduplicator.py:41-64`).
- Rust and other C decoders are not covered.
---

## 9. Validation data
- **The fixture `tests/fixtures/raw/uat-sample.txt.gz` is byte-identical to dump978's
  `sample-data.txt.gz`**, in both MUT and FA: same gzip MD5 `28d6b169…`, and 1143 lines
  (704 `+` uplinks, 439 `-` downlinks) **[V]**.
  - Content: one ground station; dates in January 2015, i.e. pre-DO-358B, yet the TWGO
    layouts above decode cleanly.
  - Product counts: 8 = 64, 11 = 2, 12 = 2, 13 = 71, 63 = 200, 413 = 224.
  - **All 200 NEXRAD blocks are empty-block messages.** There are no radar returns, no CONUS
    radar, and nothing from products 14-17, 64, 70+ or 103. One segmented NOTAM (3 segments).
  - Neither repo documents the product mix; these counts come from a survey of the file.
- **FD `tg/tg-source/generated/tg28.978`, `tg29.978`, `tg30.978` (866 KB)** were generated by
  FD's author and carry FD's BSD-2+Patent licence. Format: `+hex;rs=;t=;` with Oct 2020
  timestamps.
  - tg30 has the full product mix (counts in §3), including CRLs and segmented G-AIRMET.
  - They come with **no expected outputs**: `tg/results/` holds only a README. `tg/triggers/*.csv`
    holds check times, e.g. `7290, 10, 1, [4-7] Check test products arrived`, and start dates.
- The RTCA DO-358B test groups TG01-TG27 are free from the RTCA store after registration, but
  cannot be redistributed. They include `.bin` uplinks and a PDF of expected behaviour
  (`document.rst:1700-1730`). FD's notes on individual tests are at `document.rst:1974-2037`.
- **Golden files:** none published. Practical route: run FD offline on `uat-sample` and
  `tg30.978` with `bin/decode` (level 0-3 JSON). FD needs Python and, for harvest/RSR,
  MongoDB; level0-2 do not. Commit the JSON as golden output and attribute it per BSD-2+Patent.
  MUT `uat2text` gives a second, independent golden for headers, 413 text and hex.
- Other documents (not consulted): SRT-047 "SBS Description Document" (FAA TSO-C157/C195 attachments, links in `document.rst:2183-2268`); MITRE "Aerodrome and Airspace FIS-B Product Definitions" Rev 4, STX's TWGO source: https://github.com/cyoung/stratux/blob/master/notes/Aero_FISB_ProdDef_Rev4.pdf.

---

## 10. Pitfalls checklist
1. **Bit alignment:** APDU payload starts at byte 4, 5, 7 or 9, i.e. `ceil(header_bits/8)` (§2). Global-block fields restart at the byte boundary. DLAC tab state carries across 3-byte groups. Information frames never span uplinks.
2. **Segmentation:** ignoring S (as MUT and STX do) misparses the payload by 4 bytes. Key on `(pid, file_id)`, strip the 6-byte TWGO header from segments 2..n, count segments from 1, and expire incomplete sets at 60 min.
3. **Empty-block bitmap:** L counts the bytes *after* byte 3. The header block is implicitly empty. Offsets wrap within the latitude row. At medium scale, step 5 per bit. MUT and STX drop blocks (§5.4).
4. **High latitude (≥ 60°N, block ≥ 405000):** blocks are double width and only even numbers are used. MUT computes `row_size = 225` and then `bn = row_start + row_x`, which gives odd numbers. FD's `normalizeBins` and stepping disagree with MUT. Neither sample has data at or above 60°, so this is **[U]**: get Alaska captures before trusting either.
5. **Longitude:** blocks are 0..360° east (subtract 360 above 180). Bitmap runs wrap at 0° in block space. TWGO 18/19-bit fields use the same >180 rule, and longitude comes **before** latitude.
6. **Resolution:** CONUS bins (scale 1) are 7.5′×5′; regional bins (scale 0) are 1.5′×1′. Resample when making a mosaic, and prefer regional where they overlap.
7. **Level 0:** means different things in 63 and 64. Render empty CONUS blocks as level 1 **[U]**.
8. **TWGO:**
   - Text and graphics come in separate APDUs.
   - TFR text arrives every other cycle, with empty keep-alives in between.
   - Polygons carry top and bottom altitude rings.
   - Shapes over 64 vertices span several records.
   - The qualifier is consumed only for product 14 **[U]**.
   - Overlay operators 2 and 3 are unsupported.
   - Discard records whose time format is 0 or 2 when times are present.
9. **Years:** TWGO years are 7 bits (a single digit for some NOTAMs), and NOTAM start/end year offsets are 2 bits. Resolve them relative to the receive time (§7).
10. **Duplicates:**
    - The same products repeat every 2.5-15 min and come from several stations.
    - Dedupe level-2 output by content hash (FD keeps the hash 45 min).
    - Key TWGO by `pid/year/report/location/month`.
    - Key NEXRAD by `pid/hh:mm/block`, with the latest message winning.
11. **Stuck messages:** FD's author saw two WST items broadcast for over a year (`document.rst:2038-2047`). Sanity-check dates.
12. **Header flags:** skip uplinks with `app_data_valid = 0`. `position_valid` is always 0, so decode the station position anyway.
13. **Licensing:** MUT is GPL-2+, so reimplement from this spec rather than copying its code. FD (BSD-2+Patent) and STX (BSD-3) need attribution if you port their code or test vectors.

