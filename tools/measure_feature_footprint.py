#!/usr/bin/env python3
"""Measure the per-feature flash/static-RAM footprint of the Honch ESP-IDF SDK.

For each compile-time feature toggle (``HONCH_ENABLE_*``, surfaced on ESP-IDF as
``CONFIG_HONCH_*``) this builds the ``libhonch.a`` archive twice — once with the
feature on and once off — and attributes the difference to that feature. The
number we publish to the wizard is the *marginal* cost: how much flash / static
RAM you save by turning the feature off, measured on the real toolchain.

Why the archive and not the linked app: ``libhonch.a`` is the SDK's own code and
data, isolated from the ~400 KB of Wi-Fi/TLS/HTTP an app drags in. We only need
the archive, so we ``reconfigure`` + ``ninja`` that one target instead of doing a
full link — seconds per config, not minutes.

Run it on Citadel (the canonical ESP-IDF v6.0.1 host), inside the IDF env:

    source ~/esp/esp-idf-v6.0.1/export.sh
    ./tools/measure_feature_footprint.py --out ports/esp-idf/footprint/feature-footprint.json

The emitted JSON is the source of truth the wizard vendors; it is stamped with
the IDF version, target, SDK commit, and the exact toggle each number reflects.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FOOTPRINT_DIR = ROOT / "ports" / "esp-idf" / "footprint"
ARCHIVE_REL = Path("esp-idf") / "honch" / "libhonch.a"
SIZE_TOOL = "xtensa-esp32-elf-size"
IDF_TARGET = "esp32"

# Build configs: the all-on baseline, one per feature turned off, and a
# core-only floor (everything optional off). Each entry's `defaults` lines are
# appended to a pinned sdkconfig defaults file so the only thing that varies is
# the feature under test (and the optimization level, pinned to -Os for parity
# with production firmware).
PINNED_DEFAULTS = ['CONFIG_COMPILER_OPTIMIZATION_SIZE=y']

CONFIGS = {
    "baseline": [],
    "error_tracking_off": ["CONFIG_HONCH_ERROR_TRACKING=n"],
    "lifecycle_off": ["CONFIG_HONCH_LIFECYCLE_EVENTS=n"],
    "sessions_off": ["CONFIG_HONCH_SESSIONS=n"],
    "battery_off": ["CONFIG_HONCH_BATTERY=n"],
    "core_only": [
        "CONFIG_HONCH_ERROR_TRACKING=n",
        "CONFIG_HONCH_LIFECYCLE_EVENTS=n",
        "CONFIG_HONCH_SESSIONS=n",
        "CONFIG_HONCH_BATTERY=n",
    ],
}

# Maps an "<feature>_off" build to the wizard-facing feature id it measures.
FEATURE_OF_CONFIG = {
    "error_tracking_off": "error-tracking",
    "lifecycle_off": "lifecycle",
    "sessions_off": "sessions",
    "battery_off": "battery",
}

_SECTION_RE = re.compile(r"^(\.\S+)\s+(\d+)\s+\d+\s*$")

_FLASH_PREFIXES = (".text", ".literal", ".rodata", ".flash")
_RAM_PREFIXES = (".data", ".bss", ".dram", ".iram", ".noinit", ".rtc")


def _bucket_archive_sizes(size_output: str) -> dict[str, int]:
    """Sum allocated sections in `size -A <archive>` output into flash/ram.

    Non-allocated metadata (`.debug*`, `.comment`, `.xt.*`, `.xtensa.info`, the
    symbol/string tables) is not flashed onto the device, so it is excluded.
    """
    flash = 0
    ram = 0
    for line in size_output.splitlines():
        match = _SECTION_RE.match(line.strip())
        if not match:
            continue
        name, raw = match.group(1), int(match.group(2))
        if name.startswith(_FLASH_PREFIXES):
            flash += raw
        elif name.startswith(_RAM_PREFIXES):
            ram += raw
        # everything else (.debug*, .xt.*, .comment, .xtensa.info, ...) is
        # non-allocated metadata — intentionally ignored.
    return {"flash_bytes": flash, "ram_bytes": ram}


def _run(cmd: list[str], *, cwd: Path, log: Path) -> None:
    with log.open("w", encoding="utf-8") as handle:
        result = subprocess.run(cmd, cwd=cwd, stdout=handle, stderr=subprocess.STDOUT)
    if result.returncode != 0:
        sys.stderr.write(f"command failed ({result.returncode}): {' '.join(cmd)}\n")
        sys.stderr.write(log.read_text(encoding="utf-8")[-2000:])
        raise SystemExit(1)


def _measure_config(name: str, defaults: list[str], work: Path) -> dict[str, int]:
    build_dir = work / f"build-{name}"
    defaults_file = work / f"sdkconfig.{name}.defaults"
    defaults_file.write_text("\n".join(PINNED_DEFAULTS + defaults) + "\n", encoding="utf-8")

    if build_dir.exists():
        subprocess.run(["rm", "-rf", str(build_dir)], check=True)

    common = [
        "idf.py",
        "-C", str(FOOTPRINT_DIR),
        "-B", str(build_dir),
        "-D", f"IDF_TARGET={IDF_TARGET}",
        "-D", "HONCH_FOOTPRINT_WITH_SDK=ON",
        "-D", f"SDKCONFIG={build_dir / 'sdkconfig'}",
        "-D", f"SDKCONFIG_DEFAULTS={defaults_file}",
    ]
    _run(common + ["reconfigure"], cwd=FOOTPRINT_DIR, log=work / f"{name}-cfg.log")
    _run(["ninja", "-C", str(build_dir), str(ARCHIVE_REL)], cwd=FOOTPRINT_DIR, log=work / f"{name}-build.log")

    archive = build_dir / ARCHIVE_REL
    out = subprocess.run(
        [SIZE_TOOL, "-A", str(archive)], capture_output=True, text=True, check=True
    ).stdout
    sizes = _bucket_archive_sizes(out)
    print(f"  {name:<20} flash={sizes['flash_bytes']:>7}  ram={sizes['ram_bytes']:>6}")
    return sizes


def _sdk_commit() -> str:
    try:
        return subprocess.run(
            ["git", "-C", str(ROOT), "rev-parse", "--short", "HEAD"],
            capture_output=True, text=True, check=True,
        ).stdout.strip()
    except Exception:
        return "unknown"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=FOOTPRINT_DIR / "feature-footprint.json")
    parser.add_argument("--work", type=Path, default=Path("/tmp/honch-feature-footprint"))
    args = parser.parse_args()

    args.work.mkdir(parents=True, exist_ok=True)
    print(f"Measuring feature footprint (target={IDF_TARGET}, -Os)...")
    measured = {name: _measure_config(name, defaults, args.work) for name, defaults in CONFIGS.items()}

    baseline = measured["baseline"]
    core_only = measured["core_only"]

    features = {}
    for config_name, feature_id in FEATURE_OF_CONFIG.items():
        off = measured[config_name]
        features[feature_id] = {
            "flash_bytes": baseline["flash_bytes"] - off["flash_bytes"],
            "ram_bytes": baseline["ram_bytes"] - off["ram_bytes"],
        }

    sum_flash = sum(f["flash_bytes"] for f in features.values())
    optional_flash = baseline["flash_bytes"] - core_only["flash_bytes"]
    # Marginal deltas aren't perfectly additive (shared helper code), but they
    # should stay close to (baseline - core_only). A large gap means a feature's
    # "off" build didn't actually strip it — guard against that silently shipping.
    divergence = abs(sum_flash - optional_flash)
    tolerance = max(512, round(optional_flash * 0.10))

    report = {
        "target": IDF_TARGET,
        "idf_version": "6.0.1",
        "sdk_commit": _sdk_commit(),
        "optimization": "-Os",
        "method": (
            "Per-feature marginal footprint of libhonch.a: (all features on) minus "
            "(one feature off), summing allocated archive sections only "
            "(.text/.literal/.rodata = flash; .data/.bss/.iram = static RAM; "
            "debug/metadata excluded). RAM is static .bss/.data only — runtime "
            "queue/buffer RAM is config-driven and not counted here."
        ),
        "baseline_all_on": baseline,
        "core_only": core_only,
        "features": features,
        "consistency": {
            "sum_of_feature_flash_deltas": sum_flash,
            "baseline_minus_core_only_flash": optional_flash,
            "divergence_bytes": divergence,
            "tolerance_bytes": tolerance,
            "note": "deltas are not perfectly additive — features share helper code",
        },
    }
    args.out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"\nWrote {args.out}")
    print(json.dumps(report["features"], indent=2))
    if divergence > tolerance:
        sys.stderr.write(
            f"\nERROR: feature-delta sum ({sum_flash} B) diverges from baseline-core "
            f"({optional_flash} B) by {divergence} B > {tolerance} B tolerance — "
            "a feature's 'off' build may not be stripping it. Investigate before publishing.\n"
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
