#!/usr/bin/env python3
"""Helpers for make-tiles.sh: merge a freshly cut tile set into the tile
directory without losing tiles from earlier runs, and keep meta.json's list
of regions.

  tiles-merge.py merge STAGE OUT [--palette]
      Moves STAGE/{z}/{x}/{y}.png into OUT. A tile that is not in OUT yet is
      moved in. A tile that OUT already has is composited: the existing
      pixels stay where they are opaque and the new tile shows through where
      they are (partly) transparent, so the edge tiles of two neighbouring
      regions join up. --palette writes the new and composited tiles as
      256-colour PNGs (about a seventh of the size of gdal2tiles' RGBA
      ones). Compositing and --palette need Pillow.

  tiles-merge.py check META
      Exits 1 with a message when META exists but is not a JSON object
      (make-tiles.sh runs this before cutting any tile).

  tiles-merge.py meta META NAME WEST SOUTH EAST NORTH MINZ MAXZ GENERATED
      Adds region NAME to META (created when missing) and rewrites it. An
      entry with the same name, or with the same bbox and zoom range, is
      replaced. A meta.json in the old single-region form becomes a region
      named "default"; region entries that are not {name, bbox: [w, s, e,
      n], minZoom, maxZoom} are dropped with a warning. The top level keeps
      minZoom/maxZoom (the overall range), bbox (the union), source and
      generated.
"""
import json
import numbers
import os
import shutil
import sys

SOURCE = "Natural Earth 1:10m"


def die(msg):
    sys.stderr.write("tiles-merge: %s\n" % msg)
    sys.exit(1)


def _num(text):
    v = float(text)
    return int(v) if v == int(v) else v


# ---- merge -------------------------------------------------------------------

_Image = None


def _pil(why):
    global _Image
    if _Image is None:
        try:
            from PIL import Image
        except ImportError:
            die("%s needs Pillow (python3-pillow, or pip install Pillow)" % why)
        _Image = Image
    return _Image


def _save(im, path, palette):
    """Write im (RGBA) to path, as a 256-colour PNG with palette."""
    Image = _pil("writing %s" % path)
    if palette:  # fast octree: about 2/255 mean error on these tiles
        # Image.Quantize is Pillow >= 9.1; older ones have the constant on Image.
        im = im.quantize(256, method=getattr(Image, "Quantize", Image).FASTOCTREE)
    tmp = path + ".part"
    im.save(tmp, "PNG", optimize=True)
    os.replace(tmp, path)


def merge(stage, out, palette=False):
    moved = merged = kept = 0
    for z in sorted(os.listdir(stage)):
        if not z.isdigit() or not os.path.isdir(os.path.join(stage, z)):
            continue
        for x in sorted(os.listdir(os.path.join(stage, z))):
            sdir = os.path.join(stage, z, x)
            if not x.isdigit() or not os.path.isdir(sdir):
                continue
            ddir = os.path.join(out, z, x)
            os.makedirs(ddir, exist_ok=True)
            for name in sorted(os.listdir(sdir)):
                if not name.endswith(".png"):
                    continue
                src, dst = os.path.join(sdir, name), os.path.join(ddir, name)
                if not os.path.exists(dst):
                    if palette:
                        with _pil("--palette").open(src) as im:
                            _save(im.convert("RGBA"), dst, True)
                        os.remove(src)
                    else:
                        shutil.move(src, dst)  # the cache may be on another filesystem
                    moved += 1
                    continue
                Image = _pil("compositing %s" % dst)
                with Image.open(dst) as f:
                    old = f.convert("RGBA")
                if old.getextrema()[3][0] == 255:  # already opaque: nothing to add
                    kept += 1
                    continue
                with Image.open(src) as f:
                    new = f.convert("RGBA")
                if new.getextrema()[3][1] == 0 or new.size != old.size:
                    kept += 1
                    continue
                _save(Image.alpha_composite(new, old), dst, palette)
                merged += 1
    print("tiles-merge: %d new tiles, %d composited with existing ones, %d existing kept as is"
          % (moved, merged, kept))


