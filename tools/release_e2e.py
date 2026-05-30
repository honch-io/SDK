#!/usr/bin/env python3
"""Run the Honch SDK production E2E release campaign.

The campaign document remains the source of truth. This runner makes the
repeatable parts executable, records evidence, and calls out unavailable
toolchains or hardware as BLOCKED instead of silently skipping them.
"""

from __future__ import annotations

import argparse
import dataclasses
import datetime as dt
import enum
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
from typing import Iterable


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_LOG_ROOT = Path("/private/tmp")
ESP_IDF_TOOLS = "/Volumes/X9 Pro/Espressif/tools"
ESP_IDF_EXPORT = "/Volumes/X9 Pro/Espressif/esp-idf-v6.0.1/export.sh"
ESP_IDF_SOURCE = f"export IDF_TOOLS_PATH='{ESP_IDF_TOOLS}' && source '{ESP_IDF_EXPORT}'"
ARDUINO_HOME = "/Volumes/X9 Pro/Arduino"
ARDUINO_BUILD_ROOT = "/Volumes/X9 Pro/honch-arduino-verify"
DEFAULT_E2E_ENV = {
    "HONCH_E2E": "1",
    "HONCH_E2E_ENDPOINT": "http://127.0.0.1:8001",
    "HONCH_E2E_CAPTURE_HEALTH_URL": "http://127.0.0.1:8001/health",
    "HONCH_E2E_WORKER_HEALTH_URL": "http://127.0.0.1:8080/",
    "HONCH_E2E_CLICKHOUSE_URL": "http://127.0.0.1:8123",
    "HONCH_E2E_TOKEN": "honch_e2e_test_key",
    "HONCH_E2E_PROJECT_ID": "00000000-0000-0000-0000-000000000002",
    "HONCH_E2E_CLICKHOUSE_DATABASE": "platform",
}


class Status(str, enum.Enum):
    PASS = "PASS"
    FAIL = "FAIL"
    SKIP = "SKIP"
    BLOCKED = "BLOCKED"


@dataclasses.dataclass(frozen=True)
class Step:
    area: str
    name: str
    command: str
    profiles: tuple[str, ...]
    log_name: str
    env: dict[str, str] = dataclasses.field(default_factory=dict)
    require_commands: tuple[str, ...] = ()
    require_paths: tuple[str, ...] = ()
    require_env: tuple[str, ...] = ()
    optional: bool = False
    timeout_seconds: int | None = None
    notes: str = ""


@dataclasses.dataclass
class Result:
    area: str
    name: str
    status: Status
    command: str
    log_file: str
    exit_code: int | None
    duration_seconds: float
    notes: str


def step(
    area: str,
    name: str,
    command: str,
    profiles: Iterable[str],
    log_name: str,
    **kwargs: object,
) -> Step:
    return Step(
        area=area,
        name=name,
        command=command,
        profiles=tuple(profiles),
        log_name=log_name,
        **kwargs,
    )


HOST = ("smoke", "host", "release", "full")
SERVICES = ("services", "release", "full")
TOOLCHAINS = ("toolchains", "release", "full")
HARDWARE = ("hardware", "full")


