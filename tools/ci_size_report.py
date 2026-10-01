#!/usr/bin/env python3
"""Firmware size & resource-margin report for CI.

Combines three sources:
  - the linked app binary (.bin)   -> actual flash image size
  - partitions.csv                 -> app partition / total flash budget
  - esp-idf-size (json2, map file) -> per-region RAM usage (DIRAM/IRAM/RTC)

Non-app images (e.g. assets.bin against the `assets` data partition) can be
reported alongside via repeatable --extra-bin NAME=PATH; they are overflow-
gated (bin must fit the named partition) but carry no percentage gate.

Writes a markdown report to stdout (and $GITHUB_STEP_SUMMARY when set) and
exits non-zero when usage crosses the gate thresholds, so it can act as a
size gate in addition to the compile gate.

Usage:
  python tools/ci_size_report.py \
      --map build/esp32c6_car.map \
      --bin build/esp32c6_car.bin \
      --partitions partitions.csv \
      --extra-bin assets=build/assets.bin
"""

import argparse
import json
import os
import re
import subprocess
import sys

FLASH_SIZE_DEFINE_RE = re.compile(r"CONFIG_ESPTOOLPY_FLASHSIZE_(\d+)MB")
FLASH_SIZE_VALUE_RE = re.compile(r'CONFIG_ESPTOOLPY_FLASHSIZE\s*[=:]\s*"?(\d+)MB')
SIZE_RE = re.compile(r"^(\d+(?:\.\d+)?)([KM])?$", re.IGNORECASE)


def parse_size(text):
    """'6M' / '24K' / '0x1000' -> bytes."""
    text = text.strip()
    if text.lower().startswith("0x"):
        return int(text, 16)
    m = SIZE_RE.match(text)
    if not m:
        raise ValueError(f"cannot parse size: {text!r}")
    val = int(float(m.group(1)))
    mult = (m.group(2) or "").upper()
    return val * (1024 if mult == "K" else 1024 * 1024 if mult == "M" else 1)


