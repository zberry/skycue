# ASTERIX Category 021 (ADS-B Target Reports) decoding reference

Scope: decoding EUROCONTROL ASTERIX CAT021 data blocks arriving over UDP (or TCP, §6) into skycued
`TrackObservation`s. Target: C++17 + Boost, offline. Primary edition: **2.6** (2021-12-21).
Compiled 2026-11-02.

**Source situation.** Every eurocontrol.int URL (the CAT021 PDFs for ed. 2.5, 2.6, 2.7 and 1.8,
and the Part 1 publication page) answered **HTTP 403** to automated download on 2026-11-02, so
**no CAT021 PDF was read**. The layouts below come from **asterix-specs** (BSD-3-Clause), a
machine-readable transcription of the EUROCONTROL PDFs, one file per edition. They were
cross-checked against the CroatiaControl `asterix` decoder XML configs (GPL), which were read but
not copied, and against the decoded bytes of the `cat21_re.ast` sample (§7). Tags:

- **[V]**: read in a EUROCONTROL PDF. The only one that downloaded was the CAT001 standard
  (Part 2a ed. 1.1, 2002, hosted by ICAO), and it confirms only the data-block layout in §1.
- **[T]**: read in the asterix-specs transcription of that edition (`cat-<ed>.ast`,
  `ref-<ed>.ast`). Under the "[V] = PDF only" rule this **counts as unverified**. It is a much
  stronger source than a guess, though, because each file follows its PDF item by item.
  **[T+S]** means the sample bytes decode consistently with it.
- **[U]**: inferred, from general knowledge (DO-260, ICAO Annex 10), or only from the GPL XML.

This reference summarises the public EUROCONTROL CAT021 standard. §8 lists what the pingStation 3
emits, as the project's decoder and encoder model it; the device is not a source for any bit layout.

Fetched files are in the session scratchpad (`asterix/specs`, `asterix/cc`, `asterix/pdf`).
They are data only. Nothing from the GPL XML is reproduced here.

Bit numbering follows ASTERIX convention: in an N-bit field, bit N is the MSB and bit 1 the LSB,
and bytes go on the wire MSB first (big-endian). "b8" in a one-octet table means the octet's
MSB. In FX-extended items, every octet's bit 1 is **FX** (1 = another octet follows).

---

## 1. Data block and record structure

```
Data block:  CAT (1 octet) | LEN (2 octets, big-endian) | record 1 | record 2 | ... | record n
Record:      FSPEC (1..k octets) | data fields in FRN order (only those whose FSPEC bit is 1)
```

- **CAT** = 21 (0x15) for CAT021 **[V]** (layout from CAT001 Part 2a §4.2.2.2, which has CAT=001).
- **LEN** is the total length of the data block in octets, **including the CAT and LEN fields**
  **[V]**. A block with records totalling R octets has LEN = R + 3. The sample
  shows LEN=0x002C with 44 octets **[T+S]**. A one-record block generated with `skycue-pingsim`
  (tests/test_cat021.cpp) has LEN = 51: a 48-octet record.
- A data block holds one or more records of the **same category** **[V]** (Part 2a §3.1.3).
  Records have no length field and no separator: each record ends where its last present field
  ends, and the next record's FSPEC starts at the following octet.
- **FSPEC**: each octet carries 7 presence bits (b8..b2 = FRN 7k+1..7k+7) and an FX bit (b1).
  FX=1 means another FSPEC octet follows. Fields are sent in **FRN order** **[V]** (Part 2a
  §4.3.1). The CAT021 ed. 2.x UAP has 49 FRNs, so the FSPEC is at most 7 octets.
- **UAP** (User Application Profile): the table that maps FRN to data item **[V]**
  (Part 2a §3.1.9). CAT021 has a single UAP in every edition.
- **Field length rules** (Part 1 concepts, as used by asterix-specs) **[T]**:
  - *Fixed*: N octets.
  - *Extended* (variable): octets are read until one has FX=0. Each octet's b1 is FX.
  - *Repetitive*: a 1-octet REP count, then REP × (fixed sub-item length).
  - *Compound*: a primary subfield (an FSPEC-like bitmap: 7 presence bits + FX per octet), then
    each present secondary subfield in order. Each secondary subfield has its own length rule.
  - *Explicit* (RE, SP): the first octet is a length that **includes itself**. The field occupies
    LEN octets in total. In the sample, RE = `05 08 f0 01 62` (LEN=5) **[T+S]**.
- **An unknown FRN cannot be skipped.** Nothing in a record gives a field's length except the
  item's own length rule. A decoder that meets a set FSPEC bit for an item it does not know how
  to measure loses sync for the rest of the **data block**. Consequences:
  1. The decoder must implement the length rule of **every** item in the UAP of the configured
     edition, even items it never interprets (110, 220, 250, 260, 271, 295, ...).
  2. On any length inconsistency (a field runs past LEN, or the records do not end exactly at
     LEN), discard the **rest of the block**, not only the record. Count it in stats.
  3. The configured edition must be right (§4): a wrong UAP gives garbage, not an error.
  4. Explicit items (RE, SP) can always be skipped by their length octet, even when their
     contents are not understood.

## 2. UAP, edition 2.6 (identical FRN→item table in 1.8 [U, XML], 2.1–2.7 [T])

