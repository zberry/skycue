# skycue wire protocol (version 1)

skycued sends track data to clients as a stream of frames. The codec lives in
`libskycue` (`skycue/protocol.hpp`).

All multi-byte integers are **little-endian**. Floats (`f32`) are IEEE 754
single precision, sent as their 32-bit pattern in little-endian order. Strings
are a `u8` length followed by that many bytes, with no terminator.

## Frame header

Every frame starts with an 8-byte header.

| Offset | Size | Type | Field   | Value                                  |
|-------:|-----:|------|---------|----------------------------------------|
| 0      | 4    | char | magic   | `S` `K` `Y` `C` (`0x53 0x4B 0x59 0x43`) |
| 4      | 1    | u8   | version | `1`                                    |
| 5      | 1    | u8   | type    | frame type, see below                  |
| 6      | 2    | u16  | length  | payload bytes that follow the header   |

| Type | Name          |
|-----:|---------------|
| 1    | Hello         |
| 2    | TrackUpdate   |
| 3    | TrackDrop     |
| 4    | Heartbeat     |

## Hello (type 1)

| Offset | Size | Type   | Field                  |
|-------:|-----:|--------|------------------------|
| 0      | 4    | u32    | keyframe_interval_ms   |
| 4      | 4    | u32    | heartbeat_interval_ms  |
| 8      | 1+n  | string | sender (n ≤ 255)       |

## TrackUpdate (type 2)

Fixed part, 26 bytes:

| Offset | Size | Type | Field            | Notes                                         |
|-------:|-----:|------|------------------|-----------------------------------------------|
| 0      | 4    | u32  | address          | 24-bit address in the low bits                |
| 4      | 1    | u8   | address type     | see below                                     |
| 5      | 1    | u8   | source           | see below                                     |
| 6      | 1    | u8   | flags            | bit 0 keyframe, bit 1 on_ground, bit 2 alt_is_geom; bits 3–7 are 0 |
| 7      | 1    | u8   | reserved         | 0                                             |
| 8      | 8    | u64  | position_time_ms | milliseconds since the Unix epoch; decoders clamp values above 4102444800000 (2100-01-01) |
| 16     | 4    | i32  | lat              | degrees × 10⁷, clamped to ±90                 |
| 20     | 4    | i32  | lon              | degrees × 10⁷, clamped to ±180                |
| 24     | 2    | u16  | presence         | bitmask of optional fields that follow        |

`alt_is_geom` means the elevation filter used geometric altitude for this
track. A non-finite latitude or longitude is sent as 0.

The optional fields follow the fixed part, packed in ascending bit order; only
fields whose presence bit is set are present.

| Bit   | Field             | Type   | Size  |
|------:|-------------------|--------|-------|
| 0     | alt_baro_ft       | i32    | 4     |
| 1     | alt_geom_ft       | i32    | 4     |
| 2     | ground_speed_kt   | f32    | 4     |
| 3     | track_deg         | f32    | 4     |
| 4     | vertical_rate_fpm | f32    | 4     |
| 5     | callsign          | string | 1+n, n ≤ 8  |
| 6     | squawk            | string | 1+n, n ≤ 4 (normally 4) |
| 7     | registration      | string | 1+n, n ≤ 16 |
| 8     | nic               | u8     | 1     |
| 9     | nac_p             | u8     | 1     |
| 10    | alt_geom_uncertainty_ft | u16 | 2   |
| 11–15 | reserved          |        |       |

