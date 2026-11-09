#!/usr/bin/env python3
"""Generate the fuzz seeds that are cut from third-party captures.

    python3 -I scripts/test-data/gen-capture-seeds.py TEST_DATA_DIR OUT_DIR

TEST_DATA_DIR is the tests/data tree (raw/*.gz, fisb/tg30.978,
fisb/received-with-uplinks.jsonl). Seeds are written under OUT_DIR/<target>/
with the names and bytes listed in docs/hardening.md ("Seeds"):

    raw_formats/seed-1..18       iqgen_coding/seed-1,2
    fisb_decoder/seed-1..12      recording/seed-1..6

A target whose source sample is missing gets nothing written; the script
says so on stderr and exits 3 (other targets are still generated). Output is
deterministic.
"""
import gzip
import os
import sys

SKIP_EXIT = 3


def read_lines(path, opener):
    with opener(path, "rb") as f:
        data = f.read()
    return data.split(b"\n")


def gz_lines(path):
    return read_lines(path, gzip.open)


def plain_lines(path):
    return read_lines(path, open)


def strip_cr(line):
    return line[:-1] if line.endswith(b"\r") else line


def put(out, target, name, data):
    d = os.path.join(out, target)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, name), "wb") as f:
        f.write(data)


def raw_formats(fx, out):
    base = os.path.join(fx, "raw")
    uat = os.path.join(base, "uat-sample.txt.gz")
    stratux = os.path.join(base, "stratux-uat-2015.log.gz")
    modes = os.path.join(base, "modes-120s.csv.gz")
    for p in (uat, stratux, modes):
        if not os.path.isfile(p):
            return "missing " + os.path.relpath(p, fx)
    seeds = []
    seeds += [b"0" + strip_cr(l) for l in gz_lines(uat)[:6]]
    nonblank = [strip_cr(l) for l in gz_lines(stratux) if strip_cr(l).strip()]
    seeds += [b"0" + l for l in nonblank[:6]]
    seeds += [b"2" + strip_cr(l) for l in gz_lines(modes)[:6]]
    for i, s in enumerate(seeds, 1):
        put(out, "raw_formats", "seed-%d.txt" % i, s)
    return None


def fisb_and_iqgen(fx, out):
    p = os.path.join(fx, "fisb", "tg30.978")
    if not os.path.isfile(p):
        return "missing fisb/tg30.978"
    lines = [strip_cr(l) for l in plain_lines(p)]

    def line(n):
        return lines[n - 1]

    def payload(n):
        return bytes.fromhex(line(n).split(b";")[0][1:].decode("ascii"))

    # decode seeds: mode 0, decoder byte (1 = experimental), raw bytes
    for i, (n, dec) in enumerate(
            [(1, b"0"), (5, b"1"), (59, b"0"), (15, b"0"), (365, b"1"), (631, b"0")], 1):
        put(out, "fisb_decoder", "seed-%d.bin" % i, b"0" + dec + payload(n))
    # text seeds: mode 1, decoder byte, the whole line
    for i, (n, dec) in enumerate(
            [(82, b"0"), (42, b"1"), (46, b"0"), (701, b"0"), (777, b"1"), (301, b"0")], 7):
        put(out, "fisb_decoder", "seed-%d.txt" % i, b"1" + dec + line(n))
    # iqgen: mode 0, code byte, corruption index, payload prefix
    put(out, "iqgen_coding", "seed-1.bin", b"0\x00\x05" + payload(1)[:18])
    put(out, "iqgen_coding", "seed-2.bin", b"0\x01\x2a" + payload(5)[:34])
    return None


def recording(fx, out):
    p = os.path.join(fx, "fisb", "received-with-uplinks.jsonl")
    if not os.path.isfile(p):
        return "missing fisb/received-with-uplinks.jsonl"
    for i, l in enumerate(plain_lines(p)[:6], 1):
        put(out, "recording", "seed-%d.jsonl" % i, b"0" + strip_cr(l))
    return None


def main(argv):
    if len(argv) != 3:
        sys.stderr.write("usage: gen-capture-seeds.py TEST_DATA_DIR OUT_DIR\n")
        return 2
    fx, out = argv[1], argv[2]
    missing = []
    for fn in (raw_formats, fisb_and_iqgen, recording):
        err = fn(fx, out)
        if err:
            missing.append(err)
    for m in missing:
        sys.stderr.write("gen-capture-seeds: %s\n" % m)
    return SKIP_EXIT if missing else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