| FRN | Item | Name | Length rule (octets) |
|---|---|---|---|
| 1 | 010 | Data Source Identification | 2 |
| 2 | 040 | Target Report Descriptor | extended, 1 + FX extensions (up to 5 in 2.6) |
| 3 | 161 | Track Number | 2 |
| 4 | 015 | Service Identification | 1 |
| 5 | 071 | Time of Applicability for Position | 3 |
| 6 | 130 | Position in WGS-84 Co-ordinates | 6 |
| 7 | 131 | High-Resolution Position in WGS-84 | 8 |
| — | FX | (end of FSPEC octet 1) | |
| 8 | 072 | Time of Applicability for Velocity | 3 |
| 9 | 150 | Air Speed | 2 |
| 10 | 151 | True Airspeed | 2 |
| 11 | 080 | Target Address | 3 |
| 12 | 073 | Time of Message Reception for Position | 3 |
| 13 | 074 | Time of Message Reception of Position, High Precision | 4 |
| 14 | 075 | Time of Message Reception for Velocity | 3 |
| 15 | 076 | Time of Message Reception of Velocity, High Precision | 4 |
| 16 | 140 | Geometric Height | 2 |
| 17 | 090 | Quality Indicators | extended, 1 + FX extensions (up to 4 in 2.6, 8 in 2.7) |
| 18 | 210 | MOPS Version | 1 |
| 19 | 070 | Mode 3/A Code | 2 |
| 20 | 230 | Roll Angle | 2 |
| 21 | 145 | Flight Level | 2 |
| 22 | 152 | Magnetic Heading | 2 |
| 23 | 200 | Target Status | 1 |
| 24 | 155 | Barometric Vertical Rate | 2 |
| 25 | 157 | Geometric Vertical Rate | 2 |
| 26 | 160 | Airborne Ground Vector | 4 |
| 27 | 165 | Track Angle Rate | 2 |
| 28 | 077 | Time of ASTERIX Report Transmission | 3 |
| 29 | 170 | Target Identification | 6 |
| 30 | 020 | Emitter Category | 1 |
| 31 | 220 | Met Information | compound: primary (1 octet) + WS 2, WD 2, TMP 2, TRB 1 |
| 32 | 146 | Selected Altitude | 2 |
| 33 | 148 | Final State Selected Altitude | 2 |
| 34 | 110 | Trajectory Intent | compound: primary (1) + TIS (extended, ≥1) + TID (REP + REP×15) |
| 35 | 016 | Service Management | 1 |
| 36 | 008 | Aircraft Operational Status | 1 |
| 37 | 271 | Surface Capabilities and Characteristics | extended, 1 + FX extensions (2.2+). **Ed. ≤2.1: 1 or 2 octets, see §3.36** |
| 38 | 132 | Message Amplitude | 1 |
| 39 | 250 | Mode S MB Data | repetitive: REP + REP×8 |
| 40 | 260 | ACAS Resolution Advisory Report | 7 |
| 41 | 400 | Receiver ID | 1 |
| 42 | 295 | Data Ages | compound: primary (1..4 octets, 23 subfields) + 1 octet per present subfield |
| 43–47 | — | spare (must be 0) | — |
| 48 | RE | Reserved Expansion Field | explicit (LEN octet includes itself) |
| 49 | SP | Special Purpose Field | explicit |

All rows are [T]. The 2.7 UAP is identical [T]. The FRN↔item mapping of the 23 items the
pingStation 3 emits (FRN 1, 2, 6, 7, 9, 10, 11, 12, 14, 16, 17, 18, 19, 21, 22, 23, 24, 25, 26,
28, 29, 30, 36; §8) follows the ed. 2.5 UAP [T]. The length rules of 010, 040, 130, 131,
080, 090, 210, 070, 145, 200, 155, 160, 170, 020 and 008 add up to the length of a record the
project's encoder writes for those items [T+S].

## 3. Data items (ed. 2.6), bit layouts. All [T] unless marked.

Notation: `sN` is an N-bit two's-complement signed field, `uN` unsigned.

**3.1 I021/010 Data Source Identification (2).** Octet 1 = SAC, octet 2 = SIC (raw). Sample:
`00 01` → SAC 0, SIC 1 [T+S].

**3.2 I021/040 Target Report Descriptor (extended).**

| Octet | b8 | b7 | b6 | b5 | b4 | b3 | b2 | b1 |
|---|---|---|---|---|---|---|---|---|
| primary | ATP | ATP | ATP | ARC | ARC | RC | RAB | FX |
| ext 1 | DCR | GBS | SIM | TST | SAA | CL | CL | FX |
| ext 2 | spare | LLC | IPC | NOGO | CPR | LDPJ | RCF | FX |
| ext 3 (2.6+) | TBC.EP | TBC.VAL (6) | | | | | | FX |
| ext 4 (2.6+) | MBC.EP | MBC.VAL (6) | | | | | | FX |

- ATP (address type): 0 = 24-bit ICAO, 1 = duplicate address, 2 = surface vehicle address,
  3 = anonymous, 4–7 reserved. ED-129B requires a DF18 CF=1 to be encoded as ATP=3 (note 2).
- ARC: 0 = 25 ft, 1 = 100 ft, 2 = unknown, 3 = invalid.
- RC: 1 = range check passed, CPR validation pending (resets to 0 once CPR validation completes).
- RAB: 0 = report from the target's transponder, 1 = from a field monitor (fixed transponder).
- DCR: 1 = differential correction. GBS: 1 = ground bit set. SIM: 1 = simulated target.
  TST: 1 = test target. **SAA: 0 = equipment *capable* of providing Selected Altitude,
  1 = *not* capable** (the polarity is inverted from what the name suggests).
- CL: 0 = report valid, 1 = suspect, 2 = no information, 3 = reserved.
- Ext 2 gives the reasons the report is suspect (except NOGO). LLC: 1 = black/white-list lookup
  failed. IPC: 1 = independent position check failed (0 is the default when no check is
  implemented). NOGO: 1 = NOGO bit set (as I023/100). CPR: 1 = CPR validation failed.
  LDPJ: 1 = local decoding position jump. RCF: 1 = range check failed (operational users
  normally never see this, because such targets are suppressed).
- TBC / MBC (2.6+): EP = element populated. VAL = total bits corrected over all ES messages
  used in the report (TBC), or the maximum corrected in a single message (MBC).
- Sample: `01 40` → ATP 0, ARC 0, RC 0, RAB 0. Ext 1: GBS=1, all others 0 (ground) [T+S].

**3.3 I021/161 Track Number (2).** b16–13 spare, b12–1 track number (u12).

**3.4 I021/015 Service Identification (1).** Raw. Allocated by the system (same as I023/015).