STEPS: tuple[Step, ...] = (
    step(
        "Preflight",
        "capture branch and worktree state",
        "git branch --show-current && git status --short && git log -5 --oneline",
        ("smoke", "host", "services", "toolchains", "hardware", "release", "full"),
        "00-preflight.txt",
    ),
    step(
        "Preflight",
        "record remediation and stale-review references",
        "git log -10 --oneline; "
        "rg -n \"Open Finding|Arduino|arduino|durability|package version|response handling|scheduled flush\" "
        "local-docs/review-wip.md docs/production-e2e.md || true",
        HOST,
        "01-review-context.txt",
        require_paths=("local-docs/review-wip.md",),
        optional=True,
    ),
    step(
        "C core / POSIX",
        "configure POSIX debug build",
        "cmake -S ports/posix -B $HONCH_E2E_BUILD_ROOT/posix-debug "
        "-DHONCH_BUILD_TESTS=ON -DHONCH_BUILD_EXAMPLES=ON -DCMAKE_BUILD_TYPE=Debug",
        HOST,
        "10-posix-debug-configure.txt",
        require_commands=("cmake",),
    ),
    step(
        "C core / POSIX",
        "build POSIX debug targets",
        "cmake --build $HONCH_E2E_BUILD_ROOT/posix-debug",
        HOST,
        "11-posix-debug-build.txt",
        require_commands=("cmake",),
    ),
    step(
        "C core / POSIX",
        "run POSIX debug CTest suite",
        "ctest --test-dir $HONCH_E2E_BUILD_ROOT/posix-debug --output-on-failure",
        HOST,
        "12-posix-debug-ctest.txt",
        require_commands=("ctest",),
    ),
    step(
        "C core / POSIX",
        "configure POSIX sanitizer build",
        "cmake -S ports/posix -B $HONCH_E2E_BUILD_ROOT/posix-asan "
        "-DHONCH_BUILD_TESTS=ON -DHONCH_BUILD_EXAMPLES=ON -DCMAKE_BUILD_TYPE=Debug "
        "-DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' "
        "-DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'",
        ("host", "release", "full"),
        "13-posix-asan-configure.txt",
        require_commands=("cmake",),
    ),
    step(
        "C core / POSIX",
        "build and run POSIX sanitizer suite",
        "cmake --build $HONCH_E2E_BUILD_ROOT/posix-asan && "
        "ctest --test-dir $HONCH_E2E_BUILD_ROOT/posix-asan --output-on-failure",
        ("host", "release", "full"),
        "14-posix-asan-suite.txt",
        require_commands=("cmake", "ctest"),
    ),
    step(
        "C core / POSIX",
        "run SDK contract guards",
        "python3 ports/posix/test/test_sdk_contract.py",
        HOST,
        "15-posix-sdk-contract.txt",
        require_commands=("python3",),
    ),
    step(
        "C core / POSIX",
        "build release benchmark target",
        "cmake -S ports/posix -B $HONCH_E2E_BUILD_ROOT/posix-release "
        "-DCMAKE_BUILD_TYPE=Release -DHONCH_BUILD_TESTS=OFF "
        "-DHONCH_BUILD_EXAMPLES=OFF -DHONCH_BUILD_BENCHMARKS=ON && "
        "cmake --build $HONCH_E2E_BUILD_ROOT/posix-release --target honch_posix_bench",
        ("host", "release", "full"),
        "16-posix-release-bench.txt",
        require_commands=("cmake",),
    ),
    step(
        "C core / POSIX",
        "verify install package and version metadata",
        "python3 -m unittest ports.posix.test.test_install_package && "
        "cmake -S ports/posix -B $HONCH_E2E_BUILD_ROOT/posix-install "
        "-DHONCH_BUILD_TESTS=OFF -DHONCH_BUILD_EXAMPLES=OFF -DHONCH_BUILD_BENCHMARKS=OFF && "
        "cmake --build $HONCH_E2E_BUILD_ROOT/posix-install && "
        "cmake --install $HONCH_E2E_BUILD_ROOT/posix-install --prefix $HONCH_E2E_BUILD_ROOT/posix-prefix && "
        "rg -n 'PACKAGE_VERSION \"0\\.2\\.0\"' "
        "$HONCH_E2E_BUILD_ROOT/posix-prefix/lib/cmake/honch_posix/honch_posixConfigVersion.cmake",
        HOST,
        "17-posix-install-version.txt",
        require_commands=("cmake", "python3", "rg"),
    ),
    step(
        "Local capture",
        "verify local service health",
        "curl -i --max-time 5 http://127.0.0.1:8001/health && "
        "curl -i --max-time 5 http://127.0.0.1:8080/ && "
        "curl -i --max-time 5 http://127.0.0.1:8123/",
        SERVICES,
        "20-local-service-health.txt",
        require_commands=("curl",),
        notes="Start the sandbox first with ./honch --plain sandbox if this blocks.",
    ),
    step(
        "Local capture",
        "record sibling capture contract when available",
        "if [ -d ../capture ]; then "
        "git -C ../capture branch --show-current; "
        "git -C ../capture log -1 --oneline; "
        "rg -n '\"/capture\"|\"/e\"|\"/chunks\"|application/vnd.honch.chunk|X-Honch-Project-Key|X-Honch-Stream-Id|POST /batch|application/cbor' "
        "../capture/README.md ../capture/src ../capture/tests || true; "
        "else echo '../capture not present'; fi",
        SERVICES,
        "21-capture-contract.txt",
        optional=True,
    ),
    step(
        "Local capture",
        "build POSIX real-capture E2E target",
        "cmake -S ports/posix -B $HONCH_E2E_BUILD_ROOT/posix-e2e "
        "-DHONCH_BUILD_TESTS=ON -DHONCH_BUILD_E2E=ON -DCMAKE_BUILD_TYPE=Debug && "
        "cmake --build $HONCH_E2E_BUILD_ROOT/posix-e2e --target honch_posix_e2e",
        SERVICES,
        "22-posix-e2e-build.txt",
        require_commands=("cmake",),
    ),
    step(
        "Local capture",
        "run POSIX real-capture E2E",
        "ctest --test-dir $HONCH_E2E_BUILD_ROOT/posix-e2e --output-on-failure -R honch_posix_e2e",
        SERVICES,
        "23-posix-e2e-run.txt",
        env=DEFAULT_E2E_ENV,
        require_commands=("ctest",),
    ),
    step(
        "Local capture",
        "run MicroPython real-capture host E2E",
        "PYTHONPATH=ports/micropython python3 -m unittest -v ports.micropython.tests.test_e2e_capture",
        SERVICES,
        "24-micropython-e2e-run.txt",
        env=DEFAULT_E2E_ENV,
        require_commands=("python3",),
    ),
    step(
        "Local capture",
        "record canonical SDK transport evidence",
        "rg -n '\"/capture\"|application/vnd.honch.chunk|X-Honch-Project-Key|X-Honch-Stream-Id|post_chunk|application/cbor|POST /batch|post_batch' "
        "core ports/posix ports/esp-idf ports/arduino ports/micropython",
        SERVICES,
        "25-transport-evidence.txt",
        require_commands=("rg",),
    ),
    step(
        "ESP-IDF",
        "run ESP SDK contract and footprint tests",
        "python3 ports/esp-idf/tests/test_sdk_contract.py && "
        "python3 ports/esp-idf/tests/test_footprint_measurement.py && "
        "python3 ports/esp-idf/tests/test_durability_config.py",
        HOST,
        "30-esp-static-tests.txt",
        require_commands=("python3",),
    ),
    step(
        "ESP-IDF",
        "build ESP example for esp32",
        f"{ESP_IDF_SOURCE} && "
        "idf.py -C ports/esp-idf/example -B $HONCH_E2E_BUILD_ROOT/esp-example set-target esp32 && "
        "idf.py -C ports/esp-idf/example -B $HONCH_E2E_BUILD_ROOT/esp-example build",
        TOOLCHAINS,
        "31-esp-example-build.txt",
        require_paths=(ESP_IDF_TOOLS, ESP_IDF_EXPORT),
    ),
    step(
        "ESP-IDF",
        "build ESP benchtest for esp32",
        f"{ESP_IDF_SOURCE} && "
        "idf.py -C ports/esp-idf/benchtest -B $HONCH_E2E_BUILD_ROOT/esp-benchtest set-target esp32 && "
        "idf.py -C ports/esp-idf/benchtest -B $HONCH_E2E_BUILD_ROOT/esp-benchtest build",
        TOOLCHAINS,
        "32-esp-benchtest-build.txt",
        require_paths=(ESP_IDF_TOOLS, ESP_IDF_EXPORT),
    ),
    step(
        "ESP-IDF",
        "build ESP footprint variants",
        f"{ESP_IDF_SOURCE} && "
        "./tools/measure_esp_idf_footprint.py --target esp32",
        TOOLCHAINS,
        "33-esp-footprint-build.txt",
        require_paths=(ESP_IDF_TOOLS, ESP_IDF_EXPORT),
    ),
    step(
        "Arduino ESP32",
        "run Arduino source-shape tests",
        "python3 -m unittest ports.arduino.test.test_ci_workflow ports.arduino.test.test_tls_config",
        HOST,
        "60-arduino-source-shape.txt",
        require_commands=("python3",),
    ),
    step(
        "Arduino ESP32",
        "verify vendored core sync",
        "ports/arduino/scripts/check-core-sync.sh",
        HOST,
        "61-arduino-core-sync.txt",
    ),
    step(
        "Arduino ESP32",
        "run Arduino host wrapper behavior test",
        "cmake -S ports/arduino/test/host -B $HONCH_E2E_BUILD_ROOT/arduino-host && "
        "cmake --build $HONCH_E2E_BUILD_ROOT/arduino-host && "
        "$HONCH_E2E_BUILD_ROOT/arduino-host/honch_arduino_wrapper_test",
        HOST,
        "62-arduino-host-wrapper.txt",
        require_commands=("cmake",),
    ),
    step(
        "Arduino ESP32",
        "compile Arduino examples with strict Arduino CLI mode",
        f"HONCH_ARDUINO_HOME='{ARDUINO_HOME}' "
        f"HONCH_ARDUINO_BUILD_ROOT='{ARDUINO_BUILD_ROOT}' "
        "ports/arduino/scripts/verify-arduino.sh --require-arduino-cli",
        TOOLCHAINS,
        "63-arduino-cli-compile.txt",
        require_commands=("arduino-cli",),
        require_paths=(ARDUINO_HOME, ARDUINO_BUILD_ROOT),
    ),
    step(
        "Arduino ESP32",
        "compile PlatformIO example",
        "pio run -d ports/arduino/examples/platformio",
        TOOLCHAINS,
        "64-arduino-platformio.txt",
        require_commands=("pio",),
    ),
    step(
        "MicroPython",
        "compile MicroPython package, tests, and examples",
        "cd ports/micropython && python3 -m compileall honch tests examples",
        HOST,
        "70-micropython-compileall.txt",
        require_commands=("python3",),
    ),
    step(
        "MicroPython",
        "run MicroPython host unit and conformance tests",
        "PYTHONPATH=ports/micropython python3 -m unittest discover -v -s ports/micropython/tests -t .",
        HOST,
        "71-micropython-host-tests.txt",
        require_commands=("python3",),
    ),
    step(
        "MicroPython",
        "check MicroPython package metadata",
        "cd ports/micropython && python3 - <<'PY'\n"
        "import json\n"
        "from pathlib import Path\n"
        "pkg = json.loads(Path('package.json').read_text())\n"
        "missing = [dst for _, dst in pkg['urls'] if not Path(dst).exists()]\n"
        "if missing:\n"
        "    raise SystemExit('missing package files: ' + ', '.join(missing))\n"
        "print('package file list ok')\n"
        "PY",
        HOST,
        "72-micropython-package-metadata.txt",
        require_commands=("python3",),
    ),
    step(
        "React Native relay",
        "typecheck and test relay package",
        "cd ports/react-native-relay && bun run typecheck && bun run test",
        ("host", "release", "full"),
        "90-react-native-relay-tests.txt",
        require_commands=("bun",),
        optional=True,
        notes="Relay remains prototype/contract-mismatch unless its acceptance criteria are satisfied.",
    ),
    step(
        "React Native relay",
        "record relay production gap evidence",
        "rg -n 'decodeRelayFrame|POST /batch|application/cbor|AsyncStorage|SQLite|MMKV|BLE|background|retry|backoff' "
        "ports/react-native-relay spec/relay-envelope.md spec/relay-chunks.md || true",
        ("host", "release", "full"),
        "91-react-native-relay-gaps.txt",
        require_commands=("rg",),
        optional=True,
    ),
    step(
        "Cross-SDK conformance",
        "list conformance fixture coverage",
        "find spec/conformance -maxdepth 4 -type f -name '*.json' -print | sort",
        HOST,
        "100-conformance-fixtures.txt",
    ),
    step(
        "Cross-SDK conformance",
        "compare SDK test coverage to fixtures",
        "rg -n 'basic-track|auto_stamp|boot_event|session-track|identity-reset|response-policy|basic_batch|wire-v2|application/vnd.honch.chunk' "
        "core ports spec",
        HOST,
        "101-conformance-coverage.txt",
        require_commands=("rg",),
    ),
    step(
        "Cross-SDK conformance",
        "verify compact chunk compatibility",
        "python3 spec/conformance/test_wire_v2_fixtures.py && "
        "rg -n 'honch_core_build_wire_v2_message|honch_core_post_wire_v2_message|post_chunk|application/vnd.honch.chunk' "
        "core ports spec",
        HOST,
        "102-compact-chunk-compatibility.txt",
        require_commands=("python3", "rg"),
    ),
    step(
        "Cross-SDK conformance",
        "verify relay frame compatibility separately",
        "rg -n 'HONCH_PACKETIZER_HEADER_SIZE|decodeRelayFrame|relay frame|payloadLength|sequence|offset' "
        "core ports/react-native-relay spec/relay-chunks.md",
        HOST,
        "103-relay-frame-compatibility.txt",
        require_commands=("rg",),
    ),
    step(
        "ESP32 hardware",
        "verify ESP32 hardware inputs",
        "printf 'HONCH_ESP_PORT=%s\\nHONCH_ESP_TARGET=%s\\nHONCH_CAPTURE_LAN_URL=%s\\n' "
        "\"$HONCH_ESP_PORT\" \"${HONCH_ESP_TARGET:-esp32}\" \"$HONCH_CAPTURE_LAN_URL\" && "
        "curl -i --max-time 5 \"$HONCH_CAPTURE_LAN_URL/health\"",
        HARDWARE,
        "40-esp32-hardware-inputs.txt",
        require_commands=("curl",),
        require_env=("HONCH_ESP_PORT", "HONCH_CAPTURE_LAN_URL"),
        notes="Set Wi-Fi/API credentials in ignored sdkconfig or the ESP-IDF project config before flashing.",
    ),
    step(
        "MicroPython hardware",
        "verify MicroPython board is reachable",
        "mpremote connect \"${HONCH_MICROPYTHON_PORT:-auto}\" exec 'import sys; print(sys.implementation)'",
        HARDWARE,
        "80-micropython-board-reachable.txt",
        require_commands=("mpremote",),
        optional=True,
    ),
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--profile",
        choices=("smoke", "host", "services", "toolchains", "hardware", "release", "full"),
        default="release",
        help=(
            "smoke is the fastest host subset; host excludes services/toolchains/hardware; "
            "release runs host+services+toolchains; full also includes hardware preflights"
        ),
    )
    parser.add_argument("--list", action="store_true", help="list selected steps and exit")
    parser.add_argument("--dry-run", action="store_true", help="write no logs and run no commands")
    parser.add_argument(
        "--allow-blocked",
        action="store_true",
        help="exit 0 when the only non-passing required steps are blocked by missing prerequisites",
    )
    parser.add_argument(
        "--log-root",
        type=Path,
        default=DEFAULT_LOG_ROOT,
        help="directory where timestamped evidence directory is created",
    )
    parser.add_argument(
        "--continue-on-fail",
        action="store_true",
        help="keep running after a failed required step",
    )
    return parser.parse_args()


