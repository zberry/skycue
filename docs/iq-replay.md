# IQ replay

The other replay modes feed the decoders data that has already been
demodulated: Beast frames, dump978 text, readsb JSON. IQ replay goes one step
earlier. It makes raw radio samples that readsb's 2.4 MS/s Mode S demodulator
and dump978-fa's 2.083 MS/s UAT demodulator can read. That way the whole
chain can be exercised before a dongle is plugged in: samples, demodulator,
decoder, skycued, cues on the wire. This page covers making the files; readsb and dump978-fa read them with
their own file inputs (`readsb --device-type ifile --ifile FILE --iformat UC8`,
`dump978-fa --file FILE --format CU8`).

There are two sources of IQ files:

- **`skycue-iqgen`** synthesises CU8 files from the recordings already in
  `tests/fixtures/raw/`. You get exact ground truth and any length you
  want.
- **`scripts/fixtures/fetch-iq-samples.sh`** fetches one public off-air 1090 MHz
  capture (Porto, Zenodo 23342598, CC BY 4.0) and resamples it for readsb.
  It gives an independent check on real signals.

The real decoders can read either kind of file.

A pingStation 3 has no IQ stage to replay: it sends decoded traffic.
`skycue-pingsim` stands in for one, from its capture files or from a readsb
recording synthesised as pingStation JSON or CAT021 (see
[operating.md](operating.md#a-pingstation-without-hardware-skycue-pingsim)).

IQ files are never committed. One minute of 1090 MHz at 2.4 MS/s is 288 MB.
The tests generate 5 s slices (about 24 MB) in a temporary directory and
delete them afterwards. The fetch script caches under `build/`, which git
ignores.

## What is and is not simulated

**Simulated:**

- the waveforms below, at a set level;
- additive white Gaussian noise;
- one constant carrier frequency offset;
- the sum of frames that overlap in time, as happens on the air. The sum is
  clipped to 0..255 like an 8-bit ADC.

**Not simulated:**

- no channel model beyond the above: no multipath, no fading, no other
  interference;
- no SDR emulation: no fake `rtl_tcp`, no SoapySDR file module, no tuner,
  AGC or DC spike. The decoders' own file inputs are the interface;
- no bit-clock or sample-clock offset;
- no UAT ground-segment (MSO slot) model: downlinks are placed by their
  recording timestamps only.

All output is CU8: interleaved unsigned 8-bit I and Q, with 128 as zero.
readsb reads it as `--iformat UC8` and dump978-fa as `--format CU8`.

## Waveforms

### Mode S (1090 MHz)

| item | definition |
|---|---|
| output rate | 2 400 000 S/s, the rate readsb's `demod_2400` expects |
| preamble | four 0.5 µs pulses starting at 0.0, 1.0, 3.5 and 4.5 µs |
| data | from 8.0 µs, one bit per µs (PPM) |
| bit encoding | a 1 bit has its pulse in the first half-µs, a 0 bit in the second |
| frame end | 56-bit frames end at 64 µs, 112-bit frames at 120 µs |
| pulses | rectangular, on I (Q = 0), then rotated by the `--cfo` phase |
| generation | built at 12 MS/s (6 samples per 0.5 µs chip), then box-filter decimated by 5, so pulses have the fractional timing a receiver sees |
| timing | Beast and `@` timestamps are the 12 MHz 48-bit counter, unwrapped at 2^48; frames are sorted by time and placed relative to the first frame; untimed lines are `--spacing-us` apart (default 2000) |

### UAT (978 MHz)

| item | definition |
|---|---|
| output rate | exactly 2 083 333 S/s, 2 samples per bit |
| modulation | phase-continuous binary CPFSK, rectangular frequency pulse, deviation ±312.5 kHz (modulation index 0.6), 1 041 667 bit/s |
| bit encoding | a 1 bit is +312.5 kHz; every byte is sent MSB first |
| CU8 mapping | I = 128 + A cos φ, Q = 128 + A sin φ; a positive frequency is φ increasing |
| sync word | 36 bits, MSB first: downlink `0xEACDDA4E2`, uplink `0x153225B1D` |
| downlink short | 144 data bits + 96 parity bits, RS(30,18) |
| downlink long | 272 data bits + 112 parity bits, RS(48,34) |
| uplink | 432 payload bytes in 6 blocks of RS(92,72), block-interleaved: byte *i* of block *j* is sent at position 6·i + j |
| Reed–Solomon | GF(2^8), field polynomial 0x187, fcr 120, prim 1, encoded in `skycue-iqgen` itself (no FEC library) |
| timing | `@ts` or stratux nanosecond timestamps; untimed lines are `--spacing-us` apart (default 20000) |

The UAT inputs are post-FEC payloads, which is what dump978 prints, so
`skycue-iqgen` re-encodes them. An uplink must be 432 bytes and a downlink 18
or 34 bytes.

### Level, noise and offset (both bands)

- **`--level DBFS`** (default −12) sets the amplitude A = 128 · 10^(level/20)
  in CU8 counts. For Mode S that is the peak pulse; for UAT it is the
  constant envelope. At −12 dBFS, A ≈ 32.
- **`--snr DB`** (default 25) is A² over the complex noise power. Each of I
  and Q gets Gaussian noise with σ = A · 10^(−snr/20) / √2, added before
  quantisation. `--snr none` adds no noise.
- **`--cfo HZ`** (default 0) rotates the output of either band at the output
  rate, before quantisation.

This SNR is a peak-signal definition chosen for the generator. It is not a
receiver noise figure.

## The sidecar: expected and optional frames

Next to `OUT.cu8`, both generators write `OUT.frames.txt`, one line per
placed frame:

```
<sample offset> <hex> <expected|optional>[ overlap]
```

The sample offset is in output samples. A frame is **overlapped** when its
waveform window intersects another frame's.

A Mode S frame is **expected** when it is DF11, DF17 or DF18, its Mode S CRC
(computed by `skycue-iqgen`) is clean, and it is not overlapped. Every other
frame is **optional**:

- the address-on-parity formats (DF0, 4, 5, 16, 20, 21);
- bad-CRC frames from the recording;
- overlap victims.

Optional frames are placed like any other but are not counted. A UAT frame is
expected unless it is overlapped.

A decoder test counts recovery over expected frames only, frame by frame,
with readsb run with `--no-fix`. A 1090 frame counts when readsb sent the
same payload, byte for byte, with a Beast timestamp at the frame's position:
in ifile mode readsb stamps each frame on a 12 MHz clock derived from the
sample index, and its timestamp was measured at sample offset × 5 + 2399
ticks, spread 2397..2402 over the test slice; the test allows ±24 ticks
(±2 µs). Each decoded frame counts for one expected frame at most, so an
optional overlapped copy, or the same payload sent again elsewhere, never
stands in for an expected frame. dump978-fa gives no sample position, so a
UAT payload placed K times counts as recovered at most as many times as it
was decoded. The thresholds apply to this per-frame recall; the recall over
distinct payloads (decoded at least once, anywhere) is printed beside it. A
decoded
Mode S payload that was never placed fails the test only if it is CRC-clean
by the expected rule. readsb forwards a DF11 with any IID < 80 for a known
address, so overlapped and IID ≠ 0 frames can come out with changed IID bits.
These are counted as `unplaced_unclean`, not failed.

## Making a file

Installed, `skycue-iqgen` is on the PATH; in a build tree it is
`build/tools/iqgen/skycue-iqgen`.

```sh
# 1090: 5 s of the bundled Mode S capture, starting 1 s in
skycue-iqgen modes tests/fixtures/raw/modes-120s.csv.gz \
  --skip-seconds 1 --duration 5 -o /data/iq/sample.1090.cu8

# 978: 5 s of the bundled UAT sample, and the longer stratux log
skycue-iqgen uat tests/fixtures/raw/uat-sample.txt.gz \
  --duration 5 -o /data/iq/sample.978.cu8
skycue-iqgen uat tests/fixtures/raw/stratux-uat-2015.log.gz \
  --duration 60 -o /data/iq/stratux.978.cu8
```

**Inputs:**

- `modes`: Beast `.bin`, `epoch,HEX` `.csv`, and AVR `.txt`/`.avr`
  (including the `@ts…;` lines of `skycue-rawrec`).
- `uat`: dump978 raw lines (`-hex;`/`+hex;`, optionally with `;rs=N` and
  `@ts`) and the stratux `<ns>,<±hex>` log, as `.txt`/`.log`.

Any input may be gzip-compressed (`.gz` after the suffix).

| option | default | meaning |
|---|---|---|
| `-o OUT.cu8` | (required) | output file; the sidecar is `OUT.frames.txt` |
| `--level DBFS` | −12 | signal amplitude, see above |
| `--snr DB` | 25 | A² over complex noise power; `none` for no noise |
| `--cfo HZ` | 0 | constant carrier offset |
| `--seed N` | 1 | noise seed; the same inputs and options give byte-identical output on the same libm and architecture (verified identical on RHEL 8 and 9 builds) |
| `--limit N` | all | keep only the first N frames |
| `--skip-seconds S` | 0 | drop the frames in the first S seconds of each timed input, on the recording's own clock (at most 31 622 400, 366 days; ignored with a warning for an untimed input) |
| `--duration S` | all | keep only frames starting in the next S seconds (after `--skip-seconds`; at most 86 400) |
| `--spacing-us US` | 2000 (modes), 20000 (uat) | gap between untimed frames and between input files (at most 10^9) |
| `--pad-ms MS` | 50 | silence before the first and after the last frame (at most 60 000) |
| `--max-mb MB` | 2048 | refuse to write a file larger than this (exit 2); use `--duration` or raise it |

At the end it prints one summary line, for example:

```
frames=7461 expected=1207 overlaps=797 clipped=0 skipped=0 seconds=5.100 bytes=24479232 skipped_seconds=1.000
```

**Warnings** (on stderr; the file is still written):

- **`N pairs of frames overlap`**: the overlapped frames are marked optional
  in the sidecar. The first second of `modes-120s` is a burst of frames
  sharing a few timestamps (about 22 900 overlapping pairs). That is why the
  examples and the tests start with `--skip-seconds 1`, after which a 5 s
  slice has about 800 overlaps and no clipping.
- **`N I/Q values clipped to 0..255`**: overlapping frames summed past full
  scale. Lower `--level` or raise `--snr`.
- **`N of M lines untimed`**: if a file mixes timed and untimed lines, the
  whole file is placed untimed, `--spacing-us` apart.
- **`IN: no timestamps; --skip-seconds ignored`**: `--skip-seconds` counts on
  a recording's own clock, so an untimed file is kept whole. When no input
  was timed, the summary says `skipped_seconds=0.000`.

Usage errors exit 2, naming the bad line, option or path. A non-finite,
negative or over-cap `--duration`, `--skip-seconds`, `--spacing-us` or
`--pad-ms` is a usage error.

### `resample` and `info`

```sh
skycue-iqgen resample IN.cu8 2000000 -o OUT.1090.cu8          # to 2 400 000 S/s
skycue-iqgen resample - 2000000 -o OUT.1090.cu8 < IN.cu8      # from stdin
skycue-iqgen info OUT.1090.cu8 [--rate 2400000]
```

**`resample`** converts CU8 between rates whose ratio reduces to factors of
at most 1000. 2.0 → 2.4 MS/s is 6/5; 2 083 333 → 2 400 000 is refused. It
uses a polyphase windowed-sinc filter: 32 taps per phase, Blackman window,
unity gain at DC. The cutoff is min(0.5 · in_rate, 0.45 · out_rate):

- **Upsampling:** the cutoff is the input's Nyquist frequency. The input
  holds nothing above it, and readsb needs that full band (see the
  measurements below).
- **Downsampling:** the cutoff keeps an anti-alias margin. For large
  downsampling factors, resample in steps.

The cutoff is where the response falls, not where the passband ends. With
32 taps per phase, the Blackman window gives a transition band of about
0.17 cycles per input sample, so at 2.0 MS/s the effective (flat) passband
ends near ±0.7 MHz, not at the ±1 MHz cutoff.

It prints `in_samples=… out_samples=…`.

**`info`** prints the sample count, the duration at `--rate`, the mean and
peak magnitude, and the DC offset of I and Q.

## The real capture (Porto)

The dataset is "Automatic Dependent Surveillance Broadcast (ADSB) I/Q samples
captured using a Software-Defined Radio (SDR)", by Conrado Guimarães da
Costa, Luís Tiago Paiva and Fernando Fontes (FEUP), Zenodo,
DOI 10.5281/zenodo.23342598, CC BY 4.0. The attribution is in `NOTICE`.

According to its own readme:

- one RTL-SDR Blog V3 in an urban setting in Porto, with a 1090 MHz antenna
  3 m above ground and no LNA;
- centre frequency 1 090 000 000 Hz, 2 000 000 complex samples/s, unsigned
  8-bit interleaved I/Q with no header;
- 36 438 016 samples (18.219 s), requested tuner gain 49.6 dB;
- the authors' own pipeline (pyModeS 3.3.0) retains one aircraft, ICAO
  407181, with 122 messages, 22 of them DF17.

### Fetching it

On a machine with network access:

```sh
scripts/fixtures/fetch-iq-samples.sh                 # cache: build/iq-samples
scripts/fixtures/fetch-iq-samples.sh --cache DIR
```

The script works in this order:

1. It downloads `AWEC2026_ADSB_Data.zip` (19 575 669 bytes) with `curl`.
2. It checks the zip's pinned SHA-256. On a mismatch it deletes the file and
   exits 1.
3. It streams `raw_data/recording.bin` out of the zip with Python's
   `zipfile`, checking that member's pinned SHA-256 on the way (a mismatch
   exits 1).
4. It pipes the recording into `skycue-iqgen resample - 2000000`, so the
   73 MB raw file never lands on disk.
5. It checks the resampled `porto-awec2026.1090.cu8` (87 451 238 bytes)
   against its pinned SHA-256. The resampler is deterministic, but its
   output is byte-identical only on the same libm and architecture, so a
   mismatch here prints a warning with the expected and actual hashes, keeps
   the file and still exits 0.
6. It prints the readsb command to play the file and the receiver position to use.

A second run checks the cached files and downloads nothing.

**Without network access:** copy `AWEC2026_ADSB_Data.zip` into the cache
directory by hand and run with `--offline`. The script then never
downloads. If the zip is missing, it says so and exits 1.

`skycue-iqgen` is taken from `build/tools/iqgen`, else from `PATH`. Exit
codes: 0 done, 1 failure, 2 usage error.

To replay the capture, set the receiver position near Porto
(`receiver.lat = 41.15` / `receiver.lon = -8.61`, and readsb's `--lat` / `--lon`)
and run readsb on `build/iq-samples/porto-awec2026.1090.cu8` as in the
introduction, with skycued reading its JSON port as in a live run
([operating.md](operating.md#iq-files)).

## Limits

- **A decoded IQ file proves the demodulator chain only.** It shows that the
  demodulators and decoders as built turn samples into frames and that
  skycued turns those into cues. It says nothing about the antenna, the
  cable, the dongle, its tuner, gain or clock, or the RF environment.
- **No receiver-sensitivity claim.** The `--snr` above is A² over complex
  noise power in CU8 counts, defined by the generator. It is not a noise
  figure or an input power, and its knee does not predict on-air range.
- **The synthesis is idealised.** Waveforms are ideal, apart from additive
  white noise, one constant carrier offset and the sum of overlapping
  frames. Real captures have tuner filtering, DC offsets, interference and
  multipath. The single Porto capture is one point of comparison, not a
  validation.
- **IQ files are never committed.** Generate them where needed and delete
  them.