`alt_geom_uncertainty_ft` is present only when `alt_geom_ft` is an estimate
(see [Geometric altitude](operating.md#geometric-altitude)); a reported
altitude has no bit 10. The value saturates at 65535.

A TrackUpdate with every field present and maximum-length strings is 81 payload
bytes (89 with the header).

Encoders truncate strings longer than the limits above. Decoders reject a
string whose length byte exceeds its limit.

### Address type

| Value | Name           |
|------:|----------------|
| 0     | icao           |
| 1     | non_icao       |
| 2     | tisb_trackfile |
| 3     | other          |

### Source

| Value | Name      |
|------:|-----------|
| 0     | adsb_1090 |
| 1     | uat_978   |
| 2     | tisb      |
| 3     | adsr      |
| 4     | mlat      |
| 5     | other     |

A track is identified by (address, address type).

## TrackDrop (type 3)

| Offset | Size | Type | Field        |
|-------:|-----:|------|--------------|
| 0      | 4    | u32  | address      |
| 4      | 1    | u8   | address type |
| 5      | 1    | u8   | reason       |

| Reason | Name        |
|-------:|-------------|
| 0      | timeout     |
| 1      | filtered    |
| 2      | source_lost |
| 3      | shutdown    |
| 4      | stale       |

## Heartbeat (type 4)

| Offset | Size | Type | Field          |
|-------:|-----:|------|----------------|
| 0      | 8    | u64  | sender_time_ms |

A sender emits a Heartbeat whenever it has sent nothing else for its Hello's
`heartbeat_interval_ms`, so a live stream is never silent for longer than
that. See [Liveness](#liveness).

## Revisions

- **1.1** (additive; the header version stays 1): presence bit 10,
  `alt_geom_uncertainty_ft`. A receiver of 1.0 reads bits 0–9, stops at bit 10
  as for any unknown bit, and sees an estimated altitude as a plain
  `alt_geom_ft`.
- **1.0**: bits 0–9.

## Forward compatibility

- **Version.** A receiver rejects a header whose version is not one it
  supports (`bad_version`). A header with the wrong magic is `bad_magic`.
- **Unknown frame types** are skipped: the header's `length` gives the payload
  size, so the receiver drops `8 + length` bytes and continues.
- **Trailing payload bytes** beyond the fields a receiver knows are ignored.
  Later versions may append fields to any frame type.
- **Unknown presence bits.** If a TrackUpdate has a presence bit set that the
  receiver does not know (bits 11–15 in version 1.1, bits 10–15 for a receiver of 1.0), the receiver stops reading
  optional fields at that bit and keeps the fields it has already read. Because
  fields are packed in bit order and the reserved bits are above every known
  bit, all known fields come before any unknown one.
- **Truncation.** A payload too short for its fixed part, or for a field its
  presence mask announces, is malformed and the frame is rejected.
- **Enumerations.** Address type, source and drop reason values not listed
  above may be added later; receivers should treat unknown values as "other".

## Transport roles

Either end of a TCP connection may be the one that listens. The framing, the
frames and their meaning are identical in every mode; only who dials whom
differs.

| End        | Mode                | Setting                                      | Notes |
|------------|---------------------|----------------------------------------------|-------|
| skycued    | TCP listen          | `cues.tcp_port` (`0`: no listener)            | any number of consumers connect |
| skycued    | TCP connect         | `cues.tcp_connect` (`host:port ...`)          | dials each target, redials with backoff 0.5 s to 10 s |
| skycued    | UDP send            | `cues.udp_targets` (`host:port ...`)          | one frame per datagram; Hello before each keyframe |
| libskycue  | TCP connect         | `Transport::tcp`, `host:port` of skycued     | reconnects every `reconnect_interval` |
| libskycue  | TCP listen          | `Transport::tcp_listen`, bind `host:port`    | any number of skycued connect; one track table |
| libskycue  | UDP bind            | `Transport::udp`, bind `host:port`           | connection is inferred from traffic (heartbeat timeout) |

skycued's modes are the **cue outputs** of its hub: `cues.tcp_port`,
`cues.tcp_connect` and `cues.udp_targets` define the built-in ones (`cue-tcp`,
`cue-connect-N`, `cue-udp-N`), and an output of kind `cue` that you add in the
inputs file, the page or the API ([operating.md](operating.md#outputs)) is one
more listener, dialled target or UDP target of the same kind, with its own
port, host or group. Every cue output serves the same protocol (Hello, a
keyframe of every track to each new connection, then the normal stream), and
the Cues tab and `/api/cues/clients` list the sessions of all of them, each
named by its output. The settings in the table above are the built-in outputs'.

skycued's three modes can be used together; a TCP connection skycued dialled
is served exactly like one it accepted (Hello, then a keyframe of every
track, then the normal stream). A listening library merges the frames of all
its connections into one table and counts as connected while at least one
connection is open. Each track belongs to the connection it last came from:
that connection's keyframe interval decides when it is stale, only that
connection's TrackDrop removes it, and when that connection closes (or falls
silent, see [Liveness](#liveness)) its tracks are dropped as `source_lost`.
The UDP receiver is single-source: bind one port per sending skycued.

skycued sends all of this through its network library, which differs from
earlier releases in three ways. Every TCP connection skycued serves or dials
has keepalive (probes after 30 s idle, every 10 s, three probes) and
`TCP_NODELAY` switched on; a receiver that vanished without closing is now
noticed by skycued as well. A connect to a dialled target gives up on each
address after 10 s (earlier releases waited as long as the kernel allowed).
UDP is best effort: a datagram the kernel cannot take at once is dropped and
counted, not queued; a host name that resolves to several addresses is sent to
at one only (the first IPv4 address, or the first address when none is IPv4);
and a UDP target may now be an IPv6 address (earlier releases skipped those).
Frames, Hello timing, keyframe bursts and shutdown drops are unchanged. A TCP
consumer that falls more than 1 MiB behind is disconnected, as before, and is
greeted afresh (Hello, keyframe) when it reconnects.

## Liveness

Heartbeats are the liveness signal in every transport. A receiver treats a
source that has delivered no frame of any type for a timeout as gone; every
frame received resets the clock. In libskycue (`heartbeat_timeout` is the
client's own setting, 3 s by default):

- **UDP:** the timeout is `heartbeat_timeout`. The source is lost
  (`onConnectionChange(false)`, then `source_lost` for every track).
- **TCP (both modes):** the timeout is, per connection,

      max(heartbeat_timeout, 3 × heartbeat_interval_ms of its Hello)

  and `heartbeat_timeout` alone until that connection's Hello has arrived.
- **TCP connect:** the connection is closed as lost, exactly as if the peer
  had closed it (`onConnectionChange(false)`, `source_lost` drops), and the
  client redials after `reconnect_interval`. This catches a peer that hung or
  vanished without closing the connection, which TCP alone may not notice
  for a long time.
- **TCP listen:** that one connection is closed and its tracks are dropped
  (see Transport roles); the other connections are unaffected.

libskycue also sets `SO_KEEPALIVE` on every TCP socket as a backstop.

## Framing over UDP and TCP

- **UDP:** each datagram carries exactly one frame. The payload is at most
  `kMaxUdpPayload` = 1200 bytes, so a datagram is at most 1208 bytes. A
  receiver drops any datagram whose size is not `8 + length`.
- **TCP:** frames are concatenated on the byte stream with no extra delimiter.
  The receiver buffers incoming bytes and decodes repeatedly: `ok` consumes
  `8 + length` bytes; `need_more` (partial header or partial payload) waits for
  more data without consuming anything; `bad_magic` or `bad_version` means the
  stream cannot be resynchronised and the connection should be closed.
