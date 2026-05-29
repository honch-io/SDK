#!/usr/bin/env python3

import argparse
import re
import subprocess
import sys


REQUIRED_WINDOW_FIELDS = {
    "runtime",
    "mode",
    "rate_target_eps",
    "window",
    "actual_eps",
    "events_attempted",
    "events_accepted",
    "events_failed",
    "track_avg_us",
    "track_p50_us",
    "track_p95_us",
    "track_p99_us",
    "track_max_us",
    "flush_count",
    "flush_failures",
    "flush_avg_us",
    "flush_p95_us",
    "cpu_percent",
    "queued_estimate",
    "rss_kb",
    "sdk_current_bytes",
    "sdk_peak_bytes",
    "transport_calls",
    "transport_bytes",
}


def parse_fields(line: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for key, value in re.findall(r"([A-Za-z0-9_]+)=(\"[^\"]*\"|\S+)", line):
        fields[key] = value.strip('"')
    return fields


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    args = parser.parse_args()

    completed = subprocess.run(
        [
            args.binary,
            "--rate-sweep",
            "--rates",
            "100,500",
            "--duration",
            "1",
            "--warmup",
            "0",
            "--window",
            "1",
            "--payload-bytes",
            "16",
            "--flush-every",
            "25",
        ],
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if completed.returncode != 0:
        print(completed.stdout)
        print(completed.stderr, file=sys.stderr)
        return completed.returncode

    lines = completed.stdout.splitlines()
    suite_start = [line for line in lines if "BENCH_SUITE_START" in line]
    windows = [line for line in lines if "BENCH_WINDOW" in line]
    suite_end = [line for line in lines if "BENCH_SUITE_END" in line]

    assert suite_start, completed.stdout
    assert suite_end, completed.stdout
    assert len(windows) >= 2, completed.stdout

    seen_rates = {parse_fields(line).get("rate_target_eps") for line in windows}
    assert seen_rates == {"100", "500"}, completed.stdout

    for line in windows:
        fields = parse_fields(line)
        missing = REQUIRED_WINDOW_FIELDS - fields.keys()
        assert not missing, f"missing {sorted(missing)} in {line}"
        assert fields["runtime"] == "c-core-posix", line
        assert fields["mode"] == "sdk-only", line
        assert float(fields["actual_eps"]) > 0, line

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
