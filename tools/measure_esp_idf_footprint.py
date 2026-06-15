#!/usr/bin/env python3
"""Parse ESP-IDF footprint artifacts into a compact report."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FOOTPRINT_DIR = ROOT / "ports" / "esp-idf" / "footprint"


def _bucket(layout: list[dict], name: str) -> int:
    for entry in layout:
        if entry.get("name") == name:
            return int(entry.get("used", 0))
    return 0


def _part_sum(layout: list[dict], bucket_name: str, suffixes: tuple[str, ...]) -> int:
    for entry in layout:
        if entry.get("name") != bucket_name:
            continue
        parts = entry.get("parts", {})
        total = 0
        for key, value in parts.items():
            size = value.get("size", 0) if isinstance(value, dict) else value
            if any(key.endswith(suffix) for suffix in suffixes):
                total += int(size)
        return total
    return 0


def _parse_total_sizes_from_data(data: dict) -> dict[str, int]:
    layout = data.get("layout", [])
    dram_bytes = _bucket(layout, "DRAM")
    return {
        "app_image_bytes": int(data.get("total_size", 0)),
        "flash_code_bytes": _bucket(layout, "Flash Code"),
        "flash_data_bytes": _bucket(layout, "Flash Data"),
        "iram_bytes": _bucket(layout, "IRAM"),
        "dram_bytes": dram_bytes,
        "static_ram_bytes": dram_bytes + _part_sum(layout, "IRAM", (".text",)) + _bucket(layout, "RTC SLOW"),
    }


def parse_total_sizes(path: Path | str) -> dict[str, int]:
    return _parse_total_sizes_from_data(json.loads(Path(path).read_text(encoding="utf-8")))


parse_total_sizes.from_data = _parse_total_sizes_from_data  # type: ignore[attr-defined]


def _archive_memory_size(memory_types: dict, name: str) -> int:
    value = memory_types.get(name, {})
    if isinstance(value, dict):
        return int(value.get("size", sum(int(v) for k, v in value.items() if k != "size")))
    return int(value or 0)


def _parse_archive_sizes_from_data(data: dict, archive_name: str) -> dict[str, int]:
    archive = data[archive_name]
    memory_types = archive.get("memory_types", {})
    return {
        "total_bytes": int(archive.get("size", 0)),
        "flash_code_bytes": _archive_memory_size(memory_types, "Flash Code"),
        "dram_bytes": _archive_memory_size(memory_types, "DRAM"),
        "iram_bytes": _archive_memory_size(memory_types, "IRAM"),
    }


def parse_archive_sizes(path: Path | str, archive_name: str) -> dict[str, int]:
    return _parse_archive_sizes_from_data(json.loads(Path(path).read_text(encoding="utf-8")), archive_name)


parse_archive_sizes.from_data = _parse_archive_sizes_from_data  # type: ignore[attr-defined]


def _parse_key_values(line: str) -> dict[str, int | float | str]:
    values: dict[str, int | float | str] = {}
    for key, value in re.findall(r"([a-zA-Z0-9_]+)=([^\s]+)", line):
        try:
            values[key] = float(value) if "." in value else int(value)
        except ValueError:
            values[key] = value
    return values


def parse_monitor_log(text: str) -> dict:
    runtime: dict = {}
    for line in text.splitlines():
        if "HONCH_FOOTPRINT_RUNTIME" in line:
            values = _parse_key_values(line)
            phase = values.pop("phase", None)
            if phase:
                runtime[str(phase)] = values
        elif "HONCH_FOOTPRINT_CPU" in line:
            runtime["track_cpu"] = _parse_key_values(line)
    before = runtime.get("before_honch", {})
    after = runtime.get("after_honch_init", {})
    if "heap_free" in before and "heap_free" in after:
        runtime["heap_delta_after_init_bytes"] = int(before["heap_free"]) - int(after["heap_free"])
    return runtime


def _claim(bytes_value: int, *, quantum_kb: int) -> str:
    kib = ((max(0, bytes_value) + 1023) // 1024)
    rounded = max(1, ((kib + quantum_kb - 1) // quantum_kb) * quantum_kb)
    return f"<{rounded} KB"


def build_report(
    *,
    target: str,
    bare: dict[str, int] | None = None,
    connected: dict[str, int] | None = None,
    honch: dict[str, int],
    honch_archive: dict[str, int],
    runtime: dict | None,
) -> dict:
    baseline = connected or bare or {}
    delta = {
        key: int(honch.get(key, 0)) - int(baseline.get(key, 0))
        for key in ("app_image_bytes", "flash_code_bytes", "flash_data_bytes", "iram_bytes", "dram_bytes", "static_ram_bytes")
    }
    connected_delta_valid = not connected or all(value >= 0 for value in delta.values())
    claim_source = delta if connected_delta_valid else honch_archive
    return {
        "target": target,
        "bare": bare,
        "connected": connected,
        "honch": honch,
        "delta": delta,
        "direct_honch_archive": honch_archive,
        "runtime": runtime,
        "connected_delta_valid_for_claims": connected_delta_valid,
        "landing_claims": {
            "sdk_flash": _claim(
                int(claim_source.get("app_image_bytes", claim_source.get("total_bytes", 0))),
                quantum_kb=10,
            ),
            "sdk_static_ram": _claim(
                int(claim_source.get("static_ram_bytes", claim_source.get("dram_bytes", 0))),
                quantum_kb=1,
            ),
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--target", default="esp32")
    parser.parse_args()
    print(json.dumps({"target": "esp32", "footprint_dir": str(FOOTPRINT_DIR)}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