**3.5 I021/071 Time of Applicability for Position (3).** u24, LSB **2⁻⁷ s** (1/128 s), seconds
since the last UTC midnight. Resets to 0 at midnight. It is the time at which the reported
position is valid.

**3.6 I021/130 Position WGS-84 (6).** LAT s24 then LON s24, LSB **180/2²³ °**
(≈2.146e-5°, ≈2.4 m). Range: lat −90..90, lon −180..<180. Positive means North/East. Sample:
`2bb73e fa65ba` → 61.475329, −7.878699 [T+S].

**3.7 I021/131 High-Resolution Position (8).** LAT s32 then LON s32, LSB **180/2³⁰ °**
(≈1.676e-7°, ≈1.9 cm).

**3.8 I021/072 Time of Applicability for Velocity (3).** As 071: u24, 1/128 s. Not available
in some technologies.

**3.9 I021/150 Air Speed (2).** b16 IM, b15–1 AS (u15). IM=0: IAS, LSB 2⁻¹⁴ NM/s
(×3600 → kt, 0.2197 kt). IM=1: Mach, LSB 0.001.

**3.10 I021/151 True Airspeed (2).** b16 RE (1 = value exceeds the range, so the field holds
the maximum), b15–1 TAS u15, LSB 1 kt.

**3.11 I021/080 Target Address (3).** 24-bit raw address. Its kind is given by I021/040 ATP.
Sample: `000001`, `000002` [T+S].

**3.12 I021/073 Time of Message Reception for Position (3).** u24, 1/128 s since UTC midnight.
The ground station's receive time of the latest position squitter. Sample: `384176` →
28802.922 s = 08:00:02.922 [T+S].

**3.13 I021/074 Time of Message Reception of Position, High Precision (4).** b32–31 FSI,
b30–1 fractional second u30, LSB **2⁻³⁰ s**. FSI says how to get the whole seconds from 073:
0 = same, 1 = 073 whole seconds + 1, 2 = 073 whole seconds − 1, 3 = reserved. Full time =
(floor(073 s) + adj) + frac. Sample: FSI 0, frac 0.9196 → 08:00:02.9196, consistent with 073
[T+S].

**3.14 I021/075 Time of Message Reception for Velocity (3).** As 073, for velocity.

**3.15 I021/076 Time of Message Reception of Velocity, High Precision (4).** As 074, with
FSI relative to 075.

**3.16 I021/140 Geometric Height (2).** s16, LSB **6.25 ft** (25/4), range −1500..150000 ft.
Height above the WGS-84 ellipsoid (the ".. tangent plane" wording in the spec). 0x7FFF means the
aircraft reports "greater than".

**3.17 I021/090 Quality Indicators (extended).** All values are raw codes, interpreted per
the MOPS version in I021/210 (except PIC).

| Octet | b8 | b7 | b6 | b5 | b4 | b3 | b2 | b1 |
|---|---|---|---|---|---|---|---|---|
| primary | NUCr/NACv (3) | | | NUCp/NIC (4) | | | | FX |
| ext 1 | NICbaro | SIL (2) | | NACp (4) | | | | FX |
| ext 2 | spare | spare | SILS | SDA (2) | | GVA (2) | | FX |
| ext 3 | PIC (4) | | | | spare (2.7: SRC) | spare | spare | FX |
| ext 4 (2.7) | spare | spare | VALSTATE.EP | VALSTATE.VAL (2) | | VD | VQ | FX |
| ext 5–8 (2.7) | VALDISTP1 / VALDISTP2 / VALDISTQUALP1 / VALDISTQUALP2 (u7 each, one per octet) | | | | | | | FX |

- The primary octet is kept for backward compatibility: a version-2 NIC is mapped into NUCp/NIC
  there, so that pre-2.0 consumers still get it (note 2).
- SIL is "Surveillance (v1) or Source (v2) Integrity Level". SILS: 0 = per flight hour,
  1 = per sample. SDA and GVA are as defined in MOPS v2.
- PIC = Position Integrity Category. PIC=0 means the 3rd extension is not generated. The
  PIC↔NIC/NIC-supplement table is "in the original PDF". **It is not in the transcription and
  was not read [U]**.
- 2.7: SRC (ext 3, b4): 0 = PIC mapped from FTC + NIC supplements, 1 = received directly
  (HVA / phase overlay). VAL_STATE: 0 = not performed, 1 = performed without pass/fail,
  2 = pass, 3 = fail. VD/VQ say whether the distance octets are meaningful. The validation
  distance is P1×128 m + P2×1 m, and P1=127 means ≥16.256 km. Quality is coded the same way.
- Sample: `00` (primary only, all zero) [T+S].
- Primary, ext 1 and ext 2 bit positions are as in the item table above [T]. A generated block
  (`11 12`: primary and one extension) decodes to NIC 8, NACp 9.

**3.18 I021/210 MOPS Version (1).** b8 spare. b7 VNS (1 = the MOPS version is not supported by
the GS, which interpreted the data anyway). b6–4 VN: 0 = ED-102/DO-260, 1 = DO-260A,
2 = ED-102A/DO-260B, 3 = ED-102B/DO-260C (value 3 added in 2.5). b3–1 LTT: 0 = other, 1 = UAT,
2 = 1090 ES, 3 = VDL 4, 4–7 not assigned. Sample: `02` → VN 0, LTT 1090 ES [T+S].

**3.19 I021/070 Mode 3/A Code (2).** b16–13 spare, b12–1 four octal digits (3 bits each,
A then B, C, D from the MSB) [T]. The
per-digit bit order is the usual ASTERIX "octal representation" [U]; a generated `09 c9` decodes
to 4711. Ed. 0.2x put V/G/L flags in b16–14 (§4).

**3.20 I021/230 Roll Angle (2).** s16, LSB 0.01°, range −180..180. Negative = left wing down.
1090 ES resolution is 1°.

