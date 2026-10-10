#!/usr/bin/env python3
#
# Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Prepare reproducible diagnostic review and serial timing samples.

Consumes a completed benchmark-cir-lifetime.py report. It does not execute a
compiler or classify a warning as a defect. Edit the generated review sample's
per-check verdicts only after examining the source and attached diagnostics.
"""

import argparse
import bisect
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import sys
import re
import subprocess

CIR = "cir-lifetime-check"
BUGPRONE = "bugprone-use-after-move"
BUCKETS = ("shared", "cir_only", "bugprone_only")
VERDICTS = (
    "actual_defect",
    "false_positive",
    "policy_only",
    "intentional_test",
    "unresolved",
)


def stable_id(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True).encode()).hexdigest()[:24]


def validate_report(report):
    if not report.get("complete") or not report.get("input_freeze", {}).get(
        "final_verified"
    ):
        raise ValueError(
            "a complete report with a verified final input freeze is required"
        )
    units = report["selection"]["units"]
    ids = [unit["id"] for unit in units]
    if not ids or len(set(ids)) != len(ids):
        raise ValueError("empty or duplicate selection")
    id_set = set(ids)
    repetitions = report["measurement"]["repetitions"]
    indexed = defaultdict(list)
    seen = set()
    for run in report["runs"]:
        key = (
            run["variant"],
            run["unit_id"],
            run.get("phase", "measured"),
            run["repetition"],
        )
        if key in seen:
            raise ValueError("duplicate run: " + str(key))
        seen.add(key)
        if (
            key[0] not in ("baseline", "candidate")
            or key[1] not in id_set
            or key[2] not in ("warmup", "measured")
        ):
            raise ValueError("unexpected run: " + str(key))
        if key[2] == "measured":
            if not 1 <= key[3] <= repetitions:
                raise ValueError("unexpected repetition")
            indexed[key[:2]].append(run)
    if any(
        len(indexed[(variant, unit)]) != repetitions
        for variant in ("baseline", "candidate")
        for unit in ids
    ):
        raise ValueError("incomplete measured run accounting")
    for runs in indexed.values():
        runs.sort(key=lambda run: run["repetition"])
    return units, indexed


def eligible_success(runs):
    return all(
        run["status"] == "success"
        and run.get("eligibility", {}).get("eligible") is True
        for run in runs
    )


def clang_unit(unit):
    # Generated Clang TUs retain project=clang in a frozen selection.
    return unit.get("project") == "clang" or unit["relative_file"].startswith("clang/")


def group_kind(group):
    if group.get("level") != "warning":
        return None
    if group.get("check") == BUGPRONE:
        return "uam"
    if group.get("check") != CIR:
        return None
    message = group["primary"]["message"].lower()
    potential_move = ("moved-from" in message or "moved from" in message
                      or "invalid pointer" in message)
    if potential_move and any(
        "moved here" in note["message"].lower()
        or "move occurred here" in note["message"].lower()
        for note in group["notes"]
    ):
        return "uam"
    # Absence of a move note is not proof of a different cause: history limits
    # and merged paths can hide it. Keep potential matches out of exclusive
    # populations until the source and complete history have been reviewed.
    # Some empty library values use the legacy "moved-from" primary wording
    # even when their history records null initialization rather than a move.
    if potential_move:
        return "ambiguous_cause"
    return "other_lifetime"


def group_signature(groups):
    # Compare multisets, not output order, across measured repetitions.
    return sorted(json.dumps(group, sort_keys=True) for group in groups)


def collect_sites(unit_runs, units):
    sites = {}
    unlocated = []
    by_id = {unit["id"]: unit for unit in units}
    for variant, unit_id, runs in unit_runs:
        run = runs[0]
        for group in run.get("diagnostic_groups", []):
            kind = group_kind(group)
            if not kind:
                continue
            primary = group["primary"]
            provenance = {
                "unit_id": unit_id,
                "relative_file": by_id[unit_id]["relative_file"],
                "project": by_id[unit_id].get("project"),
                "repetitions": [r["repetition"] for r in runs],
                "stdout_logs": [r["stdout_log"] for r in runs],
                "stderr_logs": [r["stderr_log"] for r in runs],
                "diagnostics_yaml": [r.get("diagnostics_yaml") for r in runs],
            }
            if not primary.get("file") or primary.get("offset") is None:
                unlocated.append(
                    {
                        "variant": variant,
                        "kind": kind,
                        "group": group,
                        "provenance": provenance,
                    }
                )
                continue
            key = (variant, primary["file"], primary["offset"])
            if key not in sites:
                sites[key] = {
                    "id": stable_id(key),
                    "variant": variant,
                    "kind": kind,
                    "file": primary["file"],
                    "offset": primary["offset"],
                    "groups": [],
                    "provenance": [],
                    "clang_observed": False,
                }
            site = sites[key]
            priority = {"other_lifetime": 0, "ambiguous_cause": 1, "uam": 2}
            if priority[kind] > priority[site["kind"]]:
                site["kind"] = kind
            if group not in site["groups"]:
                site["groups"].append(group)
            if provenance not in site["provenance"]:
                site["provenance"].append(provenance)
            site["clang_observed"] |= clang_unit(by_id[unit_id])
    return list(sites.values()), unlocated


def intervals(site):
    spans = [(site["file"], site["offset"], site["offset"] + 1)]
    for group in site["groups"]:
        for span in group["primary"].get("ranges", []):
            if span.get("file") and span.get("length", 0) > 0:
                spans.append(
                    (span["file"], span["offset"], span["offset"] + span["length"])
                )
    return spans


def overlaps(left, right):
    return any(
        a[0] == b[0] and max(a[1], b[1]) < min(a[2], b[2])
        for a in intervals(left)
        for b in intervals(right)
    )


def statement_boundaries(source):
    """Find conservative semicolon neighborhoods, not C++ semantic identities.

    Ignore comments/literals and semicolons inside parentheses. Braces do not
    split neighborhoods: initializer and lambda braces cannot safely be treated
    as statement boundaries without an AST. A shared neighborhood only makes a
    match ambiguous; it never proves two warnings refer to the same use.
    """
    boundaries, index, parens = [], 0, 0
    while index < len(source):
        if source[index : index + 2] == b"//":
            end = source.find(b"\n", index + 2)
            index = len(source) if end < 0 else end + 1
            continue
        if source[index : index + 2] == b"/*":
            end = source.find(b"*/", index + 2)
            if end < 0:
                return None
            index = end + 2
            continue
        if source[index : index + 2] == b'R"':
            begin = source.find(b"(", index + 2, index + 20)
            if begin < 0:
                return None
            delimiter = source[index + 2 : begin]
            end = source.find(b")" + delimiter + b'"', begin + 1)
            if end < 0:
                return None
            index = end + len(delimiter) + 2
            continue
        token = source[index]
        if token in (ord('"'), ord("'")):
            # Digit separators are not the start of character literals.
            if (
                token == ord("'")
                and index
                and chr(source[index - 1]).isalnum()
                and index + 1 < len(source)
                and chr(source[index + 1]).isalnum()
            ):
                index += 1
                continue
            index += 1
            while index < len(source) and source[index] != token:
                index += 2 if source[index] == ord("\\") else 1
            if index >= len(source):
                return None
        elif token == ord("("):
            parens += 1
        elif token == ord(")"):
            parens = max(0, parens - 1)
        elif token == ord(";") and parens == 0:
            boundaries.append(index)
        index += 1
    return boundaries


class FrozenSources:
    """Read fingerprinted files or tracked files from the pinned Git revision.

    Never use an unverified current header for source association. Missing
    snapshots make associations uncertain instead of producing exclusive sites.
    """

    def __init__(self, report, directory):
        self.source = report["source"]
        self.cache = {}
        self.audit = {}
        self.fingerprints = {}
        freeze = report.get("input_freeze", {})
        if freeze.get("manifest"):
            path = directory / freeze["manifest"]
            raw = path.read_bytes()
            if hashlib.sha256(raw).hexdigest() != freeze.get("manifest_sha256"):
                raise ValueError("input fingerprint manifest changed after capture")
            self.fingerprints = {record["path"]: record for record in json.loads(raw)}
        self.git_root = None
        if self.source.get("verified_checkout"):
            try:
                self.git_root = Path(
                    subprocess.check_output(
                        ["git", "rev-parse", "--show-toplevel"],
                        cwd=self.source["root"],
                        stderr=subprocess.PIPE,
                        text=True,
                    ).strip()
                )
            except (OSError, subprocess.CalledProcessError):
                pass

    def get(self, file):
        if file in self.cache:
            return self.cache[file]
        content = None
        provenance = {"status": "unavailable"}
        expected = self.fingerprints.get(file)
        if expected:
            try:
                candidate = Path(file).read_bytes()
                if hashlib.sha256(candidate).hexdigest() == expected["sha256"]:
                    content = candidate
                    provenance = {
                        "status": "available",
                        "origin": "captured fingerprint",
                        "sha256": expected["sha256"],
                    }
                else:
                    provenance["reason"] = "current source fingerprint differs"
            except OSError as error:
                provenance["reason"] = str(error)
        revision = self.source.get("revision", "")
        if (
            content is None
            and self.git_root
            and re.fullmatch(r"[0-9a-f]{40}", revision)
        ):
            try:
                relative = Path(file).relative_to(self.git_root).as_posix()
                content = subprocess.check_output(
                    ["git", "show", revision + ":" + relative],
                    cwd=self.git_root,
                    stderr=subprocess.PIPE,
                )
                provenance = {
                    "status": "available",
                    "origin": "pinned Git object",
                    "revision": revision,
                    "path": relative,
                    "sha256": hashlib.sha256(content).hexdigest(),
                }
            except (OSError, ValueError, subprocess.CalledProcessError):
                pass
        self.cache[file] = content
        self.audit[file] = provenance
        return content


def paired_site(claims, method):
    return {
        "id": stable_id(sorted((v, c["file"], c["offset"]) for v, c in claims.items())),
        "claims": claims,
        "match_method": method,
        "match_needs_review": method == "unique_range_overlap",
        "clang_observed": any(claim["clang_observed"] for claim in claims.values()),
    }


def match_sites(sites, variants, sources):
    left = {site["id"]: site for site in sites if site["variant"] == variants[CIR]}
    right = {
        site["id"]: site for site in sites if site["variant"] == variants[BUGPRONE]
    }
    buckets = {name: [] for name in BUCKETS}
    ambiguous = []
    by_location = {(site["file"], site["offset"]): key for key, site in right.items()}
    for key, site in list(left.items()):
        other_key = by_location.get((site["file"], site["offset"]))
        if (
            other_key is not None
            and site["kind"] == "uam"
            and right[other_key]["kind"] == "uam"
        ):
            other = right.pop(other_key)
            buckets["shared"].append(
                paired_site(
                    {site["variant"]: site, other["variant"]: other},
                    "exact_byte_offset",
                )
            )
            del left[key]
    # Build connected components of remaining range matches. Multiple possible
    # partners stay outside all three review strata, including unmatched-only.
    adjacency = defaultdict(set)
    edge_reasons = {}
    contexts = {}

    def context(site):
        file = site["file"]
        if file not in contexts:
            content = sources.get(file)
            contexts[file] = (
                content,
                statement_boundaries(content) if content is not None else None,
            )
        content, boundaries = contexts[file]
        offset = site["offset"]
        if boundaries is None or not 0 <= offset < len(content):
            return None
        return bisect.bisect_left(boundaries, offset)

    right_by_file = defaultdict(set)
    for rid, site in right.items():
        for file, _, _ in intervals(site):
            right_by_file[file].add(rid)
    for lid, site in left.items():
        candidates = set().union(
            *(right_by_file[file] for file, _, _ in intervals(site))
        )
        for rid in candidates:
            other = right[rid]
            reason = "range" if overlaps(site, other) else None
            if reason is None and site["file"] == other["file"]:
                a, b = context(site), context(other)
                if a is None or b is None:
                    reason = "frozen source unavailable or unparseable"
                elif a == b:
                    reason = "different diagnostic points in the same source statement"
            if reason:
                adjacency[lid].add(rid)
                adjacency[rid].add(lid)
                edge_reasons[(lid, rid)] = reason
    seen = set()
    for node in sorted(adjacency):
        if node in seen:
            continue
        component, pending = set(), [node]
        while pending:
            current = pending.pop()
            if current in component:
                continue
            component.add(current)
            pending.extend(adjacency[current] - component)
        seen |= component
        lkeys, rkeys = component & left.keys(), component & right.keys()
        reasons = {
            edge_reasons[(lid, rid)]
            for lid in lkeys
            for rid in rkeys
            if (lid, rid) in edge_reasons
        }
        cause_unknown = any(left[key]["kind"] == "ambiguous_cause" for key in lkeys)
        if len(lkeys) == len(rkeys) == 1 and reasons == {"range"} and not cause_unknown:
            a, b = left.pop(next(iter(lkeys))), right.pop(next(iter(rkeys)))
            buckets["shared"].append(
                paired_site({a["variant"]: a, b["variant"]: b}, "unique_range_overlap")
            )
        else:
            ambiguous.append(
                {
                    "id": stable_id(sorted(component)),
                    "reason": ("ambiguous move cause; " if cause_unknown else "")
                    + ("multiple candidate source uses; " if len(component) > 2 else "")
                    + "; ".join(sorted(reasons)),
                    "cir_sites": [left.pop(key) for key in sorted(lkeys)],
                    "bugprone_sites": [right.pop(key) for key in sorted(rkeys)],
                }
            )
    for key, site in list(left.items()):
        if site["kind"] == "ambiguous_cause":
            ambiguous.append(
                {
                    "id": site["id"],
                    "reason": "ambiguous move cause",
                    "cir_sites": [left.pop(key)],
                    "bugprone_sites": [],
                }
            )
    for name, remaining in [("cir_only", left), ("bugprone_only", right)]:
        buckets[name] = [
            paired_site({site["variant"]: site}, "unmatched")
            for site in remaining.values()
        ]
    for values in buckets.values():
        values.sort(key=lambda site: site["id"])
    return buckets, ambiguous


def compare_report(report, sources=None):
    units, indexed = validate_report(report)
    variants = {info["check"]: variant for variant, info in report["binaries"].items()}
    if set(variants) != {CIR, BUGPRONE}:
        raise ValueError(
            "diagnostic matching requires one CIR and one bugprone variant"
        )
    common, diagnostic_cohort, exclusions = [], [], {}
    selected, exclusive, inactive = [], [], []
    for unit in units:
        uid = unit["id"]
        successful = {
            variant: eligible_success(indexed[(variant, uid)])
            for variant in ("baseline", "candidate")
        }
        if not all(successful.values()):
            exclusions[uid] = {
                variant: {
                    "statuses": [r["status"] for r in indexed[(variant, uid)]],
                    "eligibility": indexed[(variant, uid)][0].get("eligibility"),
                }
                for variant in successful
            }
            # Coverage-exclusive diagnostics require a successful, active check;
            # outputs from failed/ineligible invocations remain in raw artifacts.
            for variant, success in successful.items():
                runs = indexed[(variant, uid)]
                if success and all(
                    r.get("diagnostic_export", {}).get("status") in ("parsed", "empty")
                    for r in runs
                ):
                    if any(
                        r.get("eligibility", {}).get("eligible") is not True
                        for other in successful
                        for r in indexed[(other, uid)]
                    ):
                        inactive.append((variant, uid, runs))
                    else:
                        exclusive.append((variant, uid, runs))
            continue
        common.append(uid)
        reasons = {}
        for variant in successful:
            runs = indexed[(variant, uid)]
            if any(
                r.get("diagnostic_export", {}).get("status") not in ("parsed", "empty")
                for r in runs
            ):
                reasons[variant] = "missing, invalid, or incomplete diagnostic YAML"
            elif any(
                group_signature(r.get("diagnostic_groups", []))
                != group_signature(runs[0].get("diagnostic_groups", []))
                for r in runs[1:]
            ):
                reasons[variant] = (
                    "diagnostic groups differ across measured repetitions"
                )
        if reasons:
            exclusions[uid] = reasons
        else:
            diagnostic_cohort.append(uid)
            selected.extend(
                (variant, uid, indexed[(variant, uid)]) for variant in successful
            )
    sites, unlocated = collect_sites(selected, units)
    coverage_sites, coverage_unlocated = collect_sites(exclusive, units)
    inactive_sites, inactive_unlocated = collect_sites(inactive, units)
    buckets, ambiguous = match_sites(
        [s for s in sites if s["kind"] in ("uam", "ambiguous_cause")],
        variants,
        sources if sources is not None else {},
    )
    return {
        "schema_version": 1,
        "source": report["source"],
        "variants": variants,
        "common_eligible_successful_units": common,
        "diagnostic_cohort_units": diagnostic_cohort,
        "diagnostic_exclusions": exclusions,
        "buckets": buckets,
        "ambiguous_matches": ambiguous,
        "other_lifetime_sites": [s for s in sites if s["kind"] == "other_lifetime"],
        "cause_ambiguous_sites": [s for s in sites if s["kind"] == "ambiguous_cause"],
        "ineligible_comparison_sites": inactive_sites,
        "ineligible_unlocated": inactive_unlocated,
        "coverage_exclusive": coverage_sites,
        "coverage_unlocated": coverage_unlocated,
        "unlocated": unlocated,
        "interpretation": "Source-use matches are candidates for manual review, not distinct defects. No warning is automatically judged correct.",
    }


def deterministic_order(values, seed, stratum, identity):
    return sorted(
        values,
        key=lambda item: (stable_id([seed, stratum, identity(item)]), identity(item)),
    )


def review_sample(comparison, size_per_bucket=100, seed=20261010):
    result = {
        "schema_version": 1,
        "seed": seed,
        "requested_per_bucket": size_per_bucket,
        "verdict_categories": list(VERDICTS),
        "strata": {},
        "sites": [],
    }
    for bucket in BUCKETS:
        population = comparison["buckets"][bucket]
        strata = {
            name: [
                site
                for site in population
                if site["clang_observed"] == (name == "clang")
            ]
            for name in ("clang", "other")
        }
        counts = {
            "clang": min(len(strata["clang"]), size_per_bucket // 2),
            "other": min(len(strata["other"]), size_per_bucket - size_per_bucket // 2),
        }
        spare = size_per_bucket - sum(counts.values())
        for name in ("clang", "other"):
            extra = min(spare, len(strata[name]) - counts[name])
            counts[name] += extra
            spare -= extra
        for name, population in strata.items():
            key, count = bucket + "/" + name, counts[name]
            weight = len(population) / count if count else None
            result["strata"][key] = {
                "population": len(population),
                "sampled": count,
                "weight": weight,
            }
            chosen = deterministic_order(
                population, seed, key, lambda site: site["id"]
            )[:count]
            for site in chosen:
                result["sites"].append(
                    dict(
                        site,
                        bucket=bucket,
                        stratum=name,
                        weight=weight,
                        inclusion_probability=count / len(population),
                        verdicts={},
                        evidence="",
                        match_review=(
                            "pending"
                            if site.get("match_needs_review")
                            else "exact_or_unmatched"
                        ),
                    )
                )
    return result


def timing_sample(report, clang_count=100, other_count=200, seed=20261010):
    units, indexed = validate_report(report)
    common = [
        unit
        for unit in units
        if all(
            eligible_success(indexed[(v, unit["id"])])
            for v in ("baseline", "candidate")
        )
    ]
    chosen, counts, populations = [], {}, {}
    for name, count in [("clang", clang_count), ("other", other_count)]:
        population = [unit for unit in common if clang_unit(unit) == (name == "clang")]
        sampled = deterministic_order(
            population, seed, "timing/" + name, lambda unit: unit["id"]
        )[:count]
        chosen.extend(sampled)
        counts[name] = len(sampled)
        populations[name] = len(population)
    return {
        "schema_version": 1,
        "seed": seed,
        "units": chosen,
        "counts": counts,
        "populations": populations,
        "selection_basis": "all measured invocations successful and checker-eligible; independent of warning contents",
    }


def review_totals(sample):
    result = defaultdict(
        lambda: {"reviewed": 0, "weighted": Counter(), "unweighted": Counter()}
    )
    for site in sample["sites"]:
        if set(site.get("verdicts", {})) - set(site["claims"]):
            raise ValueError("verdict for a checker without a claim")
        if site.get("match_needs_review") and site.get("match_review") != "confirmed":
            raise ValueError(
                "confirm or repair provisional source-use matches before estimating review totals"
            )
        for variant in site["claims"]:
            verdict = site.get("verdicts", {}).get(variant) or "unresolved"
            if verdict not in VERDICTS:
                raise ValueError("unknown review verdict: " + verdict)
            result[variant]["reviewed"] += verdict != "unresolved"
            result[variant]["unweighted"][verdict] += 1
            result[variant]["weighted"][verdict] += site["weight"]
    return dict(result)


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", type=Path)
    parser.add_argument(
        "--reviewed-sample",
        type=Path,
        help="summarize manually completed verdicts instead of selecting samples",
    )
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--seed", type=int, default=20261010)
    parser.add_argument("--sites-per-bucket", type=int, default=100)
    parser.add_argument("--clang-timing-units", type=int, default=100)
    parser.add_argument("--other-timing-units", type=int, default=200)
    args = parser.parse_args()
    try:
        if bool(args.report) == bool(args.reviewed_sample):
            raise ValueError("provide exactly one of --report or --reviewed-sample")
        if (
            min(args.sites_per_bucket, args.clang_timing_units, args.other_timing_units)
            < 0
        ):
            raise ValueError("sample sizes must be nonnegative")
        args.output_dir.mkdir(parents=True, exist_ok=True)
        if any(args.output_dir.iterdir()):
            raise ValueError("output directory must be empty")
        source = args.report or args.reviewed_sample
        raw = source.read_bytes()
        data = json.loads(raw)
        provenance = {
            "input": str(source.resolve()),
            "sha256": hashlib.sha256(raw).hexdigest(),
            "tool_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            "invocation": sys.argv,
        }
        if args.reviewed_sample:
            write_json(args.output_dir / "review-totals.json", review_totals(data))
        else:
            sources = FrozenSources(data, args.report.parent)
            comparison = compare_report(data, sources)
            write_json(args.output_dir / "source-association-audit.json", sources.audit)
            write_json(args.output_dir / "comparison.json", comparison)
            write_json(
                args.output_dir / "review-sample.json",
                review_sample(comparison, args.sites_per_bucket, args.seed),
            )
            write_json(
                args.output_dir / "serial-selection.json",
                timing_sample(
                    data, args.clang_timing_units, args.other_timing_units, args.seed
                ),
            )
        write_json(args.output_dir / "provenance.json", provenance)
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(2, "error: " + str(error) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
