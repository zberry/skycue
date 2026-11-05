# skycue

Source and releases: <https://github.com/zberry/skycue>

skycue is an operator console and forwarding bridge for ADS-B and UAT
receivers. It runs beside the receiver stack you already have (readsb, and
optionally dump978-fa), reads what those decoders already decode, shows it
on a live map in the browser, and forwards selected aircraft as cues to
your own programs and devices. It does not feed any network, replace your decoders or
change their configuration: your feeder keeps running exactly as it was.

<!-- screenshot: docs/screenshot.png (the map with traffic, the side panel open) -->

What it does:

- Reads readsb's JSON position output, dump978-fa's UAT uplinks and pingStation 3 receivers into one track table.
- Decides per aircraft what to forward, by range, elevation angle and allow and block lists you can change live.
- Sends cues to cue clients over TCP or UDP in a small documented wire protocol ([docs/protocol.md](docs/protocol.md)).
- Serves an offline web map: decision colours, trails, a track table, alerts and the health of every input.
- Decodes FIS-B weather from the 978 MHz uplinks: NEXRAD, METAR, TAF, PIREP, winds aloft and NOTAMs.
- Keeps statistics across restarts and records what it received and sent, for replay and offline analysis.

## Install

Debian and Raspberry Pi OS packages (arm64, amd64), RHEL 8 and 9 packages
(x86_64), a plain CMake build from source, and how to turn on the decoder
output skycue reads for readsb, ultrafeeder and PiAware:
[docs/installing.md](docs/installing.md). A full-stack offline bundle that also builds the SDR decoders lives in a separate repository.

Running it, the configuration keys and the web page are described in
[docs/operating.md](docs/operating.md); the HTTP API in
[docs/web-api.md](docs/web-api.md); the library for building your own
cue client in [docs/libskycue.md](docs/libskycue.md).

## Status

Version 0.1.0, the first public release. The configuration file and the
cue protocol may still change between minor versions; each release says
what changed.

## Licence

Apache-2.0: see [LICENSE](LICENSE) and [NOTICE](NOTICE). The web page
includes Leaflet (BSD-2-Clause) and Apache ECharts (Apache-2.0, with parts
derived from D3 under BSD-3-Clause); the maps are baked from Natural Earth
(public domain).
