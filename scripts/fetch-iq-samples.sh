#!/usr/bin/env bash
# Usage: scripts/fetch-iq-samples.sh [--cache DIR] [--offline]
#
# Fetches the one public off-air 1090 MHz IQ capture we use (Porto, Zenodo
# record 23342598, CC BY 4.0, see NOTICE), checks it against pinned SHA-256
# values and resamples it from 2.0 to 2.4 MS/s for readsb:
#
#   DIR/AWEC2026_ADSB_Data.zip     the Zenodo download (about 20 MB)
#   DIR/porto-awec2026.1090.cu8    the resampled capture (about 87 MB)
#
# DIR defaults to <repo>/build/iq-samples (git-ignored). The raw 73 MB
# recording is streamed out of the zip into the resampler and never written.
# Every step whose output already verifies is skipped, so a second run uses
# no network. --offline never downloads: place AWEC2026_ADSB_Data.zip in DIR
# by hand (on a machine without network access) and the script checks and
# resamples it, or says what is missing. Needs curl (only to download),
# python3, sha256sum and skycue-iqgen (<repo>/build/stage/bin, else PATH).
# The zip and recording hashes are strict; a resampled file whose hash differs
# from the pinned one (another libm or architecture) is kept with a warning.
# Exit status: 0 done, 1 failure (download, checksum, tool), 2 usage error.
set -euo pipefail

here=$(cd "$(dirname "$0")/.." && pwd)

zip_url=https://zenodo.org/api/records/23342598/files/AWEC2026_ADSB_Data.zip/content
zip_name=AWEC2026_ADSB_Data.zip
zip_sha=407f9d8ebf6451afb7ffa12c768286235f4791fdc8e03fce8136b621108114b9
member=AWEC2026_ADSB_Data/raw_data/recording.bin
member_sha=d0c8e0803bba352e29a870489b2fbe023a09071cee4410a5ab5f0dac6fac1c58
member_rate=2000000
out_name=porto-awec2026.1090.cu8
# skycue-iqgen resample is deterministic; this is its 2.4 MS/s output.
out_sha=d0a78f005bbf339c40183de67f76afe661d4146e851d947fb579c6a3cc05ec05

usage() {
  echo "usage: $0 [--cache DIR] [--offline]" >&2
  exit 2
}

cache=$here/build/iq-samples
offline=0
while [ $# -gt 0 ]; do
  case $1 in
    --cache)
      [ $# -ge 2 ] && [ -n "$2" ] || usage
      cache=$2
      shift 2
      ;;
    --offline)
      offline=1
      shift
      ;;
    -h | --help)
      sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *) usage ;;
  esac
done

die() {
  echo "fetch-iq-samples.sh: $*" >&2
  exit 1
}

# verify FILE SHA: 0 when FILE exists and has that SHA-256.
verify() {
  [ -f "$1" ] && printf '%s  %s\n' "$2" "$1" | sha256sum -c --status - 2>/dev/null
}

mkdir -p "$cache"
zip=$cache/$zip_name
out=$cache/$out_name

print_command() {
  cat <<EOF

Replay it (real time, 18.2 s; add --loop to repeat):
  bin/skycue-start.sh replay-iq $out -c CONFIG
The receiver position in CONFIG must be near Porto, or readsb cannot place
the aircraft and the range filter drops them, e.g.
  RECEIVER_LAT=41.15
  RECEIVER_LON=-8.61
EOF
}

if verify "$out" "$out_sha"; then
  echo "sha256 ok: $out"
  print_command
  exit 0
fi
rm -f "$out"

if verify "$zip" "$zip_sha"; then
  echo "sha256 ok: $zip"
else
  if [ -e "$zip" ]; then
    rm -f "$zip"
    die "$zip did not match its pinned SHA-256; deleted it"
  fi
  if [ "$offline" -eq 1 ]; then
    die "--offline and no $zip_name in $cache: download $zip_url there (SHA-256 $zip_sha) and run again"
  fi
  command -v curl >/dev/null || die "curl is needed to download $zip_url"
  echo "downloading $zip_url"
  rm -f "$zip.part"
  curl -fL --retry 3 --connect-timeout 30 --max-time 900 --silent --show-error -o "$zip.part" "$zip_url" || {
    rm -f "$zip.part"
    die "download failed"
  }
  if ! verify "$zip.part" "$zip_sha"; then
    rm -f "$zip.part"
    die "the download did not match its pinned SHA-256; deleted it"
  fi
  mv "$zip.part" "$zip"
  echo "sha256 ok: $zip"
fi

iqgen=$here/build/stage/bin/skycue-iqgen
if [ ! -x "$iqgen" ]; then
  iqgen=$(command -v skycue-iqgen) || die "skycue-iqgen not found in $here/build/stage/bin or PATH; build the superbuild first"
fi
command -v python3 >/dev/null || die "python3 is needed to read the zip"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# The member goes to stdout in 1 MiB pieces and its SHA-256 to a file, so the
# raw recording is checked without ever landing on disk.
python3 -I -c '
import hashlib, sys, zipfile
h = hashlib.sha256()
with zipfile.ZipFile(sys.argv[1]) as z, z.open(sys.argv[2]) as f:
    while True:
        b = f.read(1 << 20)
        if not b:
            break
        h.update(b)
        sys.stdout.buffer.write(b)
sys.stdout.flush()
with open(sys.argv[3], "w") as o:
    o.write(h.hexdigest() + "\n")
' "$zip" "$member" "$tmp/member.sha256" |
  "$iqgen" resample - "$member_rate" -o "$out" --out-rate 2400000 || {
  rm -f "$out"
  die "extracting $member or resampling it failed"
}

if [ "$(cat "$tmp/member.sha256")" != "$member_sha" ]; then
  rm -f "$out"
  die "$member in $zip_name did not match its pinned SHA-256"
fi
echo "sha256 ok: $member"
# The input is verified above, so a different output hash means a different
# resampler build: the pinned value comes from one libm and architecture, and
# floating-point results (the Blackman taps, the filter sums) may round
# differently elsewhere. Warn and keep the file; a later run re-derives it.
if verify "$out" "$out_sha"; then
  echo "sha256 ok: $out"
else
  actual=$(sha256sum "$out" | cut -d' ' -f1)
  {
    echo "fetch-iq-samples.sh: warning: the resampled $out_name has SHA-256 $actual,"
    echo "  not the pinned $out_sha."
    echo "  The zip and the recording matched their pinned hashes, so this is the resampler:"
    echo "  its output is byte-identical only on the same libm and architecture (or the"
    echo "  skycue-iqgen resampler changed). Keeping the file."
  } >&2
fi
print_command