# ---- meta.json ---------------------------------------------------------------

def _is_num(v):
    return isinstance(v, numbers.Real) and not isinstance(v, bool)


def valid_region(r):
    if not isinstance(r, dict) or not isinstance(r.get("name"), str) or not r["name"]:
        return False
    b, lo, hi = r.get("bbox"), r.get("minZoom"), r.get("maxZoom")
    return (isinstance(b, list) and len(b) == 4 and all(_is_num(v) for v in b) and
            b[0] < b[2] and b[1] < b[3] and isinstance(lo, int) and isinstance(hi, int) and
            not isinstance(lo, bool) and not isinstance(hi, bool) and 0 <= lo <= hi)


def read_meta(path):
    """META's object, {} when missing; dies when it is not a JSON object."""
    if not os.path.exists(path):
        return {}
    try:
        with open(path) as f:
            meta = json.load(f)
    except (OSError, ValueError) as e:
        die("cannot read %s: %s (fix or delete it)" % (path, e))
    if not isinstance(meta, dict):
        die("%s is not a JSON object (fix or delete it)" % path)
    return meta


def load_regions(path):
    meta = read_meta(path)
    if isinstance(meta.get("regions"), list):
        regions = []
        for r in meta["regions"]:
            if valid_region(r):
                regions.append({"name": r["name"], "bbox": r["bbox"], "minZoom": r["minZoom"],
                                "maxZoom": r["maxZoom"]})
            else:
                sys.stderr.write("tiles-merge: %s: dropping malformed region %s\n"
                                 % (path, json.dumps(r)[:200]))
        return regions
    old = dict(meta, name="default")
    if "regions" not in meta and valid_region(old):
        return [{"name": "default", "bbox": meta["bbox"], "minZoom": meta["minZoom"],
                 "maxZoom": meta["maxZoom"]}]
    return []


def meta_json(regions, generated):
    bboxes = [r["bbox"] for r in regions]
    union = [min(b[0] for b in bboxes), min(b[1] for b in bboxes),
             max(b[2] for b in bboxes), max(b[3] for b in bboxes)]
    meta = {
        "minZoom": min(r["minZoom"] for r in regions),
        "maxZoom": max(r["maxZoom"] for r in regions),
        "bbox": union,
        "regions": regions,
        "source": SOURCE,
        "generated": generated,
    }
    return json.dumps(meta, separators=(",", ":"))


def meta(path, name, west, south, east, north, minz, maxz, generated):
    region = {"name": name, "bbox": [_num(west), _num(south), _num(east), _num(north)],
              "minZoom": int(minz), "maxZoom": int(maxz)}
    regions = [r for r in load_regions(path)
               if r.get("name") != name and
               not (r.get("bbox") == region["bbox"] and r.get("minZoom") == region["minZoom"] and
                    r.get("maxZoom") == region["maxZoom"])]
    regions.append(region)
    text = meta_json(regions, generated)
    tmp = path + ".part"
    with open(tmp, "w") as f:
        f.write(text + "\n")
    os.replace(tmp, path)
    print("tiles-merge: %s: %s" % (path, ", ".join(
        "%s z%s-%s" % (r.get("name", "?"), r.get("minZoom"), r.get("maxZoom")) for r in regions)))


def main(argv):
    if len(argv) == 3 and argv[1] == "check":
        load_regions(argv[2])
    elif len(argv) in (4, 5) and argv[1] == "merge" and argv[4:] in ([], ["--palette"]):
        merge(argv[2], argv[3], palette=argv[4:] == ["--palette"])
    elif len(argv) == 11 and argv[1] == "meta":
        meta(*argv[2:])
    else:
        sys.stderr.write(__doc__)
        sys.exit(2)


if __name__ == "__main__":
    main(sys.argv)