def selected_steps(profile: str) -> list[Step]:
    return [entry for entry in STEPS if profile in entry.profiles]


def missing_requirements(entry: Step, env: dict[str, str]) -> list[str]:
    missing: list[str] = []
    for command in entry.require_commands:
        if shutil.which(command) is None:
            missing.append(f"command not found: {command}")
    for raw_path in entry.require_paths:
        if not Path(os.path.expandvars(raw_path)).exists():
            missing.append(f"path not found: {raw_path}")
    for name in entry.require_env:
        if not env.get(name):
            missing.append(f"environment variable not set: {name}")
    return missing


def make_log_dir(log_root: Path) -> Path:
    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    log_dir = log_root / f"honch-production-e2e-{stamp}"
    log_dir.mkdir(parents=True, exist_ok=False)
    return log_dir


def run_step(entry: Step, log_dir: Path, dry_run: bool) -> Result:
    env = os.environ.copy()
    env.update(entry.env)
    env["HONCH_E2E_BUILD_ROOT"] = str(log_dir / "build")
    missing = missing_requirements(entry, env)
    log_file = log_dir / entry.log_name

    if dry_run:
        print(f"DRY-RUN {entry.area}: {entry.name}")
        print(f"  {entry.command}")
        return Result(entry.area, entry.name, Status.SKIP, entry.command, "", None, 0.0, "dry run")

    if missing:
        status = Status.SKIP if entry.optional else Status.BLOCKED
        notes = "; ".join(missing)
        log_file.write_text(notes + "\n", encoding="utf-8")
        return Result(entry.area, entry.name, status, entry.command, str(log_file), None, 0.0, notes)

    started = time.monotonic()
    header = (
        f"$ {entry.command}\n"
        f"area: {entry.area}\n"
        f"step: {entry.name}\n"
        f"cwd: {ROOT}\n"
        f"started_at: {dt.datetime.now().isoformat(timespec='seconds')}\n\n"
    )
    with log_file.open("w", encoding="utf-8") as log:
        log.write(header)
        log.flush()
        try:
            completed = subprocess.run(
                ["bash", "-lc", entry.command],
                cwd=ROOT,
                env=env,
                stdout=log,
                stderr=subprocess.STDOUT,
                text=True,
                timeout=entry.timeout_seconds,
                check=False,
            )
            exit_code = completed.returncode
        except subprocess.TimeoutExpired:
            log.write(f"\nTIMEOUT after {entry.timeout_seconds} seconds\n")
            exit_code = 124

    duration = time.monotonic() - started
    if exit_code == 0:
        status = Status.PASS
        notes = entry.notes
    elif entry.optional:
        status = Status.SKIP
        notes = f"optional step exited {exit_code}. {entry.notes}".strip()
    else:
        status = Status.FAIL
        notes = entry.notes

    return Result(entry.area, entry.name, status, entry.command, str(log_file), exit_code, duration, notes)


