#!/usr/bin/env python3
#
# Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Compare checker builds or checks on frozen compilation commands.

Runs on Linux or macOS. Failures remain in the report; paired serial timing
requires successful, eligible units in every measured repetition for both tools.
Mixed-check diagnostic grouping requires PyYAML. See CIRLifetimeBenchmark.md.
"""

import argparse
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import platform
import re
import shutil
import statistics
import subprocess
import sys
import time


# Historical campaign drivers import this file by path from another directory.
_support_path = Path(__file__).with_name("cir_lifetime_benchmark_support.py")
_support_spec = importlib.util.spec_from_file_location(
    "cir_lifetime_benchmark_support", _support_path
)
support = importlib.util.module_from_spec(_support_spec)
_support_spec.loader.exec_module(support)

CHECK = "cir-lifetime-check"
CHECKS = (CHECK, "bugprone-use-after-move")
PROFILE_MODES = ("off", "phases", "phases+cir", "legacy")
# Compile commands can force color despite --use-color=false. Strip terminal
# controls only when interpreting diagnostics; keep the raw log files intact.
ANSI_CSI = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
DIAGNOSTIC = re.compile(
    r"^(?:(.*):(\d+):(\d+): )?(fatal error|error|warning|remark|note): (.*)$"
)


def timestamp():
    return datetime.now(timezone.utc).isoformat()


def json_text(value):
    return json.dumps(value, indent=2, sort_keys=True) + "\n"


def file_hash(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def positive_int(value):
    result = int(value)
    if result < 1:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return result


def nonnegative_int(value):
    result = int(value)
    if result < 0:
        raise argparse.ArgumentTypeError("must be nonnegative")
    return result


def positive_seconds(value):
    result = float(value)
    if not math.isfinite(result) or result <= 0:
        raise argparse.ArgumentTypeError("must be a finite positive number")
    return result


def revision(value):
    if not re.fullmatch(r"[0-9a-fA-F]{40}", value):
        raise argparse.ArgumentTypeError("supply the full 40-character Git revision")
    return value.lower()


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    for variant in ("baseline", "candidate"):
        parser.add_argument("--" + variant, required=True, help="clang-tidy executable")
        parser.add_argument("--" + variant + "-revision", required=True, type=revision)
        parser.add_argument("--" + variant + "-build-type", required=True)
        parser.add_argument(
            "--" + variant + "-assertions", choices=("on", "off"), required=True
        )
        parser.add_argument("--" + variant + "-check", choices=CHECKS, default=CHECK)
        parser.add_argument("--" + variant + "-profile-mode", choices=PROFILE_MODES)
        parser.add_argument(
            "--" + variant + "-check-option",
            action="append",
            default=[],
            metavar="NAME=VALUE",
        )
    parser.add_argument(
        "--source-revision",
        required=True,
        type=revision,
        help="revision of the Clang source corpus",
    )
    parser.add_argument(
        "--clang-source-root",
        "--source-root",
        dest="clang_source_root",
        type=Path,
        required=True,
    )
    parser.add_argument(
        "--compilation-database",
        type=Path,
        required=True,
        help="compile_commands.json or its directory",
    )
    parser.add_argument(
        "--output-dir", type=Path, required=True, help="new or empty artifact directory"
    )
    parser.add_argument(
        "--file-filter",
        default=".*",
        help="regular expression matching paths relative to Clang's source root",
    )
    parser.add_argument(
        "--limit", type=positive_int, help="first N selected paths, in sorted order"
    )
    parser.add_argument("--repetitions", type=positive_int, default=3)
    parser.add_argument("--warmups", type=nonnegative_int, default=0)
    parser.add_argument("--jobs", type=positive_int, default=1)
    parser.add_argument(
        "--profile-checks", action="store_true",
        help="compatibility alias: select legacy profiling for both variants",
    )
    parser.add_argument(
        "--execution-mode",
        choices=("paired", "batches"),
        help="default: paired for one job, separate check batches otherwise",
    )
    parser.add_argument(
        "--selection-file",
        type=Path,
        help="frozen units array, including explicit generated-source selections",
    )
    parser.add_argument(
        "--eligibility-driver",
        type=Path,
        help="matching clang driver; required for bugprone language eligibility",
    )
    parser.add_argument(
        "--parse-yaml",
        action="store_true",
        help="parse diagnostic groups with PyYAML; automatic for mixed checks",
    )
    parser.add_argument(
        "--input-manifest",
        type=Path,
        action="append",
        default=[],
        help="additional frozen file fingerprints; checked before and after runs",
    )
    parser.add_argument(
        "--verify-source",
        action="store_true",
        help="require a clean tracked source checkout at --source-revision",
    )
    parser.add_argument(
        "--timeout",
        type=positive_seconds,
        default=120.0,
        help="wall-clock limit in seconds for each analysis subprocess",
    )
    parser.add_argument(
        "--check-option",
        action="append",
        default=[],
        metavar="NAME=VALUE",
        help=(
            "HistoryList, RemarksList, HistLimit, NumThreads, or AnalyzeUnusedMainFileFunctions; "
            "identical for both tools"
        ),
    )
    parser.add_argument("--header-filter", default=".*")
    parser.add_argument(
        "--system-headers",
        action="store_true",
        help="include diagnostics in system headers",
    )
    return parser, parser.parse_args()


def select_commands(database, source_root, pattern, limit):
    data = json.loads(database.read_text())
    if not isinstance(data, list):
        raise ValueError("compilation database must contain a JSON array")
    grouped = {}
    for entry in data:
        if not isinstance(entry, dict) or not isinstance(entry.get("file"), str):
            raise ValueError("every compilation database entry must name a source file")
        directory = Path(entry.get("directory", database.parent))
        if not directory.is_absolute():
            directory = database.parent / directory
        directory = directory.resolve()
        file = Path(entry["file"])
        file = (file if file.is_absolute() else directory / file).resolve()
        try:
            relative = file.relative_to(source_root).as_posix()
        except ValueError:
            continue
        if file.suffix not in (".c", ".cc", ".cpp", ".cxx", ".C", ".m", ".mm"):
            continue
        if not pattern.search(relative):
            continue
        arguments = entry.get("arguments")
        command = entry.get("command")
        if arguments is not None:
            if (
                not isinstance(arguments, list)
                or not arguments
                or not all(isinstance(argument, str) for argument in arguments)
            ):
                raise ValueError("invalid arguments for " + str(file))
        elif not isinstance(command, str) or not command:
            raise ValueError("missing command or arguments for " + str(file))
        if not file.is_file() or not directory.is_dir():
            raise ValueError(
                "missing source file or compilation directory for " + str(file)
            )
        normalized = dict(entry, directory=str(directory), file=str(file))
        grouped.setdefault(relative, []).append(normalized)

    units = []
    for relative in sorted(grouped)[:limit]:
        commands = sorted(grouped[relative], key=json_text)
        units.append(
            {
                "id": hashlib.sha256(relative.encode()).hexdigest()[:16],
                "relative_file": relative,
                "command": commands[0],
                "discarded_commands": len(commands) - 1,
            }
        )
    if not units:
        raise ValueError("no Clang translation units match the selection")
    return units, len(data), len(grouped)


def variant_check(args, variant):
    return getattr(args, variant + "_check", CHECK)


def profile_mode(args, variant):
    mode = getattr(args, variant + "_profile_mode", None)
    if getattr(args, "profile_checks", False):
        if mode is not None:
            raise ValueError("--profile-checks cannot be combined with per-variant profile modes")
        mode = "legacy"
    mode = mode or "off"
    if mode == "phases+cir" and variant_check(args, variant) != CHECK:
        raise ValueError("phases+cir requires cir-lifetime-check")
    return mode


def analysis_config(args, variant=None):
    check = variant_check(args, variant) if variant else CHECK
    options = (
        {"HistoryList": "all", "RemarksList": "", "HistLimit": "1"}
        if check == CHECK
        else {"InvalidationFunctions": "", "ReinitializationFunctions": ""}
    )
    settings = list(getattr(args, "check_option", []))
    if settings and variant_check(args, "baseline") != variant_check(args, "candidate"):
        raise ValueError(
            "shared --check-option requires matching checks; use per-variant options"
        )
    if variant:
        settings += getattr(args, variant + "_check_option", [])
    for setting in settings:
        name, separator, value = setting.partition("=")
        name = name.removeprefix(check + ".")
        unused_functions = check == CHECK and name == "AnalyzeUnusedMainFileFunctions"
        threads = check == CHECK and name == "NumThreads"
        if not separator or (name not in options and not unused_functions and not threads):
            raise ValueError("unknown check option: " + setting)
        if threads and (not value.isascii() or not value.isdigit() or int(value) > 2**32 - 1):
            raise ValueError("NumThreads must be an unsigned 32-bit integer")
        if unused_functions:
            if value.lower() not in ("true", "false"):
                raise ValueError("AnalyzeUnusedMainFileFunctions must be true or false")
            value = value.lower()
        options[name] = value
    if check == CHECK and (
        not options["HistLimit"].isdigit() or int(options["HistLimit"]) > 2**32 - 1
    ):
        raise ValueError("HistLimit must be an unsigned 32-bit integer")
    for name, allowed in (
        ("HistoryList", {"all", "invalid", "null"}),
        ("RemarksList", {"all", "pset-invalid", "pset-always"}),
    ):
        if check == CHECK and any(
            value.strip() not in allowed
            for value in options[name].split(";")
            if value.strip()
        ):
            raise ValueError("unknown category in " + name)
    return {
        "Checks": "-*," + check,
        "CheckOptions": {check + "." + name: value for name, value in options.items()},
        "WarningsAsErrors": "",
        "HeaderFilterRegex": args.header_filter,
        "SystemHeaders": args.system_headers,
    }


def inspect_binary(binary, variant, args, output, environment):
    resolved = shutil.which(binary)
    if not resolved:
        raise ValueError("executable not found: " + binary)
    path = Path(resolved).resolve()
    check = variant_check(args, variant)
    probes = {}
    for name, flags in (
        ("version", ["--version"]),
        ("checks", ["--list-checks", "--checks=-*," + check, "--config={}"]),
    ):
        command = [str(path), *flags]
        process = subprocess.run(
            command, capture_output=True, timeout=30, env=environment
        )
        text = (process.stdout + process.stderr).decode(errors="replace")
        (output / (variant + "-" + name + ".txt")).write_text(text)
        if process.returncode:
            raise ValueError(variant + " " + name + " probe failed: " + text.strip())
        probes[name] = text
    if check not in (line.strip() for line in probes["checks"].splitlines()):
        raise ValueError(variant + " does not register " + check)
    return {
        "path": str(path),
        "sha256": file_hash(path),
        "version": probes["version"].strip(),
        "revision": getattr(args, variant + "_revision"),
        "build_type": getattr(args, variant + "_build_type"),
        "assertions": getattr(args, variant + "_assertions"),
        "check": check,
    }


def measure_process(command, cwd, environment, timeout, stdout_path, stderr_path):
    return support.measure_process(
        command, cwd, environment, timeout, stdout_path, stderr_path
    )


def parse_diagnostics(text):
    text = ANSI_CSI.sub("", text)
    diagnostics = []
    for line in text.splitlines():
        match = DIAGNOSTIC.match(line)
        if not match:
            continue
        file, row, column, level, message = match.groups()
        check = re.search(r" \[([^\]]+)\]$", message)
        diagnostics.append(
            {
                "file": file,
                "line": int(row) if row else None,
                "column": int(column) if column else None,
                "level": level,
                "message": message[: check.start()] if check else message,
                "check": check.group(1) if check else None,
            }
        )
    return diagnostics


def classify(returncode, text, diagnostics, check=CHECK):
    if returncode < 0 or 128 < returncode < 193:
        return "crash"
    text = ANSI_CSI.sub("", text)
    errors = [d for d in diagnostics if d["level"] in ("error", "fatal error")]
    # Diagnostic messages and quoted source may themselves contain crash-like
    # text. Match fatal banners outside those lines, including zero-exit tools.
    crash_text = "\n".join(
        line
        for line in text.splitlines()
        if not DIAGNOSTIC.match(line) and not re.match(r"^\s*\d*\s*\|", line)
    )
    if re.search(r"(?m)^(LLVM ERROR:|Stack dump:)", crash_text) or re.search(
        r"\bAssertion [^\n]* failed\.", crash_text
    ):
        return "crash"
    if returncode == 0 and not errors:
        return "success"
    if "CIR lifetime analysis failed" in text:
        return "pass_failure"
    if any(
        marker in text
        for marker in (
            "ClangIR code gen Not Yet Implemented",
            "CIR generation failed",
            "CIR verification failed",
        )
    ):
        return "cir_failure"
    if any(d["check"] == check for d in errors):
        return "analysis_failure"
    return "frontend_failure" if errors else "tool_failure"


def run_analysis(
    binary,
    variant,
    unit,
    repetition,
    args,
    output,
    config,
    environment,
    phase="measured",
):
    prefix = "warmup" if phase == "warmup" else "repeat"
    directory = output / "runs" / variant / unit["id"] / (prefix + "-%03d" % repetition)
    directory.mkdir(parents=True)
    stdout = directory / "stdout.txt"
    stderr = directory / "stderr.txt"
    yaml = directory / "diagnostics.yaml"
    check = variant_check(args, variant)
    mode = profile_mode(args, variant)
    invocation_config = json.loads(json.dumps(config))
    if mode in ("phases+cir", "legacy") and check == CHECK:
        invocation_config["CheckOptions"][CHECK + ".ProfileOutput"] = str(directory / "cir-profile.json")
    command = [
        binary["path"],
        "-p=" + str(output / "compilation-database"),
        "--checks=" + config["Checks"],
        "--config=" + json.dumps(invocation_config, sort_keys=True),
        "--warnings-as-errors=",
        "--use-color=false",
        "--quiet",
        "--export-fixes=" + str(yaml),
        unit["command"]["file"],
    ]
    if mode != "off":
        command[1:1] = ["--store-phase-profile=" + str(directory / "phase-profile")]
    if mode == "legacy":
        command[1:1] = ["--enable-check-profile", "--store-check-profile=" + str(directory / "check-profile")]
    cwd = unit["command"]["directory"]
    returncode, elapsed, memory, forced_status = measure_process(
        command, cwd, environment, args.timeout, stdout, stderr
    )
    diagnostics_text = (
        stdout.read_text(errors="replace") + "\n" + stderr.read_text(errors="replace")
    )
    diagnostics = parse_diagnostics(diagnostics_text)
    eligibility = (
        getattr(args, "_eligibility", {}).get(
            unit["id"],
            {"eligible": None, "reason": "language eligibility was not established"},
        )
        if check == "bugprone-use-after-move"
        else {
            "eligible": True,
            "reason": "CIR invocation; emitted-function coverage is separate",
        }
    )
    record = {
        "unit_id": unit["id"],
        "relative_file": unit["relative_file"],
        "variant": variant,
        "repetition": repetition,
        "phase": phase,
        "check": check,
        "profile_mode": mode,
        "status": forced_status
        or classify(returncode, diagnostics_text, diagnostics, check),
        "returncode": returncode,
        "wall_seconds": elapsed,
        "peak_rss_bytes": memory,
        "command": command,
        "cwd": cwd,
        "stdout_log": str(stdout.relative_to(output)),
        "stderr_log": str(stderr.relative_to(output)),
        "diagnostics_yaml": str(yaml.relative_to(output)) if yaml.exists() else None,
        "diagnostics": diagnostics,
        "eligibility": {
            key: eligibility[key]
            for key in ("eligible", "reason", "language", "standard")
            if key in eligibility
        },
    }
    if getattr(args, "parse_yaml", False):
        parsed = support.parse_yaml_diagnostics(
            yaml, cwd, sum(d["level"] == "warning" for d in diagnostics)
        )
        record["diagnostic_groups"] = parsed.pop("groups")
        record["diagnostic_export"] = parsed
    if mode != "off":
        record["tidy_phase_profile"] = support.read_phase_profile(
            directory / "phase-profile", check, unit["command"]["file"], cwd,
            stock_profiling=mode == "legacy")
    if mode == "legacy":
        record["check_profile"] = support.read_check_profile(
            directory / "check-profile", check, unit["command"]["file"], cwd)
    if mode in ("phases+cir", "legacy") and check == CHECK:
        record["cir_profile"] = support.read_cir_profile(
            directory / "cir-profile.json", unit["command"]["file"], cwd,
            allow_incomplete=True)
    record["profile_validation"] = validate_profiles(record)
    (directory / "run.json").write_text(json_text(record))
    return record


def validate_profiles(run):
    mode = run.get("profile_mode", "off")
    if mode == "off":
        return {"status": "disabled"}
    if (run["status"] != "success"
            or run.get("eligibility", {}).get("eligible") is not True):
        return {"status": "not_applicable"}
    required = ["tidy_phase_profile"]
    if mode == "legacy":
        required.append("check_profile")
    if mode in ("phases+cir", "legacy") and run["check"] == CHECK:
        required.append("cir_profile")
    failures = {name: run.get(name, {}).get("status", "missing") for name in required
                if run.get(name, {}).get("status") != "parsed"}
    if not failures:
        intervals = run["tidy_phase_profile"]["phases"]
        if sum(p["wall_seconds"] for p in intervals.values()) > run["wall_seconds"] + 1e-6:
            failures["tidy_phase_profile"] = "intervals exceed process wall time"
    return {"status": "failed", "failures": failures} if failures else {"status": "parsed"}


def summarize_legacy(report):
    def measured_maximum(records, field):
        return max(
            (run[field] for run in records
             if type(run.get(field)) in (int, float)
             and math.isfinite(run[field]) and run[field] >= 0),
            default=None,
        )

    warmups = [run for run in report["runs"] if run.get("phase") == "warmup"]
    runs = [run for run in report["runs"] if run.get("phase", "measured") == "measured"]
    repetitions = report["measurement"]["repetitions"]
    units = report["selection"]["units"]
    by_unit = defaultdict(list)
    by_variant = defaultdict(list)
    for run in runs:
        by_unit[(run["variant"], run["relative_file"])].append(run)
        by_variant[run["variant"]].append(run)
    successful = {}
    summary = {}
    for variant in ("baseline", "candidate"):
        selected = by_variant[variant]
        successful[variant] = {
            unit["relative_file"]
            for unit in units
            if len(by_unit[(variant, unit["relative_file"])]) == repetitions
            and all(
                run["status"] == "success"
                and run.get("eligibility", {}).get("eligible", True) is True
                for run in by_unit[(variant, unit["relative_file"])]
            )
        }
        execution_successful = sum(
            len(by_unit[(variant, unit["relative_file"])]) == repetitions
            and all(
                r["status"] == "success"
                for r in by_unit[(variant, unit["relative_file"])]
            )
            for unit in units
        )
        summary[variant] = {
            "execution_successful_units": execution_successful,
            "selected_units": len(units),
            "completed_runs": len(selected),
            "max_wall_seconds_all_runs": measured_maximum(selected, "wall_seconds"),
            "max_peak_rss_bytes_all_runs": measured_maximum(selected, "peak_rss_bytes"),
            "successful_units": len(successful[variant]),
            "unsuccessful_units": [
                unit["relative_file"]
                for unit in units
                if unit["relative_file"] not in successful[variant]
            ],
            "coverage_fraction": len(successful[variant]) / len(units),
            "status_counts": dict(Counter(run["status"] for run in selected)),
            "check_profile_status_counts": dict(Counter(
                run.get("check_profile", {}).get("status", "disabled") for run in selected)),
            "tidy_phase_profile_status_counts": dict(Counter(
                run.get("tidy_phase_profile", {}).get("status", "disabled")
                for run in selected)),
            "cir_profile_status_counts": dict(Counter(
                run.get("cir_profile", {}).get("status", "disabled") for run in selected)),
            "warmup_status_counts": dict(
                Counter(run["status"] for run in warmups if run["variant"] == variant)
            ),
            "ineligible_units": sorted(
                {
                    run["relative_file"]
                    for run in selected
                    if run.get("eligibility", {}).get("eligible") is False
                }
            ),
            "eligibility_unknown_units": sorted(
                {
                    run["relative_file"]
                    for run in selected
                    if run.get("eligibility", {}).get("eligible", True) is None
                }
            ),
            "diagnostic_counts_all_runs": dict(
                Counter(
                    diagnostic["level"]
                    for run in selected
                    for diagnostic in run["diagnostics"]
                )
            ),
            "diagnostic_counts_successful_runs": dict(
                Counter(
                    diagnostic["level"]
                    for run in selected
                    if run["status"] == "success"
                    for diagnostic in run["diagnostics"]
                )
            ),
        }
    common = sorted(successful["baseline"] & successful["candidate"])
    comparison = {
        "available": bool(common),
        "common_successful_units": common,
        "selected_units": len(units),
        "common_coverage_fraction": len(common) / len(units),
        "candidate_over_baseline_wall_ratio": None,
        "per_unit": [],
    }
    profiling_asymmetry = (
        report["measurement"].get("check_profiling", False)
        and report["binaries"]["baseline"]["check"]
        != report["binaries"]["candidate"]["check"]
    )
    paired_timing = (
        report["measurement"].get("workers", 1) == 1
        and report["measurement"].get("execution", "paired") == "paired"
        and not profiling_asymmetry
    )
    for file in common:
        item = {"relative_file": file}
        for variant in ("baseline", "candidate"):
            selected = by_unit[(variant, file)]
            item[variant] = {
                "median_wall_seconds": statistics.median(
                    run["wall_seconds"] for run in selected
                ),
                "median_peak_rss_bytes": statistics.median(
                    run["peak_rss_bytes"] for run in selected
                ),
                "max_peak_rss_bytes": max(run["peak_rss_bytes"] for run in selected),
            }
        if paired_timing:
            base_time = item["baseline"]["median_wall_seconds"]
            base_rss = item["baseline"]["median_peak_rss_bytes"]
            item["candidate_over_baseline_wall_ratio"] = (
                item["candidate"]["median_wall_seconds"] / base_time
                if base_time
                else None
            )
            item["candidate_over_baseline_rss_ratio"] = (
                item["candidate"]["median_peak_rss_bytes"] / base_rss
                if base_rss
                else None
            )
        comparison["per_unit"].append(item)
    if common:
        comparison["diagnostic_counts_common_successful_runs"] = {
            variant: dict(
                Counter(
                    diagnostic["level"]
                    for file in common
                    for run in by_unit[(variant, file)]
                    for diagnostic in run["diagnostics"]
                )
            )
            for variant in ("baseline", "candidate")
        }
        for variant in ("baseline", "candidate"):
            comparison[variant + "_sum_median_wall_seconds"] = sum(
                item[variant]["median_wall_seconds"] for item in comparison["per_unit"]
            )
        baseline = comparison["baseline_sum_median_wall_seconds"]
        if baseline and paired_timing:
            comparison["candidate_over_baseline_wall_ratio"] = (
                comparison["candidate_sum_median_wall_seconds"] / baseline
            )
        elif not paired_timing:
            comparison["timing_ratio_unavailable_reason"] = (
                "check profiling has matcher-dependent overhead; compare unprofiled "
                "runs for cross-check latency"
                if profiling_asymmetry else
                "only serial paired execution provides paired timing ratios"
            )
    else:
        comparison["unavailable_reason"] = (
            "no unit succeeded in every repetition for both binaries"
        )
    paired_outcomes = Counter()
    for unit in units:
        outcomes = []
        for variant in ("baseline", "candidate"):
            selected = by_unit[(variant, unit["relative_file"])]
            states = {run["status"] for run in selected}
            outcomes.append(
                next(iter(states))
                if len(states) == 1 and len(selected) == repetitions
                else "mixed_or_incomplete"
            )
        paired_outcomes["/".join(outcomes)] += 1
    report["paired_unit_outcomes"] = dict(paired_outcomes)
    report["summary"] = summary
    report["comparison"] = comparison
    if report["measurement"].get("check_profiling"):
        profiled = []
        for file in common:
            if not all(run.get("check_profile", {}).get("status") == "parsed"
                       for variant in ("baseline", "candidate")
                       for run in by_unit[(variant, file)]):
                continue
            item = {"relative_file": file}
            for variant in ("baseline", "candidate"):
                selected = by_unit[(variant, file)]
                item[variant] = {
                    field: statistics.median(run["check_profile"][field] for run in selected)
                    for field in ("wall_seconds", "user_seconds", "sys_seconds")}
                phases = [run.get("tidy_phase_profile", {}) for run in selected]
                if all(phase.get("status") == "parsed" for phase in phases):
                    item[variant]["tidy_phase_wall_seconds"] = {
                        name: statistics.median(
                            phase["phases"][name]["wall_seconds"] for phase in phases)
                        for name in ("consumer_setup", "frontend_before_tu", "tu_callbacks")}
                stages = [run.get("cir_profile", {}) for run in selected]
                if all(stage.get("status") == "parsed" for stage in stages):
                    item[variant]["cir_stage_wall_seconds"] = {
                        stage: statistics.median(s["wall_seconds"][stage] for s in stages)
                        for stage in ("generation", "verification", "canonicalization", "lifetime")}
            baseline = item["baseline"]["wall_seconds"]
            if paired_timing and baseline > 0:
                item["candidate_over_baseline_check_wall_ratio"] = item["candidate"]["wall_seconds"] / baseline
            profiled.append(item)
        report["profiling"] = {
            "common_profiled_units": len(profiled),
            "excluded_common_units": len(common) - len(profiled),
            "tidy_phase_profiled_units": {
                variant: sum("tidy_phase_wall_seconds" in item[variant]
                             for item in profiled)
                for variant in ("baseline", "candidate")},
            "cir_stage_profiled_units": {
                variant: sum("cir_stage_wall_seconds" in item[variant] for item in profiled)
                for variant in ("baseline", "candidate")},
            "per_unit": profiled,
            "timing_note": "instrumented check wall includes matching/callback and matcher-dependent timer overhead; CIR includes generation and passes; process residual is not pure AST time; cross-check timing ratios require unprofiled runs",
        }


def summarize(report):
    # Historical extended-stock reports retain their original interpretation.
    if report.get("schema_version", 2) < 3:
        return summarize_legacy(report)
    modes = report["measurement"]["profile_modes"]
    # Reuse execution/coverage accounting, but never the old profiled ratios.
    legacy = dict(report, measurement=dict(report["measurement"], check_profiling=False))
    summarize_legacy(legacy)
    for key in ("summary", "comparison", "paired_unit_outcomes"):
        report[key] = legacy[key]
    runs = [r for r in report["runs"] if r.get("phase", "measured") == "measured"]
    by_unit = defaultdict(list)
    for run in runs:
        by_unit[(run["variant"], run["relative_file"])].append(run)
    validation_failures = []
    for variant in ("baseline", "candidate"):
        selected = [r for r in runs if r["variant"] == variant]
        for run in selected:
            run["profile_validation"] = validate_profiles(run)
            if run["profile_validation"]["status"] == "failed":
                validation_failures.append({"variant": variant, "unit_id": run["unit_id"],
                                           "repetition": run["repetition"],
                                           **run["profile_validation"]})
        report["summary"][variant]["profile_validation_status_counts"] = dict(Counter(
            r["profile_validation"]["status"] for r in selected))
        report["summary"][variant]["sum_wall_seconds_all_runs"] = sum(r["wall_seconds"] for r in selected)
    report["measurement_validation"] = {
        "status": "failed" if validation_failures else "valid",
        "failure_count": len(validation_failures), "failures": validation_failures}
    comparison = report["comparison"]
    common = comparison["common_successful_units"]
    timing_units = [file for file in common if all(
        r["profile_validation"]["status"] in ("disabled", "parsed")
        for variant in ("baseline", "candidate") for r in by_unit[(variant, file)])]
    comparison["timing_units"] = timing_units
    timing_unit_set = set(timing_units)
    comparison["available"] = bool(timing_units)
    if common and not timing_units:
        comparison["unavailable_reason"] = "no common successful units have valid requested profiles"
    comparison["excluded_profile_units"] = sorted(set(common) - set(timing_units))
    # Do not silently salvage a campaign with broken successful measurements.
    comparative_modes = modes["baseline"] == modes["candidate"] and modes["baseline"] in ("off", "phases")
    same_analysis = (
        report["binaries"]["baseline"]["check"] == report["binaries"]["candidate"]["check"]
        and report["binaries"]["baseline"].get("sha256") is not None
        and report["binaries"]["baseline"]["sha256"] == report["binaries"]["candidate"].get("sha256")
        and report.get("analysis_configs", {}).get("baseline") is not None
        and report["analysis_configs"]["baseline"] == report["analysis_configs"].get("candidate"))
    same_instrument = same_analysis and set(modes.values()) in (
        {"off", "phases"}, {"off", "legacy"})
    serial = report["measurement"].get("workers", 1) == 1 and report["measurement"].get("execution", "paired") == "paired"
    allow_ratio = serial and not validation_failures and (comparative_modes or same_instrument)
    comparison["comparison_kind"] = (
        "instrumentation_overhead" if same_instrument else
        "repeatability" if same_analysis and modes["baseline"] == modes["candidate"] else
        "checker_comparison")
    comparison["candidate_over_baseline_wall_ratio"] = None
    comparison.pop("timing_ratio_unavailable_reason", None)
    comparison["per_unit"] = [item for item in comparison["per_unit"] if item["relative_file"] in timing_unit_set]
    for item in comparison["per_unit"]:
        item.pop("candidate_over_baseline_wall_ratio", None)
        item.pop("candidate_over_baseline_rss_ratio", None)
        if allow_ratio:
            base = item["baseline"]
            item["candidate_over_baseline_wall_ratio"] = item["candidate"]["median_wall_seconds"] / base["median_wall_seconds"] if base["median_wall_seconds"] else None
            item["candidate_over_baseline_rss_ratio"] = item["candidate"]["median_peak_rss_bytes"] / base["median_peak_rss_bytes"] if base["median_peak_rss_bytes"] else None
    for variant in ("baseline", "candidate"):
        comparison[variant + "_sum_median_wall_seconds"] = sum(item[variant]["median_wall_seconds"] for item in comparison["per_unit"])
        report["summary"][variant]["excluded_input_wall_seconds"] = sum(
            r["wall_seconds"] for r in runs if r["variant"] == variant and r["relative_file"] not in timing_unit_set)
    baseline = comparison["baseline_sum_median_wall_seconds"]
    if allow_ratio and baseline:
        comparison["candidate_over_baseline_wall_ratio"] = comparison["candidate_sum_median_wall_seconds"] / baseline
    elif not allow_ratio:
        comparison["timing_ratio_unavailable_reason"] = (
            "profile validation failed on successful eligible invocations" if validation_failures else
            "only serial paired execution provides paired timing ratios" if not serial else
            "matcher-dependent stock timers or asymmetric detailed profiling; compare matching off or phases modes")
    if not any(mode != "off" for mode in modes.values()):
        return
    profiled = []
    for file in timing_units:
        item = {"relative_file": file}
        for variant in ("baseline", "candidate"):
            selected = by_unit[(variant, file)]
            entry = item[variant] = {"profile_mode": modes[variant]}
            if modes[variant] != "off":
                entry["tidy_phase_wall_seconds"] = {
                    name: statistics.median(r["tidy_phase_profile"]["phases"][name]["wall_seconds"] for r in selected)
                    for name in ("consumer_setup", "frontend_before_tu", "tu_callbacks")}
                entry["median_process_residual_seconds"] = statistics.median(
                    r["wall_seconds"] - sum(p["wall_seconds"] for p in r["tidy_phase_profile"]["phases"].values()) for r in selected)
            if modes[variant] == "legacy":
                entry["stock_check_seconds"] = {field: statistics.median(r["check_profile"][field] for r in selected)
                                                for field in ("wall_seconds", "user_seconds", "sys_seconds")}
            if modes[variant] in ("phases+cir", "legacy") and selected[0]["check"] == CHECK:
                entry["cir_stage_wall_seconds"] = {
                    stage: statistics.median(r["cir_profile"]["wall_seconds"][stage] for r in selected)
                    for stage in ("generation", "verification", "canonicalization", "lifetime")}
        # TU intervals are comparable only in matching independent modes.
        if allow_ratio and modes["baseline"] == modes["candidate"] == "phases":
            base = item["baseline"]["tidy_phase_wall_seconds"]["tu_callbacks"]
            item["candidate_over_baseline_tu_analysis_wall_ratio"] = item["candidate"]["tidy_phase_wall_seconds"]["tu_callbacks"] / base if base else None
        profiled.append(item)
    report["profiling"] = {
        "common_profiled_units": len(profiled),
        "excluded_common_units": len(common) - len(profiled),
        "tidy_phase_profiled_units": {v: sum("tidy_phase_wall_seconds" in item[v] for item in profiled) for v in modes},
        "cir_stage_profiled_units": {v: sum("cir_stage_wall_seconds" in item[v] for item in profiled) for v in modes},
        "per_unit": profiled,
        "timing_note": "TU-analysis wall includes AST matching, callbacks, in-TU diagnostics, and CIR generation/passes/context teardown; it is not pure solver time. Residual includes startup, teardown, serialization and wrapper overhead, not pure AST time. Stock timers remain matcher dependent."}


def save_report(report, output):
    summarize(report)
    temporary = output / "report.json.tmp"
    temporary.write_text(json_text(report))
    temporary.replace(output / "report.json")


def verify_source(root, revision):
    try:
        actual = subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=root, text=True, stderr=subprocess.PIPE
        ).strip()
        dirty = subprocess.check_output(
            ["git", "diff", "--name-only", "HEAD"],
            cwd=root,
            text=True,
            stderr=subprocess.PIPE,
        ).strip()
    except subprocess.CalledProcessError as error:
        raise ValueError("cannot verify source Git checkout: " + str(root)) from error
    if actual != revision or dirty:
        raise ValueError("source checkout is not clean at --source-revision")
    return {
        "revision": actual,
        "tree": subprocess.check_output(
            ["git", "rev-parse", "HEAD^{tree}"], cwd=root, text=True
        ).strip(),
        "tracked_clean": True,
    }


def benchmark(args):
    if not hasattr(os, "wait4") or not (
        sys.platform.startswith("linux") or sys.platform == "darwin"
    ):
        raise ValueError(
            "per-process RSS measurement requires Linux or macOS with wait4"
        )
    source_root = args.clang_source_root.resolve()
    database = args.compilation_database.resolve()
    if database.is_dir():
        database /= "compile_commands.json"
    profile_modes = {v: profile_mode(args, v) for v in ("baseline", "candidate")}
    configs = {
        variant: analysis_config(args, variant) for variant in ("baseline", "candidate")
    }
    mode = args.execution_mode or ("paired" if args.jobs == 1 else "batches")
    if mode == "paired" and args.jobs != 1:
        raise ValueError(
            "paired execution requires --jobs=1; use batches for parallel runs"
        )
    if (
        any(variant_check(args, v) == "bugprone-use-after-move" for v in configs)
        and not args.eligibility_driver
    ):
        raise ValueError("bugprone comparison requires --eligibility-driver")
    args.parse_yaml |= variant_check(args, "baseline") != variant_check(
        args, "candidate"
    )
    if args.parse_yaml:
        yaml_version = support.require_yaml().__version__
    else:
        yaml_version = None
    if args.selection_file:
        if args.file_filter != ".*" or args.limit is not None:
            raise ValueError(
                "a frozen --selection-file cannot be combined with filtering or a limit"
            )
        units = support.frozen_selection(args.selection_file)
        entry_count = len(json.loads(database.read_text()))
        eligible_count = len(units)
    else:
        units, entry_count, eligible_count = select_commands(
            database, source_root, re.compile(args.file_filter), args.limit
        )
    expected_inputs = []
    for manifest in args.input_manifest:
        expected_inputs.extend(support.read_input_manifest(manifest))
    source_proof = (
        verify_source(source_root, args.source_revision) if args.verify_source else None
    )
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise ValueError("output directory must be empty: " + str(output))
    frozen = output / "compilation-database"
    frozen.mkdir()
    (frozen / "compile_commands.json").write_text(
        json_text([unit["command"] for unit in units])
    )
    environment = dict(os.environ, CLANG_NO_DEFAULT_CONFIG="1")
    binaries = {
        variant: inspect_binary(
            getattr(args, variant), variant, args, output, environment
        )
        for variant in ("baseline", "candidate")
    }
    input_paths = {
        str(database),
        str(frozen / "compile_commands.json"),
        str(Path(__file__).resolve()),
        str(_support_path.resolve()),
        *[item["path"] for item in binaries.values()],
        *[unit["command"]["file"] for unit in units],
        *[str(p.resolve()) for p in args.input_manifest],
        *[item["path"] for item in expected_inputs],
    }
    if args.selection_file:
        input_paths.add(str(args.selection_file.resolve()))
    if args.eligibility_driver:
        args.eligibility_driver = args.eligibility_driver.resolve()
        input_paths.add(str(args.eligibility_driver))
    fingerprints = [support.fingerprint(path) for path in sorted(input_paths)]
    (output / "input-fingerprints.json").write_text(json_text(fingerprints))
    args._eligibility = {}
    if args.eligibility_driver:
        cache = {}
        for unit in units:
            try:
                args._eligibility[unit["id"]] = support.effective_language(
                    unit["command"],
                    args.eligibility_driver,
                    output / "eligibility" / unit["id"],
                    environment,
                    cache,
                )
            except (OSError, subprocess.TimeoutExpired) as error:
                args._eligibility[unit["id"]] = {"eligible": None, "reason": str(error)}
        (output / "eligibility.json").write_text(json_text(args._eligibility))
    support.verify_fingerprints(fingerprints)
    report = {
        "schema_version": 3,
        "started_at": timestamp(),
        "finished_at": None,
        "complete": False,
        "invocation": sys.argv,
        "binaries": binaries,
        "source": {
            "root": str(source_root),
            "revision": args.source_revision,
            "verified_checkout": source_proof,
        },
        "host": {
            "platform": platform.platform(),
            "node": platform.node(),
            "machine": platform.machine(),
            "cpu_count": os.cpu_count(),
            "python": sys.version,
        },
        "measurement": {
            "repetitions": args.repetitions,
            "timeout_seconds": args.timeout,
            "workers": args.jobs,
            "execution": mode,
            "memory": "compiler-child wait4 ru_maxrss via a fresh small supervisor, normalized to bytes; residual supervisor-image floor, not private heap or aggregate process-tree RSS",
            "warmup_runs": args.warmups,
            "check_profiling": any(m == "legacy" for m in profile_modes.values()),
            "profile_modes": profile_modes,
        },
        "analysis_config": (
            configs["baseline"] if configs["baseline"] == configs["candidate"] else None
        ),
        "analysis_configs": configs,
        "diagnostic_parser": {
            "yaml_enabled": args.parse_yaml,
            "pyyaml_version": yaml_version,
        },
        "input_freeze": {
            "scope": "runner/helpers, executables, selected source files, databases, explicit input manifests; tracked source tree only with --verify-source",
            "manifest": "input-fingerprints.json",
            "manifest_sha256": file_hash(output / "input-fingerprints.json"),
            "files": len(fingerprints),
            "final_verified": False,
        },
        "eligibility_manifest": "eligibility.json" if args.eligibility_driver else None,
        "environment_overrides": {"CLANG_NO_DEFAULT_CONFIG": "1"},
        "selection": {
            "input_database": str(database),
            "input_database_sha256": file_hash(database),
            "input_entries": entry_count,
            "eligible_units": eligible_count,
            "file_filter": args.file_filter,
            "limit": args.limit,
            "frozen_database_sha256": file_hash(frozen / "compile_commands.json"),
            "duplicate_policy": (
                "explicit frozen selection"
                if args.selection_file
                else "lexicographically first normalized JSON command per source path"
            ),
            "selection_file": (
                str(args.selection_file.resolve()) if args.selection_file else None
            ),
            "units": units,
        },
        "runs": [],
        "batches": [],
    }
    save_report(report, output)

    def progress(run):
        report["runs"].append(run)
        if len(report["runs"]) % 128 == 0:
            path = output / "progress.json.tmp"
            path.write_text(
                json_text(
                    {
                        "completed_runs": len(report["runs"]),
                        "updated_at": timestamp(),
                        "phase": run["phase"],
                        "variant": run["variant"],
                    }
                )
            )
            path.replace(output / "progress.json")

    try:
        rounds = [("warmup", i) for i in range(1, args.warmups + 1)] + [
            ("measured", i) for i in range(1, args.repetitions + 1)
        ]
        for round_index, (phase, repetition) in enumerate(rounds):
            support.verify_fingerprints(fingerprints)
            if args.verify_source:
                verify_source(source_root, args.source_revision)
            if mode == "paired":
                started, start = timestamp(), time.perf_counter()
                for index, unit in enumerate(units):
                    order = (
                        ("baseline", "candidate")
                        if (index + repetition) % 2
                        else ("candidate", "baseline")
                    )
                    for variant in order:
                        run = run_analysis(
                            binaries[variant],
                            variant,
                            unit,
                            repetition,
                            args,
                            output,
                            configs[variant],
                            environment,
                            phase,
                        )
                        progress(run)
                report["batches"].append(
                    {
                        "phase": phase,
                        "repetition": repetition,
                        "variant": "paired",
                        "started_at": started,
                        "finished_at": timestamp(),
                        "wall_seconds": time.perf_counter() - start,
                        "completed_runs": 2 * len(units),
                    }
                )
            else:
                order = (
                    ("baseline", "candidate")
                    if round_index % 2 == 0
                    else ("candidate", "baseline")
                )
                for variant in order:
                    started, start = timestamp(), time.perf_counter()
                    completed = []
                    with ThreadPoolExecutor(max_workers=args.jobs) as executor:
                        futures = [
                            executor.submit(
                                run_analysis,
                                binaries[variant],
                                variant,
                                unit,
                                repetition,
                                args,
                                output,
                                configs[variant],
                                environment,
                                phase,
                            )
                            for unit in units
                        ]
                        for future in as_completed(futures):
                            run = future.result()
                            completed.append(run)
                            progress(run)
                    report["batches"].append(
                        {
                            "phase": phase,
                            "repetition": repetition,
                            "variant": variant,
                            "started_at": started,
                            "finished_at": timestamp(),
                            "wall_seconds": time.perf_counter() - start,
                            "completed_runs": len(completed),
                            "status_counts": dict(
                                Counter(run["status"] for run in completed)
                            ),
                        }
                    )
                    print(
                        f"{phase} {repetition} {variant}: {len(completed)} invocations",
                        flush=True,
                    )
            save_report(report, output)
        expected_count = 2 * len(units) * (args.repetitions + args.warmups)
        if (
            len(report["runs"]) != expected_count
            or len(
                {
                    (r["variant"], r["unit_id"], r["phase"], r["repetition"])
                    for r in report["runs"]
                }
            )
            != expected_count
        ):
            raise ValueError("incomplete or duplicate run accounting")
        support.verify_fingerprints(fingerprints)
        if args.verify_source:
            verify_source(source_root, args.source_revision)
        report["input_freeze"]["final_verified"] = True
        report["input_freeze"]["verified_at"] = timestamp()
    except KeyboardInterrupt:
        report["interrupted"] = True
        save_report(report, output)
        raise
    except BaseException as error:
        report["error"] = str(error)
        save_report(report, output)
        raise
    report["complete"] = True
    report["finished_at"] = timestamp()
    save_report(report, output)
    for variant in ("baseline", "candidate"):
        count = report["summary"][variant]["successful_units"]
        print(f"{variant}: {count}/{len(units)} units succeeded in all repetitions")
    comparison = report["comparison"]
    count = len(comparison["timing_units"])
    ratio = comparison["candidate_over_baseline_wall_ratio"]
    print(f"Validated timing cohort: {count}/{len(units)} units")
    if report["measurement_validation"]["failure_count"]:
        print("Measurement validation failed: " +
              str(report["measurement_validation"]["failure_count"]) +
              " successful eligible invocations have invalid requested profiles")
    print(
        f"Candidate/baseline wall-time ratio: {ratio:.4f}"
        if ratio is not None
        else "Timing comparison unavailable"
    )
    print("Report: " + str(output / "report.json"))


def main():
    parser, args = parse_args()
    try:
        benchmark(args)
    except (OSError, ValueError, re.error, subprocess.TimeoutExpired) as error:
        parser.exit(2, "error: " + str(error) + "\n")
    except KeyboardInterrupt:
        return 130
    return 0


if __name__ == "__main__":
    sys.exit(main())
