#!/usr/bin/env python3
"""Publish a built firmware into docs/firmware/ for the web installer.

The installer at docs/index.html serves the firmware straight off GitHub Pages
so nobody has to download, unzip or compile anything. This script copies the
two build outputs into that folder under a stable name and keeps
docs/firmware/manifest.json in step.

    tools/publish_firmware.py --variant default \
        --title "Default (Mt Kanigan and neighbours)" \
        --description "Queensland networks around Mt Kanigan, Mt Glorious and Barcaldine." \
        --raw firmware.bin --packed firmware.packed.bin

Run it once per variant; the manifest accumulates entries in the order given by
--order (lower first). Standard library only.
"""

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FWDIR = ROOT / "docs" / "firmware"
MANIFEST = FWDIR / "manifest.json"


def station_count(header: Path) -> int | None:
    """Read ALERT_STATIONS_COUNT out of the generated station table."""
    try:
        m = re.search(r"#define\s+ALERT_STATIONS_COUNT\s+(\d+)", header.read_text())
        return int(m.group(1)) if m else None
    except OSError:
        return None


def station_addresses(header: Path) -> int | None:
    try:
        m = re.search(r"Addresses covered:\s*(\d+)", header.read_text())
        return int(m.group(1)) if m else None
    except OSError:
        return None


def git(*args: str) -> str:
    try:
        return subprocess.run(["git", "-C", str(ROOT), *args],
                              capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return ""


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--variant", required=True, help="short id, e.g. 'default'")
    ap.add_argument("--title", required=True, help="name shown on the installer card")
    ap.add_argument("--description", required=True, help="one line under the name")
    ap.add_argument("--raw", default="firmware.bin")
    ap.add_argument("--packed", default="firmware.packed.bin")
    ap.add_argument("--order", type=int, default=50, help="sort key on the page")
    ap.add_argument("--header", default="app/alert_stations_gen.h",
                    help="generated station table, read for the station count")
    ap.add_argument("--provenance", default=None,
                    help="JSON from gen_stations.py --provenance-out; folded into "
                         "the manifest so the installer can still say which MegaNet "
                         "snapshot a build came from")
    args = ap.parse_args()

    raw = Path(args.raw)
    packed = Path(args.packed)
    for path in (raw, packed):
        if not path.is_file():
            print(f"error: {path} not found - build the firmware first", file=sys.stderr)
            return 1

    FWDIR.mkdir(parents=True, exist_ok=True)
    base = f"quansheng-alert-{args.variant}"
    shutil.copyfile(raw, FWDIR / f"{base}.bin")
    shutil.copyfile(packed, FWDIR / f"{base}.packed.bin")

    provenance = {}
    if args.provenance:
        try:
            provenance = json.loads(Path(args.provenance).read_text())
        except (OSError, json.JSONDecodeError) as err:
            print(f"warning: could not read {args.provenance}: {err}", file=sys.stderr)

    packed_bytes = packed.read_bytes()
    entry = {
        "id": args.variant,
        "title": args.title,
        "description": args.description,
        "file": f"firmware/{base}.packed.bin",
        "raw_file": f"firmware/{base}.bin",
        "size": len(packed_bytes),
        "raw_size": raw.stat().st_size,
        "sha256": hashlib.sha256(packed_bytes).hexdigest(),
        "stations": station_count(ROOT / args.header),
        "addresses": station_addresses(ROOT / args.header),
        "commit": git("rev-parse", "--short", "HEAD"),
        "built": datetime.now(timezone.utc).strftime("%Y-%m-%d"),
        "order": args.order,
    }
    if provenance:
        entry["stations_from"] = {
            "repo": provenance.get("repo"),
            "commit": provenance.get("commit"),
            "date": provenance.get("date"),
            "fingerprint": provenance.get("fingerprint"),
        }

    manifest = {"builds": []}
    if MANIFEST.is_file():
        try:
            manifest = json.loads(MANIFEST.read_text())
        except json.JSONDecodeError:
            pass
    builds = [b for b in manifest.get("builds", []) if b.get("id") != args.variant]
    builds.append(entry)
    builds.sort(key=lambda b: (b.get("order", 50), b.get("id", "")))

    MANIFEST.write_text(json.dumps({
        "generated": datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M UTC"),
        "commit": entry["commit"],
        "builds": builds,
    }, indent=2) + "\n")

    print(f"published {base}: {entry['size']} bytes packed, "
          f"{entry['stations']} sites, commit {entry['commit'] or 'unknown'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