**3.21 I021/145 Flight Level (2).** s16, LSB **1/4 FL** (= 25 ft) [T] (a generated `02 d0`
is FL 180), range −15..1500 FL.
Barometric, not QNH-corrected. alt_baro_ft = raw × 25.

**3.22 I021/152 Magnetic Heading (2).** u16, LSB 360/2¹⁶ ° (0.0055°). True heading is in RE/TNH.

**3.23 I021/200 Target Status (1).** b8 ICF (intent change; always 0 from MOPS v3 on).
b7 LNAV (**0 = engaged**, 1 = not engaged, the reverse of DO-260). b6 ME (military emergency,
added in 2.2). b5–3 PS: 0 none, 1 general, 2 lifeguard/medical, 3 minimum fuel, 4 no
communications, 5 unlawful interference, 6 downed aircraft. b2–1 SS: 0 no condition,
1 permanent alert (emergency), 2 temporary alert (3/A change other than emergency), 3 SPI.
For VN=3 aircraft, PS3 lives in RE/STA and is mapped down into PS (2→4, 6→1, 7→1).

**3.24 I021/155 Barometric Vertical Rate (2).** b16 RE, b15–1 BVR **s15**, LSB 6.25 ft/min.
Sign-extend from bit 15, not bit 16.

**3.25 I021/157 Geometric Vertical Rate (2).** As 155 (b16 RE, b15–1 s15, 6.25 ft/min), WGS-84.

**3.26 I021/160 Airborne Ground Vector (4).** b32 RE. b31–17 GS u15, LSB 2⁻¹⁴ NM/s
(0.2197 kt, range 0..<2 NM/s). b16–1 TA u16, LSB 360/2¹⁶ °, clockwise from true north. The
surface ground vector is in RE/SGV, not here.

**3.27 I021/165 Track Angle Rate (2).** b16–11 spare, b10–1 TAR s10, LSB 1/32 °/s, range ±16.
Positive = right turn. Not sent for 1090 ES.

**3.28 I021/077 Time of ASTERIX Report Transmission (3).** u24, 1/128 s since UTC midnight.
The time the ground system sent the record.

**3.29 I021/170 Target Identification (6).** 8 × 6-bit characters, MSB first (char 1 in
b48–43). The coding is the ICAO 6-bit set ("string icao") [T]. The table is from ICAO Annex 10
Vol IV, not from the PDF [U]:

| Code | Char |
|---|---|
| 1–26 (0x01–0x1A) | A–Z |
| 32 (0x20) | space |
| 48–57 (0x30–0x39) | 0–9 |
| all others (0, 27–31, 33–47, 58–63) | invalid. Map to '?' or reject; readsb drops them |

Trim trailing spaces. An all-space value means no callsign. A generated block
decodes 170 to "RPA4521" followed by a space, which shows an 8-character field but does not
confirm the code table.

**3.30 I021/020 Emitter Category (1).** 0 no information. 1 light (≤15500 lb). 2 small
(15500–75000 lb). 3 medium (75000–300000 lb). 4 high vortex large. 5 heavy (≥300000 lb).
6 highly manoeuvrable / high speed. 7–9 reserved. 10 rotorcraft. 11 glider/sailplane.
12 lighter-than-air. 13 UAV. 14 space/transatmospheric. 15 ultralight/hang glider/paraglider.
16 parachutist/skydiver. 17–19 reserved. 20 surface emergency vehicle. 21 surface service
vehicle. 22 fixed ground or tethered obstruction. 23 cluster obstacle. 24 line obstacle.
Sample: 0 and 21 [T+S].

**3.31 I021/220 Met Information (compound).** Primary octet: b8 WS, b7 WD, b6 TMP, b5 TRB,
b4–2 spare, b1 FX [T, order]. WS u16 1 kt (0–300). WD u16 1° (1–360). TMP s16 0.25 °C
(−100..100). TRB u8 (0–15).

**3.32 I021/146 Selected Altitude (2).** b16 SAS (1 = source info provided). b15–14 S: 0 unknown,
1 aircraft altitude (holding), 2 MCP/FCU, 3 FMS. b13–1 ALT s13, LSB 25 ft (−1300..100000).
Called "Intermediate State Selected Altitude" before 2.x and in 1.8 [U, XML].

**3.33 I021/148 Final State Selected Altitude (2).** b16 MV, b15 AH, b14 AM (1 = mode active),
b13–1 ALT s13, 25 ft. Kept for backward compatibility. Not used for v2 aircraft (use 146 and
RE/NAV).

**3.34 I021/110 Trajectory Intent (compound).** Primary octet: b8 TIS, b7 TID, b6–2 spare, b1 FX.
- TIS (extended): b8 NAV (1 = intent data *not* available), b7 NVB (1 = *not* valid),
  b6–2 spare, b1 FX.
- TID (repetitive): REP octet, then REP × 15 octets. Each repetition is: TCA(1) NC(1) TCPN(u6) |
  ALT s16 10 ft | LAT s24 180/2²³° | LON s24 180/2²³° | PT(4) TD(2) TRA(1) TOA(1) |
  TOV u24 1 s | TTR u16 0.01 NM. The sum is 120 bits = 15 octets.

**3.35 I021/016 Service Management (1).** u8, LSB 0.5 s: the report period of the service
(as I023/101). Sample: `08` → 4 s [T+S].

**3.36 I021/008 Aircraft Operational Status (1).** b8 RA (TCAS RA active). b7–6 TC: 0 none,
1 TC+0 only, 2 multiple, 3 reserved. b5 TS. b4 ARV. b3 CDTIA (CDTI operational). b2 NOTTCAS
(**1 = TCAS not operational**). b1 SA (1 = single antenna). In 1.8, b1 is spare [U, XML].

**3.37 I021/271 Surface Capabilities and Characteristics.**
- Ed. **2.2 and later (including 2.5 and 2.6, which are identical)**, extended:
  primary = b8–7 spare, b6 POA, b5 CDTIS, b4 B2low (1 = <70 W), b3 RAS, b2 IDENT, b1 FX.
  ext 1 = b8–5 LW (4-bit length/width code), b4–2 spare, b1 FX.
