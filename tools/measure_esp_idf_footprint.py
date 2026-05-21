#!/usr/bin/env python3
"""Measure ESP-IDF Honch footprint from differential builds."""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[1]
FOOTPRINT_DIR = REPO_ROOT / "ports" / "esp-idf" / "footprint"
HONCH_ARCHIVE = "esp-idf/honch/libhonch.a"


def _memory_used(data: dict[str, Any], name: str) -> int:
    for item in data.get("layout", []):
        if item.get("name") == name:
            return int(item.get("used", 0))
    return 0


def _memory_type_size(memory_types: dict[str, Any], name: str) -> int:
    item = memory_types.get(name, {})
    return int(item.get("size", 0))


def parse_total_sizes(path: Path | str) -> dict[str, int]:
    with Path(path).open(encoding="utf-8") as f:
        return parse_total_sizes.from_data(json.load(f))


def _parse_total_sizes_from_data(data: dict[str, Any]) -> dict[str, int]:
    flash_code = _memory_used(data, "Flash Code")
    flash_data = _memory_used(data, "Flash Data")
    iram = _memory_used(data, "IRAM")
    dram = _memory_used(data, "DRAM")
    rtc_slow = _memory_used(data, "RTC SLOW")
    return {
        "app_image_bytes": int(data.get("total_size", 0)),
        "flash_code_bytes": flash_code,
        "flash_data_bytes": flash_data,
        "iram_bytes": iram,
        "dram_bytes": dram,
        "rtc_slow_bytes": rtc_slow,
        "static_ram_bytes": iram + dram + rtc_slow,
    }


parse_total_sizes.from_data = _parse_total_sizes_from_data  # type: ignore[attr-defined]


def parse_archive_sizes(path: Path | str, archive_suffix: str = HONCH_ARCHIVE) -> dict[str, int]:
    with Path(path).open(encoding="utf-8") as f:
        return parse_archive_sizes.from_data(json.load(f), archive_suffix)


def _parse_archive_sizes_from_data(data: dict[str, Any], archive_suffix: str) -> dict[str, int]:
    archive = None
    archive_name = ""
    for name, value in data.items():
        if name.endswith(archive_suffix):
            archive = value
            archive_name = name
            break
    if archive is None:
        return {
            "archive": archive_suffix,
            "total_bytes": 0,
            "flash_code_bytes": 0,
            "flash_data_bytes": 0,
            "iram_bytes": 0,
            "dram_bytes": 0,
            "rtc_slow_bytes": 0,
            "static_ram_bytes": 0,
        }

    memory_types = archive.get("memory_types", {})
    iram = _memory_type_size(memory_types, "IRAM")
    dram = _memory_type_size(memory_types, "DRAM")
    rtc_slow = _memory_type_size(memory_types, "RTC SLOW")
    return {
        "archive": archive_name,
        "total_bytes": int(archive.get("size", 0)),
        "flash_code_bytes": _memory_type_size(memory_types, "Flash Code"),
        "flash_data_bytes": _memory_type_size(memory_types, "Flash Data"),
        "iram_bytes": iram,
        "dram_bytes": dram,
        "rtc_slow_bytes": rtc_slow,
        "static_ram_bytes": iram + dram + rtc_slow,
    }


parse_archive_sizes.from_data = _parse_archive_sizes_from_data  # type: ignore[attr-defined]


RUNTIME_RE = re.compile(r"HONCH_FOOTPRINT_RUNTIME\s+(?P<body>.+)$")
CPU_RE = re.compile(r"HONCH_FOOTPRINT_CPU\s+(?P<body>.+)$")


def _parse_kv(body: str) -> dict[str, str]:
    result = {}
    for part in body.split():
        if "=" not in part:
            continue
        key, value = part.split("=", 1)
        result[key] = value
    return result


def _coerce_number(value: str) -> int | float | str:
    try:
        if "." in value:
            return float(value)
        return int(value)
    except ValueError:
        return value