def write_summary(log_dir: Path, profile: str, results: list[Result]) -> None:
    summary = {
        "profile": profile,
        "root": str(ROOT),
        "created_at": dt.datetime.now().isoformat(timespec="seconds"),
        "results": [dataclasses.asdict(result) for result in results],
    }
    (log_dir / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")


def write_report(log_dir: Path, profile: str, results: list[Result]) -> None:
    by_area: dict[str, list[Result]] = {}
    for result in results:
        by_area.setdefault(result.area, []).append(result)

    lines = [
        "# Honch Production E2E Readiness Report",
        "",
        f"- Profile: `{profile}`",
        f"- Repository: `{ROOT}`",
        f"- Evidence directory: `{log_dir}`",
        f"- Generated: `{dt.datetime.now().isoformat(timespec='seconds')}`",
        "",
        "## Summary",
        "",
    ]
    for status in Status:
        count = sum(1 for result in results if result.status is status)
        lines.append(f"- {status.value}: {count}")

    lines.extend(["", "## Results", ""])
    for area, area_results in by_area.items():
        statuses = {result.status for result in area_results}
        if Status.FAIL in statuses:
            area_status = Status.FAIL
        elif Status.BLOCKED in statuses:
            area_status = Status.BLOCKED
        elif all(result.status is Status.SKIP for result in area_results):
            area_status = Status.SKIP
        else:
            area_status = Status.PASS
        lines.extend([f"### {area}: {area_status.value}", ""])
        for result in area_results:
            exit_text = "n/a" if result.exit_code is None else str(result.exit_code)
            lines.append(
                f"- {result.status.value}: {result.name} "
                f"(exit `{exit_text}`, {result.duration_seconds:.1f}s, log `{result.log_file}`)"
            )
            if result.notes:
                lines.append(f"  - Notes: {result.notes}")
        lines.append("")

    lines.extend(
        [
            "## Release Recommendation",
            "",
            "Use this report as evidence, not as automatic approval. A release is blocked when any",
            "required step is FAIL or BLOCKED for the intended release surface. Hardware-dependent",
            "readiness still requires running the `hardware` or `full` profile with device inputs.",
            "",
        ]
    )
    (log_dir / "PRODUCTION_READINESS_REPORT.md").write_text("\n".join(lines), encoding="utf-8")


def print_list(profile: str) -> None:
    for entry in selected_steps(profile):
        optional = " optional" if entry.optional else ""
        print(f"[{entry.area}]{optional} {entry.name}")
        print(f"  {entry.command}")


def exit_code_for(results: list[Result], allow_blocked: bool) -> int:
    required = [result for result in results if result.status is not Status.SKIP]
    if any(result.status is Status.FAIL for result in required):
        return 1
    if any(result.status is Status.BLOCKED for result in required):
        return 0 if allow_blocked else 2
    return 0


def main() -> int:
    args = parse_args()
    if args.list:
        print_list(args.profile)
        return 0

    steps_to_run = selected_steps(args.profile)
    if args.dry_run:
        for entry in steps_to_run:
            run_step(entry, Path("."), True)
        return 0

    log_dir = make_log_dir(args.log_root)
    print(f"Evidence directory: {log_dir}")

    results: list[Result] = []
    for entry in steps_to_run:
        print(f"{entry.area}: {entry.name} ...", flush=True)
        result = run_step(entry, log_dir, False)
        results.append(result)
        print(f"  {result.status.value} ({result.duration_seconds:.1f}s)")
        if result.status is Status.FAIL and not args.continue_on_fail:
            break

    write_summary(log_dir, args.profile, results)
    write_report(log_dir, args.profile, results)
    print(f"Report: {log_dir / 'PRODUCTION_READINESS_REPORT.md'}")
    return exit_code_for(results, args.allow_blocked)


if __name__ == "__main__":
    sys.exit(main())