- Ed. **2.1 and earlier (1.8 per XML [U])**: the same primary octet. The ext 1 octet is
  b8–5 spare, **b4–1 LW, with no FX bit** ("the LSB in the first extension is not used as
  FX"). This makes the item at most 2 octets, and a 2.2+ length rule misreads an odd LW as FX.
  The spec states "Edition 2.2 is not backwards compatible with previous editions" [T].
- The LW code table is "in the original PDF" and **was not read [U]** (it follows the DO-260B
  aircraft length/width codes).
- "No data / unknown" (v2 aircraft, value 0) means ext 1 is not generated.

**3.38 I021/132 Message Amplitude (1).** s8, LSB 1 dBm, the latest received squitter.
Sample: `cb` → −53 dBm, `ad` → −83 dBm [T+S].

**3.39 I021/250 Mode S MB Data (repetitive).** REP octet, then REP × 8 octets. Each 8 octets
hold the 56-bit MB data followed by BDS1 (4 bits) and BDS2 (4 bits) [T for 64 bits "bds"; the
56+4+4 split is the usual ASTERIX layout, U]. BDS 2,0 goes in 170 and BDS 3,0 in 260.

**3.40 I021/260 ACAS Resolution Advisory Report (7).** TYP(5, =28 for 1090 ES v2) STYP(3, =2)
ARA(14) RAC(4) RAT(1) MTE(1) TTI(2) TID(26), all raw. A copy of BDS 3,0 (via ES type 28/2).

**3.41 I021/400 Receiver ID (1).** Raw. The designator of the ground station within a
distributed system.

**3.42 I021/295 Data Ages (compound).** The primary subfield has 23 presence bits over up to
4 octets (7 per octet + FX). Each present subfield is 1 octet u8, LSB **0.1 s**, max 25.5 s
(meaning "or above"). Order (bit 8 of the first octet first): 1 AOS (008), 2 TRD (040),
3 M3A (070), 4 QI (090), 5 TI1 (110), 6 MAM (132), 7 GH (140) | 8 FL (145), 9 SAL (146;
named **ISA** "Intermediate State Selected Altitude Age" in ≤2.5, same layout), 10 FSA (148),
11 AS (150), 12 TAS (151), 13 MH (152), 14 BVR (155) | 15 GVR (157), 16 GV (160), 17 TAR (165),
18 TI2 (170), 19 TS (200), 20 MET (220), 21 ROA (230) | 22 ARA (260), 23 SCC (271).
Sample: `54 0d 0d 0d` → TRD, QI, MAM = 1.3 s. `55 01 10 0a 0a 0a ff` → TRD/QI/MAM 1.0 s,
TI2 25.5 s [T+S].

**3.43 RE: Reserved Expansion Field (explicit).** LEN (1 octet, includes itself), then a
compound. Layout from asterix-specs `ref-1.5.ast` (RE "edition 1.5", 2021-12-22, which matches
CAT021 2.6). Which RE edition belongs to which CAT021 edition is [U]. Primary subfield:
b8 BPS, b7 SH, b6 NAV, b5 GAO, b4 SGV, b3 STA, b2 TNH, b1 FX. Next octet: b8 MES.

| Sub | Length | Layout [T] |
|---|---|---|
| BPS | 2 | b16–13 spare, b12–1 BPS u12, LSB 0.1 hPa (0..409.5). Whether there is an offset (e.g. +800 hPa) is in prose that was not read [U] |
| SH | 2 | b16–13 spare, b12 HDR (horizontal reference: true/magnetic), b11 STAT, b10–1 SH u10, LSB 45/64° (0.703125°) |
| NAV | 1 | b8 AP, b7 VN, b6 AH, b5 AM, b4 MFM.EP, b3 MFM.VAL (MFM added in RE 1.5; spare in 1.4), b2–1 spare |
| GAO | 1 | GPS antenna offset, raw |
| SGV | extended, 2 or 3 | Octets 1–2: b16 STP, b15 HTS, b14 HTT, b13 HRD, b12–2 GSS u11, LSB 1/8 kt, b1 FX. Octet 3: b8–2 HGT u7, LSB 45/16° (2.8125°), b1 FX |
| STA | extended, 1–6 | ES, UAT, RCE(EP+2), RRL(EP+1), FX / PS3(EP+3), TPW(EP+2), FX / TSI(EP+2), MUO(EP+1), RWC(EP+1), FX / DAA(EP+2), DF17CA(EP+3), FX / SVH(EP+2), CATC(EP+3), FX / TAO(EP+5), spare, FX. In RE 1.4 STA was 1 octet: ES, UAT, spare 5, FX |
| TNH | 2 | True north heading u16, 360/2¹⁶ ° |
| MES | compound | Military ES: SUM 1, PNO 4, EM1 2, XP 1, FOM 1, M2 2 |

Sample RE: `05 08 f0 01 62` → SGV only: STP=1 HTS=1 HTT=1 HRD=1, GSS=0 kt, HGT=49×2.8125 =
137.8°. `05 08 70 f1 40` → STP=0, GSS=120×0.125 = 15 kt, HGT=90° [T+S]. Because RE is explicit,
**skip it by LEN** when its contents are not needed. Decode SGV for surface speed and heading.

**3.44 SP: Special Purpose Field (explicit).** LEN (includes itself) + user-defined contents.
Always skip.

## 4. Edition differences that matter to a multi-edition decoder

| Edition (date) | Differences that change parsing or meaning [T unless noted] |
|---|---|
| 0.23 (2003-11), 0.24 (2004-10) | **Different UAP, 35 FRNs**: 1 010, 2 040, 3 030 (Time of Day, u24 1/128 s), 4 130, 5 080, 6 140, 7 090, 8 210, 9 230, 10 145, 11 150, 12 151, 13 152, 14 155, 15 157, 16 160, 17 165, 18 170, 19 095 (Velocity Accuracy, 1), 20 032 (ToD Accuracy, 1, LSB 2⁻⁸ s), 21 200, 22 020, 23 220, 24 146, 25 148, 26 110, 27–33 spare, 34 RE, 35 SP. **130 is 6 octets** (s24/s24, 180/2²³). 040 is **fixed 2 octets**: DCR GBS SIM TST RAB SAA SPI spare / ATP(3) ARC(2) spare(3). 090 "Figure of Merit" fixed 2 octets: AC(2) MN(2) DC(2) spare(6) PA(4). 151 is u16 kt with no RE bit. 155/157 are s16. 160 GS is s16. 165 "Rate of Turn" is *extended*: TI(2) spare(5) FX / ROT s7 1/4 °/s FX. 200 is one octet whose value is PS (0–5). 210 "Link Technology Indicator": spare(3) DTI MDS UAT VDL OTR. The CroatiaControl 0.23 XML has 070/131 at FRN 27/28, unlike asterix-specs 0.23 [U]. |
| 0.25 (2005-03) | 130 becomes **8 octets** (s32/s32, LSB 180/2²⁵). Otherwise the same as 0.24. |
| 0.26 (2005-06) | Adds FRN 27 = 070 (V, G, L, spare, 12-bit octal) and FRN 28 = 131 "Signal Amplitude" (1 octet). Note that **131 means something else** in 2.x. |
| 1.x (1.8, 2011-01) | **New 49-FRN UAP, identical to 2.x** [U, XML]. 040 ≤3 octets; ext 2 b8–7 are other/spare bits (the XML names them EC7/EC6). 090 is only primary + ext 1. 200 has b8–6 spare (no ICF/LNAV/ME). 008 b1 spare. 271 is the old non-FX form. 146 is "Intermediate State". asterix-specs has no 1.x file. |
| 2.1 (2011-05) | 090 gains ext 2 (SILS/SDA/GVA) and ext 3 (PIC). 200 gains ICF and LNAV. 271 is still the old form (ext 1 = spare 4 + LW 4, no FX). |
| 2.2 (2014-08) | **271 restructured: ext 1 = LW(4) spare(3) FX. Not backward compatible.** 200 b6 ME added (was spare). 260 note fixed (BDS 6,1). |
| 2.3 (2015-01) / 2.4 (2015-06) | 040 ext 2 b7 LLC added (was spare). 2.4 has no structural change. |
| 2.5 (2021-02) | 210 VN=3 (DO-260C) added. ICF must be 0 for v3. Notes only otherwise. 271 is identical to 2.2–2.4. |
| 2.6 (2021-12) | 040 gains ext 3 (TBC) and ext 4 (MBC). 295 subfield 9 renamed ISA→SAL (same layout). PS3 mapping and the LNAV/MFM note added. |
| 2.7 (2025-07) | 090 ext 3 b4 becomes SRC (was spare). 090 gains ext 4–8 (VAL_STATE, VD, VQ, validation distance and quality). Same UAP. The only additions are FX-chained octets, so a decoder that follows FX generically reads 2.7 with 2.6 rules. **No layout change in 2.7 breaks a 2.6 decoder** [T]. A web-search summary of 2.7 mentions a "note 6 correction in I021/110"; the transcription shows no structural change to 110 [T]. |

**The edition cannot be detected from the data.** A data block carries only CAT and LEN, with
no edition field. **I021/210 MOPS Version describes the aircraft's transponder/ADS-B link
(DO-260/A/B/C), not the ASTERIX edition.** A 2.6 ground station reports VN=0 for an old
transponder. Heuristics such as "130 is 6 or 8 octets" cannot be applied reliably, because
records have no lengths. Therefore:

- **Configuration knob (per input/listener): `cat021_edition`.** The decoder needs these
  length-rule families: `0.23` (= 0.24), `0.25`, `0.26`, `1.x/2.0–2.1` (49-FRN UAP, old
  271), and `2.2+` (2.2–2.7: the FX-extended 271). Default `2.6`. Within `2.2+`, the editions
  differ only in bit meanings, so one decoder can read all of them by following FX and
  ignoring unknown extension octets. The pingStation 3 sends **ed. 2.5**, which
  is in the `2.2+` family. It never sets 040 ext 3/4 (2.6-only), and it sends neither 271 nor
  RE.
- Optionally, `cat021_re_edition` (RE 1.4 vs 1.5). Since RE is explicit and its newer parts
  are FX-chained, this matters only to decode NAV.MFM and STA beyond its first octet.
- A sanity check, not detection: on length errors (records not ending exactly at LEN), log
  "edition mismatch?" with the configured edition.

## 5. Mapping to skycued's `TrackObservation`

`TrackObservation` is defined in `skycued/src/observation.hpp`, merging happens in
`track_table.hpp`, and the identity types are in `libskycue/include/skycue/types.hpp`.

| Field | From | Notes |
|---|---|---|
| `id.address` | 080 | 24 bits. Drop the record if 080 is absent: there is no identity to use (161 is the ground system's track number, not an address). |
| `id.type` | 040 ATP | 0 → `AddressType::icao`. 1 (duplicate) → `icao`, logged/counted. 2 (surface vehicle), 3 (anonymous) → `non_icao`. 4–7 → `other`. Recommendation [U]. |
| `source` | 210 LTT | 2 → `adsb_1090`, 1 → `uat_978`, else `other`. If 210 is absent, use a per-input default (normally `adsb_1090`). |
| `received_time` | local clock at datagram arrival | as readsb "now". |
| `position_time` | **071** if present, else **073** (+074 high precision), else 077, else `received_time` | 071 is when the position is *valid*. 073/074 is when the squitter was *received* (very close for ES). 077 is the *ASTERIX send time*, a last resort that is biased late. The pingStation 3 never sends 071/074 (§8), and a record may carry none of 073/075/077, so the `received_time` fallback is needed. Each is seconds-of-day only. Rebuild the date from the UTC date of `received_time`: if tod − now_tod > 12 h, the value is from the previous day; if < −12 h, from the next day. |
| `lat`,`lon` | **131** if present, else 130 | Set both or neither. Ignore the position when 040 shows CL=1 (suspect) or ext 2 CPR/RCF/IPC=1 (§5.1). |
| `on_ground` | 040 ext 1 GBS | Absent ext 1 = not on ground (GBS 0). Ed. 0.2x: GBS is in the fixed 040. |
| `alt_baro_ft` | 145 × 25 ft | Rounded to int. ARC=3 (invalid) → do not use 145. |
| `alt_geom_ft` | 140 × 6.25 ft | Ignore 0x7FFF ("greater than"). |
| `gs_kt`,`track_deg` | 160 (GS × 3600/2¹⁴, TA × 360/2¹⁶) | Surface: RE/SGV GSS × 0.125 kt, HGT × 2.8125°. HRD/HTT say whether HGT is heading or track and whether it is true or magnetic. Ignore when the 160 RE bit is set, or treat it as "≥". |
| `baro_rate_fpm` / `geom_rate_fpm` | 155 / 157 (s15 × 6.25) | Ignore when the RE bit is set. |
| `callsign` | 170 | ICAO 6-bit, trimmed. Empty → nullopt. |
| `squawk` | 070 | 4 octal digits, zero-padded ("%04o"). |
| `category` | 020 | Convert to readsb's DO-260 "A0..D7" strings [U]: 1–6 → A1–A6, 10 → A7, 11 → B1, 12 → B2, 16 → B3, 15 → B4, 13 → B6, 14 → B7, 20 → C1, 21 → C2, 22 → C3, 23 → C4, 24 → C5. 0 and reserved → nullopt. |
| `emergency` | 200 PS | 0 → "none", 1 "general", 2 "lifeguard", 3 "minfuel", 4 "nordo", 5 "unlawful", 6 "downed" (readsb's strings). The 200 ME bit could be counted as "general" [U]. |
| `nic` | 090 primary NUCp/NIC | Interpretation per 210 VN (v0: NUCp). |
| `nac_p` | 090 ext 1 NACp | Absent for 0.2x/1.x if ext 1 is missing. |
| `rssi` | 132 (dBm) | **Not readsb's dBFS.** Label it or keep it separate. |
| `registration` | — | Not in CAT021. |
| `raw_line` | the record's bytes as hex | e.g. "cat021:" + hex, for recordings. |

### 5.1 Descriptor flags and filtering

- **SIM=1 (simulated) or TST=1 (test target)**: drop by default and count. Optionally forward
  under a config flag for site testing. "Test target" is a ground-station test or monitor
  transponder, not a real aircraft.
- **RAB=1 (field monitor / fixed transponder)**: drop by default. It is a parked reference
  transponder used to monitor the station.
- **CL=1 (suspect)** or ext 2 **CPR=1, RCF=1, IPC=1, LLC=1, LDPJ=1**: keep the identity and
  non-position fields, but do not accept the position. CL=2 means no information (accept).
- **RC=1**: range check passed, CPR validation pending. The position is provisional: accept it
  but do not use it to start a new track (or wait for one more report).
- **NOGO=1**: the station's GO/NOGO bit is set. The data may be unreliable, so log it.
- **DCR**: informational. **SAA**: 1 = cannot provide a selected altitude (inverted). Do not
  read SAA as "146 is valid".
- **ARC**: 0 = 25 ft, 1 = 100 ft resolution; 2/3 = unknown/invalid (treat 145 as untrusted
  for 3).
- **GBS=1 = "ground"**: the aircraft's air/ground status says it is on the surface. It sets
  `on_ground` and applies existing ground filters (surface vehicles are also ATP=2 or ecat
  20/21). It does not mean the report came from a ground system.

## 6. Transport

- ASTERIX defines no transport framing. Over UDP, the convention is **one or more complete data
  blocks per datagram**, most often exactly one block containing one or more records. The only
  framing is CAT + LEN [U: Part 1 could not be fetched; CAT001 Part 2a defines only the block].
  Several blocks can be concatenated (the sample file is two blocks back-to-back, §7), so the
  decoder must loop `while (off + 3 <= size)` and step by LEN. A LEN < 3 or past the end of the
  datagram → drop the rest of the datagram.
- Typical deployment: UDP **multicast** from the ADS-B ground station or SDPS (a group and port
  per service/SAC-SIC), or unicast to a configured host. Ports are site-specific. There is no
  well-known port [U]. Some systems wrap ASTERIX in vendor headers (e.g. a final-format record
  with a timestamp); those must be configured explicitly, never guessed.
- Keep SAC/SIC (010) and 015/400 so that several stations or services can be told apart. 016
  gives the expected update period.
- **pingStation 3**: CAT021 over **UDP** (to one configured host/port) or
  **TCP**, one aircraft update per datagram or TCP write. A TCP reader must still frame on
  CAT/LEN. The device documentation describes unicast or broadcast (its ATAK example is `x.x.x.255`, port 4000), with no multicast and no
  default port (port 0, output disabled until configured). SAC/SIC are set on the device web
  page (default 00/00). Only one UDP format can be active, so with UDP CAT021 the device sends
  no status message. Its position and health then come only from `GET /api/v1/status` (JSON).

## 7. Test data

- `cat21_re.ast` from CroatiaControlLtd/asterix `install/sample_data/` (added 2016-02-24 with
  the RE definition for CAT021 1.8). It is **91 octets = two data blocks** (LEN 44 + 47). Each
  holds one record with FSPEC `c5 1d 31 01 43 23 04` (7 octets) = FRNs 1, 2, 6, 11, 12, 13, 17,
  18, 30, 35, 38, 42, 48 → 010, 040, 130, 080, 073, 074, 090, 210, 020, 016, 132, 295, RE.
  Decoded: SAC 0 / SIC 1, addresses 000001 and 000002, both GBS=1 (on ground), position
  61.4753 N 7.8787 W, ToMR 08:00:02.92 and 08:00:03.16, VN 0 / LTT 1090 ES, ecat 0 and 21,
  016 = 4 s, amplitude −53/−83 dBm, data ages as in §3.42, RE/SGV 0 kt 137.8° and 15 kt 90°.
  Both records end exactly at LEN with the 2.x length rules. **Licence**: the file has no
  licence of its own. The repository's LICENSE is **GPL-2.0** (GitHub reports `GPL-2.0`, the
  LICENSE text is GPL v2), **not GPL-3 as assumed in the task**. Do not commit the file into a
  non-GPL repo. Reproducing its 91 bytes as a test vector is a licensing judgement call.
- No larger public CAT021 sample was found. CroatiaControl's other samples (`asterix.pcap`,
  CAT 001/002/034/048/062/065) are other categories, also GPL. **We need our own CAT021
  encoder for tests**: build records from a `TrackObservation`-like struct with the 2.6 rules
  (FSPEC builder, FX chains, compound primary subfields, RE explicit length). Include
  round-trip tests, all-items records, unknown-FRN/edition-mismatch cases, multi-record blocks,
  midnight wrap, and negative lat/lon/rates.

## 8. pingStation 3 subset

What the uAvionix pingStation 3 emits, as modelled by the decoder and by `skycue-pingsim`.
Edition: **CAT021 2.5**. Each field is sent only when valid data is available, so every item is
optional per record.

| FRN | Item | Section | Notes |
|---|---|---|---|
| 1 | 010 Data Source Identification | 3.1 | SAC/SIC from the device configuration |
| 2 | 040 Target Report Descriptor | 3.2 | 2 octets in the example (primary + ext 1) |
| 6 | 130 Position WGS-84 | 3.6 | sent together with 131 in the example |
| 7 | 131 High-Resolution Position | 3.7 | prefer it over 130 |
| 9 | 150 Air Speed | 3.9 | |
| 10 | 151 True Airspeed | 3.10 | |
| 11 | 080 Target Address | 3.11 | |
| 12 | 073 Time of Message Reception for Position | 3.12 | absent from the example |
| 14 | 075 Time of Message Reception for Velocity | 3.14 | absent from the example |
| 16 | 140 Geometric Height | 3.16 | absent from the example |
| 17 | 090 Quality Indicators | 3.17 | 3 octets in the example (no PIC) |
| 18 | 210 MOPS Version | 3.18 | |
| 19 | 070 Mode 3/A Code | 3.19 | |
| 21 | 145 Flight Level | 3.21 | |
| 22 | 152 Magnetic Heading | 3.22 | |
| 23 | 200 Target Status | 3.23 | |
| 24 | 155 Barometric Vertical Rate | 3.24 | |
| 25 | 157 Geometric Vertical Rate | 3.25 | |
| 26 | 160 Airborne Ground Vector | 3.26 | |
| 28 | 077 Time of ASTERIX Report Transmission | 3.28 | absent from the example |
| 29 | 170 Target Identification | 3.29 | |
| 30 | 020 Emitter Category | 3.30 | |
| 36 | 008 Aircraft Operational Status | 3.36 | |

All rows follow the ed. 2.5 UAP [T]. The highest FRN is 36, so the FSPEC is at most 6 octets.
**Never sent**: 161, 015, 071, 072, 074, 076, 230, 165, 220, 146, 148, 110, 016, 271, 132, 250,
260, 400, 295, RE, SP. In practice this means no message amplitude (RSSI), no surface ground
vector (RE/SGV), no true-north heading (RE/TNH), and no high-precision times. The decoder should
still implement the full 2.2+ length rules (§1), because a firmware change could add items.

## 9. Sources (all fetched or attempted 2026-11-02)

| Source | Edition | Result |
|---|---|---|
| https://www.eurocontrol.int/sites/default/files/2021-12/asterix-adsbtr-cat021-part12-v2-6.pdf | CAT021 2.6 (EUROCONTROL-SPEC-0149-12) | **HTTP 403, not read** |
| https://www.eurocontrol.int/sites/default/files/2021-02/asterix-adsbtr-cat021-part12-v2-5.pdf | 2.5 | **403, not read** |
| https://www.eurocontrol.int/sites/default/files/2025-07/asterix-adsbtr-cat021-part12-v2-7.pdf | 2.7 | **403, not read** |
| https://www.eurocontrol.int/sites/default/files/2019-06/asterix-cat021-asterix-ads-b-messages-part-12-v1.8-012011.pdf | 1.8 | **403, not read** |
| https://www.eurocontrol.int/publication/eurocontrol-specification-surveillance-data-exchange-part-i | Part 1 (page lists ed. 3.1 as newest, per search snippet) | **403, not read** |
| https://www.icao.int/sites/default/files/sp-files/SAM/Documents/2010/ASTERIX/01%20ASTERIX_CAT_1.pdf | EUROCONTROL CAT001 Part 2a ed. 1.1, Aug 2002 | **Read** (data block / LEN / record / UAP definitions only) [V] |
| https://github.com/zoranbosnjak/asterix-specs `specs/cat021/cat-{0.23,0.24,0.25,0.26,2.1,2.2,2.3,2.4,2.5,2.6,2.7}.ast`, `ref-{1.4,1.5}.ast`, `remarks.md` (master, last commit 2026-10-27) | Transcriptions, BSD-3-Clause (© 2019 KZPS) | **Read in full for 2.6. Structural diffs for the rest** [T] |
| https://github.com/CroatiaControlLtd/asterix `install/config/asterix_cat021_{0_23,0_26,1_8,2_4,2_6}.xml` | Decoder configs, GPL-2.0 | Read for cross-check only, nothing copied [U] |
| https://github.com/CroatiaControlLtd/asterix `install/sample_data/cat21_re.ast` | 91-byte sample, GPL-2.0 repo | Decoded (§7) |

Known gaps (to close when a PDF can be obtained by hand): the PIC table (090), the LW codes
(271), the BPS offset and the remaining RE prose, the exact UDP wording in Part 1, the 1.x
editions other than 1.8, and the 1.8 layouts (seen only in the GPL XML).