def parse_monitor_log(text: str) -> dict[str, Any]:
    runtime: dict[str, Any] = {}
    for line in text.splitlines():
        runtime_match = RUNTIME_RE.search(line)
        if runtime_match:
            values = _parse_kv(runtime_match.group("body"))
            phase = values.pop("phase", "")
            if phase:
                runtime[phase] = {key: _coerce_number(value) for key, value in values.items()}
            continue

        cpu_match = CPU_RE.search(line)
        if cpu_match:
            values = _parse_kv(cpu_match.group("body"))
            runtime["track_cpu"] = {key: _coerce_number(value) for key, value in values.items()}

    before = runtime.get("before_honch")
    after_init = runtime.get("after_honch_init")
    if before and after_init:
        runtime["heap_delta_after_init_bytes"] = int(before["heap_free"]) - int(after_init["heap_free"])

    return runtime


def _diff_sizes(honch: dict[str, int], bare: dict[str, int]) -> dict[str, int]:
    return {key: honch.get(key, 0) - bare.get(key, 0) for key in honch}


def _less_than_kb(value: int, minimum_kb: int = 1) -> str:
    kb = max(minimum_kb, (value + 1023) // 1024)
    for tier in (1, 2, 4, 8, 10, 20, 32, 48, 64, 96, 128, 192, 256):
        if kb <= tier:
            return f"<{tier} KB"
    return f"<{kb} KB"


def build_report(
    *,
    target: str,
    bare: dict[str, int],
    honch: dict[str, int],
    honch_archive: dict[str, int],
    runtime: dict[str, Any] | None,
    connected: dict[str, int] | None = None,
) -> dict[str, Any]:
    baseline = connected or bare
    delta = _diff_sizes(honch, baseline)
    minimal_delta = _diff_sizes(honch, bare)
    connected_delta_valid = connected is not None and all(value >= 0 for value in delta.values())
    claims = {
        "sdk_flash": _less_than_kb(honch_archive["total_bytes"]),
        "sdk_static_ram": _less_than_kb(honch_archive["static_ram_bytes"], minimum_kb=1),
    }
    if runtime and "heap_delta_after_init_bytes" in runtime:
        claims["runtime_heap_after_init"] = _less_than_kb(runtime["heap_delta_after_init_bytes"])
    if runtime and "track_cpu" in runtime:
        cpu = runtime["track_cpu"].get("cpu_pct_1hz")
        if isinstance(cpu, (int, float)):
            claims["cpu_at_1hz"] = f"<{max(0.01, cpu * 1.25):.2f}%"

    return {
        "target": target,
        "method": "ESP-IDF map analysis: direct libhonch.a archive contribution for landing SDK flash/static-RAM claims; Honch app minus minimal baseline for worst-case dependency pull-in; connected baseline delta is diagnostic only when it is non-negative.",
        "minimal_baseline": bare,
        "connected_baseline": connected or {},
        "with_honch": honch,
        "delta": delta,
        "connected_delta_valid_for_claims": connected_delta_valid,
        "minimal_delta": minimal_delta,
        "direct_honch_archive": honch_archive,
        "runtime": runtime or {},
        "landing_claims": claims,
    }


def _run(cmd: list[str], cwd: Path) -> None:
    print("+", " ".join(cmd), flush=True)
    subprocess.run(cmd, cwd=cwd, check=True)


def _idf_py() -> list[str]:
    idf_path = os.environ.get("IDF_PATH")
    if idf_path:
        idf = Path(idf_path) / "tools" / "idf.py"
        if idf.exists():
            return [sys.executable, str(idf)]
    return ["idf.py"]


def build_variant(target: str, build_dir: str, *, with_honch: bool, connected_baseline: bool = False) -> Path:
    args = _idf_py() + [
        "-B",
        build_dir,
        "-D",
        f"IDF_TARGET={target}",
        "-D",
        f"HONCH_FOOTPRINT_WITH_SDK={'ON' if with_honch else 'OFF'}",
        "-D",
        f"HONCH_FOOTPRINT_CONNECTED_BASELINE={'ON' if connected_baseline else 'OFF'}",
        "build",
    ]
    _run(args, FOOTPRINT_DIR)
    return FOOTPRINT_DIR / build_dir / "honch_footprint.map"


def write_size_json(map_path: Path, output_path: Path, archives: bool = False) -> None:
    cmd = [sys.executable, "-m", "esp_idf_size"]
    if archives:
        cmd.append("--archives")
    cmd += ["--format", "json2", "-o", str(output_path), str(map_path)]
    _run(cmd, FOOTPRINT_DIR)


def print_summary(report: dict[str, Any]) -> None:
    delta = report["delta"]
    minimal_delta = report["minimal_delta"]
    archive = report["direct_honch_archive"]
    claims = report["landing_claims"]
    print("")
    print("Honch ESP-IDF footprint")
    print(f"Target: {report['target']}")
    if report["connected_delta_valid_for_claims"]:
        print(f"Connected app image delta: {delta['app_image_bytes']} bytes")
        print(f"Connected static DRAM delta: {delta['dram_bytes']} bytes")
    else:
        print(
            "Connected app delta is diagnostic only: baseline is larger than Honch app, "
            "so it is not used for landing claims."
        )
        print(f"Connected app image delta: {delta['app_image_bytes']} bytes")
        print(f"Connected static DRAM delta: {delta['dram_bytes']} bytes")
    print(f"Connected static RAM delta including IRAM/RTC: {delta['static_ram_bytes']} bytes")
    print(f"Minimal app image delta: {minimal_delta['app_image_bytes']} bytes")
    print(f"Direct libhonch.a: {archive['total_bytes']} bytes ({claims['sdk_flash']})")
    print(f"Direct libhonch.a static RAM: {archive['static_ram_bytes']} bytes ({claims['sdk_static_ram']})")
    if report["runtime"]:
        print(f"Runtime: {json.dumps(report['runtime'], sort_keys=True)}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", default="esp32", help="ESP-IDF target, e.g. esp32, esp32s3, esp32c3")
    parser.add_argument("--skip-build", action="store_true", help="Use existing build-footprint-* directories")
    parser.add_argument("--monitor-log", type=Path, help="Optional captured monitor log with HONCH_FOOTPRINT markers")
    parser.add_argument(
        "--output",
        type=Path,
        default=FOOTPRINT_DIR / "build-footprint-report.json",
        help="Report JSON path",
    )
    args = parser.parse_args()

    bare_build = f"build-footprint-{args.target}-bare"
    connected_build = f"build-footprint-{args.target}-connected"
    honch_build = f"build-footprint-{args.target}-honch"
    bare_map = FOOTPRINT_DIR / bare_build / "honch_footprint.map"
    connected_map = FOOTPRINT_DIR / connected_build / "honch_footprint.map"
    honch_map = FOOTPRINT_DIR / honch_build / "honch_footprint.map"

    if not args.skip_build:
        bare_map = build_variant(args.target, bare_build, with_honch=False)
        connected_map = build_variant(
            args.target,
            connected_build,
            with_honch=False,
            connected_baseline=True,
        )
        honch_map = build_variant(args.target, honch_build, with_honch=True)

    bare_total_json = FOOTPRINT_DIR / bare_build / "size-total.json"
    connected_total_json = FOOTPRINT_DIR / connected_build / "size-total.json"
    honch_total_json = FOOTPRINT_DIR / honch_build / "size-total.json"
    honch_archives_json = FOOTPRINT_DIR / honch_build / "size-archives.json"
    write_size_json(bare_map, bare_total_json)
    write_size_json(connected_map, connected_total_json)
    write_size_json(honch_map, honch_total_json)
    write_size_json(honch_map, honch_archives_json, archives=True)

    runtime = None
    if args.monitor_log:
        runtime = parse_monitor_log(args.monitor_log.read_text(encoding="utf-8"))

    report = build_report(
        target=args.target,
        bare=parse_total_sizes(bare_total_json),
        connected=parse_total_sizes(connected_total_json),
        honch=parse_total_sizes(honch_total_json),
        honch_archive=parse_archive_sizes(honch_archives_json),
        runtime=runtime,
    )
    args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print_summary(report)
    print(f"\nWrote {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