def parse_partitions(csv_path):
    """-> (app_partitions, partitions_by_name, flash_used_end). Offsets required."""
    apps = []
    by_name = {}
    end = 0
    with open(csv_path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            fields = [x.strip() for x in line.split(",")]
            if len(fields) < 5:
                continue
            name, ptype, subtype, offset, size = fields[:5]
            if not offset:
                raise ValueError(
                    f"partition {name}: explicit offsets required by this script"
                )
            off, sz = int(offset, 16), parse_size(size)
            end = max(end, off + sz)
            by_name[name] = {"name": name, "type": ptype, "size": sz}
            if ptype == "app":
                apps.append(by_name[name])
    if not apps:
        raise ValueError("no app partition found")
    return apps, by_name, end


def detect_flash_mb(build_dir):
    """Read CONFIG_ESPTOOLPY_FLASHSIZE_<N>MB from the generated sdkconfig.h."""
    for cand in (
        os.path.join(build_dir, "config", "sdkconfig.h"),
        os.path.join(build_dir, "sdkconfig"),
    ):
        if not os.path.isfile(cand):
            continue
        with open(cand) as f:
            for line in f:
                m = FLASH_SIZE_DEFINE_RE.search(line) or FLASH_SIZE_VALUE_RE.search(line)
                if m:
                    return int(m.group(1))
    return None


def ram_regions(map_path):
    """Run esp-idf-size json2 on the map file -> layout list."""
    out = subprocess.run(
        [sys.executable, "-m", "esp_idf_size", map_path, "--format", "json2"],
        capture_output=True, text=True, check=True,
    ).stdout
    return json.loads(out)["layout"]


def human(n):
    for unit in ("B", "KiB", "MiB", "GiB"):
        if abs(n) < 1024 or unit == "GiB":
            return f"{n:.2f} {unit}" if unit != "B" else f"{n} B"
        n /= 1024


def pct(used, total):
    return 100.0 * used / total if total else 0.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--map", required=True)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--partitions", default="partitions.csv")
    ap.add_argument("--extra-bin", action="append", default=[],
                    metavar="PARTITION=PATH",
                    help="non-app image to report, e.g. assets=build/assets.bin")
    ap.add_argument("--flash-size", type=int, help="total flash in MB (default: autodetect)")
    ap.add_argument("--max-app-pct", type=float, default=90.0,
                    help="gate: max used %% of the largest app partition")
    ap.add_argument("--max-ram-pct", type=float, default=95.0,
                    help="gate: max used %% of DIRAM (static)")
    args = ap.parse_args()

    build_dir = os.path.dirname(os.path.abspath(args.map))
    flash_mb = args.flash_size or detect_flash_mb(build_dir)
    if not flash_mb:
        sys.exit("error: cannot detect flash size; pass --flash-size")

    apps, by_name, part_end = parse_partitions(args.partitions)
    app = max(apps, key=lambda a: a["size"])  # for OTA take the bigger slot
    bin_size = os.path.getsize(args.bin)
    flash_total = flash_mb * 1024 * 1024

    extras = []
    for spec in args.extra_bin:
        name, sep, path = spec.partition("=")
        if not sep or name not in by_name:
            sys.exit(f"error: --extra-bin needs PARTITION=PATH with a known "
                     f"partition, got {spec!r}")
        extras.append({"part": by_name[name], "path": path,
                       "size": os.path.getsize(path)})

    regions = ram_regions(args.map)
    by_reg = {r["name"]: r for r in regions}
    diram = by_reg.get("DIRAM")

    # ---- gate evaluation ----------------------------------------------------
    app_pct = pct(bin_size, app["size"])
    ram_pct = pct(diram["used"], diram["total"]) if diram else 0.0
    failures = []
    if app_pct > args.max_app_pct:
        failures.append(
            f"app image uses {app_pct:.1f}% of partition '{app['name']}' "
            f"(gate {args.max_app_pct:.0f}%)"
        )
    if diram and ram_pct > args.max_ram_pct:
        failures.append(
            f"static DIRAM usage {ram_pct:.1f}% (gate {args.max_ram_pct:.0f}%)"
        )
    for e in extras:
        if e["size"] > e["part"]["size"]:
            failures.append(
                f"{os.path.basename(e['path'])} ({human(e['size'])}) overflows "
                f"partition '{e['part']['name']}' ({human(e['part']['size'])})"
            )

    # ---- report -------------------------------------------------------------
    lines = []
    add = lines.append
    add("## 📦 Firmware size & resource margins")
    add("")
    add(f"App image `{os.path.basename(args.bin)}`: **{human(bin_size)}** "
        f"— `{app['name']}` app partition {human(app['size'])} → "
        f"used **{app_pct:.1f}%**, margin **{100 - app_pct:.1f}%** ({human(app['size'] - bin_size)} free)")
    add("")
    add("| Region | Used | Total | Used % | Margin % |")
    add("|---|---:|---:|---:|---:|")
    add(f"| `{app['name']}` app (flash) | {human(bin_size)} | {human(app['size'])} "
        f"| {app_pct:.1f}% | {100 - app_pct:.1f}% |")
    for e in extras:
        u, t = e["size"], e["part"]["size"]
        add(f"| {os.path.basename(e['path'])} → `{e['part']['name']}` (flash) "
            f"| {human(u)} | {human(t)} | {pct(u, t):.1f}% | {100 - pct(u, t):.1f}% |")
    add(f"| flash (all partitions) | {human(part_end)} | {human(flash_total)} "
        f"| {pct(part_end, flash_total):.1f}% | {100 - pct(part_end, flash_total):.1f}% |")
    for r in regions:
        if r["total"]:
            u, t = r["used"], r["total"]
            note = " *(fixed region, linker-enforced)*" if r["name"] == "IRAM" else ""
            add(f"| {r['name']}{note} | {human(u)} | {human(t)} "
                f"| {pct(u, t):.1f}% | {100 - pct(u, t):.1f}% |")
        else:  # flash-mapped regions: budget is the app partition
            add(f"| {r['name']} (flash-mapped) | {human(r['used'])} | app budget | — | — |")
    add("")
    add("RAM figures are link-time static usage (`.data`/`.bss`); runtime heap "
        "is not covered. flash (all partitions) is the partition-table "
        "footprint, not actual usage.")
    if failures:
        add("")
        for f in failures:
            add(f"❌ **gate failed**: {f}")
    else:
        add("")
        add(f"✅ size gates passed (app ≤ {args.max_app_pct:.0f}%, static DIRAM ≤ {args.max_ram_pct:.0f}%)")

    report = "\n".join(lines)
    print(report)
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a") as f:
            f.write(report + "\n")

    if failures:
        sys.exit(1)


if __name__ == "__main__":
    main()
