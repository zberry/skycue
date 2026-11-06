# make-locations.py: report locations from OurAirports

METARs, TAFs, winds and PIREPs name a location (`KPRG`) without coordinates.
skycued learns positions from decoded NOTAMs and TFRs; this script adds a
starting set from OurAirports, so more reports can be placed on the map.
It is optional.

## 1. Download (on a machine with network access)

OurAirports data is public domain. Fetch two files:

- https://davidmegginson.github.io/ourairports-data/airports.csv
- https://davidmegginson.github.io/ourairports-data/navaids.csv

Copy them to the machine that runs the script. Python 3.6 or later is
enough; nothing else is needed.

## 2. Build

    python3 -I scripts/maps/make-locations.py airports.csv navaids.csv -o locations.csv

Without `-o` the CSV goes to stdout. The output is `id,lat,lon` lines (six
decimals, sorted by id, no header). A summary goes to stderr:

    make-locations: N locations (A airports, V navaids), D ambiguous dropped, C closed skipped, I invalid skipped

- Closed airports are skipped. Each airport gives its `ident`, plus its
  `gps_code` and `icao_code` when set and different.
- Ids are upper-cased and must be 2 to 5 letters or digits; others are skipped.
  A row none of whose ids is kept (every id bad, or no valid position) is
  counted in the summary's "invalid skipped".
- An id used for places more than 1 nm apart (a navaid and an airport with
  the same ident on different continents, say) is ambiguous and dropped.
- In the summary, an id that is both an airport and a navaid within 1 nm
  counts once, as an airport (airports are read first, and the first place
  read gives the position and the kind).
- There is no region filter: every country in the input is kept. FIS-B
  only names US, Canadian and nearby ids, but a world-wide file still fits
  (see the cap below), and the extra rows cost only memory.
- Size cap: skycued reads at most 250,000 `LOCATIONS_FILE` ids; past that
  it ignores the rest (the end of the sorted file) and logs one WARN with
  the count. Full OurAirports data gives well under that (in the order of
  100,000 ids). When the output has more rows than the cap the script still
  writes them all and adds
  `make-locations: warning: N rows is more than the 250000 skycued reads ...`
  to the summary. Positions skycued learns itself have their own cap,
  20,000 ids.
- `--help` prints the usage.
- Exit status: 0 done, 1 unreadable file, 2 usage or a CSV missing a
  required column (the message names the file and the column).

## 3. Use

Set `LOCATIONS_FILE=/path/locations.csv` in `skycue.conf` and restart
skycued. Positions skycued learns from the air win over the file for the
same id.

The output is a local file; never commit it.
