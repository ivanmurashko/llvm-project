```{title} Comparing CIR lifetime builds and use-after-move checks
```

# Comparing CIR lifetime builds and use-after-move checks

`clang-tidy/tool/benchmark-cir-lifetime.py` compares two checker builds or two
checks on frozen compilation commands. Its original defaults remain a serial
CIR-versus-CIR comparison: one worker, no warmups, three measured repetitions,
120 seconds per invocation, and the same explicit CIR options for both builds.
The following protocol also supports comparing `cir-lifetime-check` with default
`bugprone-use-after-move` in **the same clang-tidy executable**.

The runner needs Python 3.9 or later and Linux or macOS with `os.wait4`.
CIR requires a build with `CLANG_ENABLE_CIR=ON`. Mixed-check comparisons and
`--parse-yaml` additionally require **PyYAML** in the selected Python interpreter;
a missing dependency is a setup error. The parser uses `yaml.safe_load` and
records the package version. CIR-versus-CIR text capture remains standard-library
only. Keep `cir_lifetime_benchmark_support.py` beside the runner when copying it.
`analyze-cir-lifetime.py` reads the resulting JSON and does not run compilers.

## Prepare and freeze inputs

Build the checker revisions separately with matching compiler, CMake settings,
build type, and assertions. A minimal build enables
`LLVM_ENABLE_PROJECTS=clang;mlir;clang-tools-extra` and `CLANG_ENABLE_CIR=ON`;
build `clang`, `clang-tidy`, and `cir-opt`. For a comparison between checks, pass
one newly built clang-tidy executable as both `--baseline` and `--candidate`.
The runner probes registration of each selected check, records executable
hashes/version output, and records supplied full revisions and build settings.
Those settings are declarations; retain the build manifests that establish them.

Configure the corpus with `CMAKE_EXPORT_COMPILE_COMMANDS=ON` and disable or avoid
PCH. Prepare generated inputs, dependency headers, and any response files before
freezing the database. Run and record a syntax-only preflight with the matching
compiler, retaining original semantic flags and working directories. **Keep
failed preflight TUs in the analysis selection.** The runner does not generate
inputs or perform this preflight. Record the exact configured scope; a checkout
or `LLVM_ENABLE_PROJECTS=all` does not imply every repository file is represented.

`--source-root` is an alias for the original `--clang-source-root`. Default
selection keeps existing C/C++/Objective-C sources beneath that root, sorts by
relative path, and selects the lexicographically first normalized JSON command
per path. `--file-filter` and `--limit` retain their original behavior.

Use `--selection-file` for an already reviewed multi-database selection, including
generated sources outside the source root. It accepts a units array, `{"units":
[...]}`, or `{"selection":{"units":[...]}}`. Each unit contains a unique safe
`id`, unique `relative_file`, and a `command` with absolute `file` and `directory`
and original `arguments` or `command`. Additional fields such as `project`,
`origin`, `path_scope`, and duplicate-selection provenance are retained. Duplicate
canonical files are rejected. This option cannot be combined with a limit or
path filter. Create a separate frozen Clang selection by retaining units whose
`project` is `clang`; this includes generated Clang TUs when present.

The chosen commands are copied unchanged to the output compilation database.
The runner fingerprints both databases, selected source files, executables,
runner/helper, and each `--input-manifest`. A manifest contains a JSON array
(or `{"files":[...]}`) of absolute `path` and `sha256` records, optionally with
`size` and `symlink_target`. Supply manifests for generated inputs, resource and
standard-library headers, sysroots, response files, configuration files, and
other external dependencies. These files are checked before measurement rounds
and at completion. `--verify-source` additionally requires a clean tracked Git
checkout at `--source-revision`, including checks before rounds and at the end.
A changed input leaves an incomplete report and a nonzero runner exit. Snapshots
cover the documented files and tracked tree, not an automatically discovered
hermetic filesystem. Archive the inputs externally for later reproduction.

`CLANG_NO_DEFAULT_CONFIG=1` and explicit check configurations prevent default
Clang config files and repository `.clang-tidy` settings from choosing checks.
Other inherited environment and original command flags remain relevant; record
those externally without exposing credentials. Keep the host free of other
builds and scans during a timing experiment.

## Choose checks and run complete batches

Each variant has independent `--baseline-check` / `--candidate-check` and
`--baseline-check-option NAME=VALUE` / `--candidate-check-option NAME=VALUE`.
Allowed checks are `cir-lifetime-check` and `bugprone-use-after-move`.
The existing shared `--check-option` remains available when both checks match;
using it for mixed checks is rejected. CIR defaults are `HistoryList=all`,
`RemarksList=`, and `HistLimit=1`. Bugprone's two configurable function lists
are explicitly empty, matching its defaults. Do not set custom lists for the
default-check comparison. The runner never applies fixes.

Use the same header visibility for both variants: `--header-filter` defaults to
`.*`, and system headers require `--system-headers`. These options affect
reported diagnostics; they do not establish complete emitted-function coverage.

Bugprone is inactive before C++11 and in non-C++ source. Any run using that check
requires `--eligibility-driver` naming the matching Clang executable. Before
measurement, the runner translates each original command with `-###`, retaining
its original argv[0], `-x`, standard, target flags, and working directory. It
records the effective frontend command and uses a small frontend feature query
for the effective standard, including the driver's default. A launcher or
multiple frontend jobs that cannot be resolved produces **unknown eligibility**.
Inactive and unknown outcomes remain in full execution counts and are excluded
from the primary diagnostic and paired timing cohorts; exit zero is not a clean
UAM analysis for an inactive check.

For a full-corpus comparison, first run the frozen Clang selection, then the whole
configured repository selection. Each uses 16 workers, one warmup and three
measured rounds per check, and a 300-second invocation deadline. Each check runs
as a separate complete batch; order alternates between rounds. Replace the
paths and full 40-character SHA placeholders below. Use a new external output
directory for every experiment.

```bash
python3 clang-tools-extra/clang-tidy/tool/benchmark-cir-lifetime.py \
  --baseline /path/to/new/bin/clang-tidy \
  --candidate /path/to/new/bin/clang-tidy \
  --baseline-check bugprone-use-after-move \
  --candidate-check cir-lifetime-check \
  --baseline-revision NEW_CHECKER_FULL_SHA \
  --candidate-revision NEW_CHECKER_FULL_SHA \
  --baseline-build-type Release --candidate-build-type Release \
  --baseline-assertions on --candidate-assertions on \
  --source-root /path/to/llvm-project --source-revision CORPUS_FULL_SHA \
  --verify-source --eligibility-driver /path/to/new/bin/clang \
  --compilation-database /path/to/frozen/compile_commands.json \
  --selection-file /path/to/frozen/whole-selection.json \
  --input-manifest /path/to/frozen/generated-inputs.json \
  --input-manifest /path/to/frozen/resources-and-configs.json \
  --jobs 16 --execution-mode batches --warmups 1 --repetitions 3 \
  --timeout 300 --output-dir /path/to/artifacts/whole-comparison
```

Use the same command with the separate Clang selection and output directory for
the Clang comparison. To compare two CIR builds, pass their distinct
executables and revisions and omit the two check-selection arguments. Do not
relabel an earlier measured source revision after an implementation change.

## Review diagnostics and select serial timing TUs

Mixed-check runs parse the raw exported YAML into primary diagnostics, attached
notes, byte offsets, and ranges. Original YAML and stdout/stderr remain intact.
Missing, malformed, incomplete, or changing diagnostic exports exclude the TU
from diagnostic matching and remain visible as exclusions. Raw line counts
include repetitions; matching uses a single representative measured repetition
only after verifying the groups are identical across all measured repetitions.

Run the analyzer on a completed whole-corpus report:

```bash
python3 clang-tools-extra/clang-tidy/tool/analyze-cir-lifetime.py \
  --report /path/to/artifacts/whole-comparison/report.json \
  --output-dir /path/to/artifacts/whole-review --seed 20261010
```

`comparison.json` contains shared, CIR-only, and bugprone-only source-use
candidates on TUs where both checks were eligible and succeeded in every measured
round. Repeated header/template reports are deduplicated by canonical file and
byte offset per check, retaining every emitting TU, original diagnostic group,
and move-origin note. Exact byte-offset matches are preferred. Unique range
overlaps are provisional matches requiring review. Different point locations in
the same conservative source-statement neighborhood remain ambiguous, including
multi-variable expressions. Neighborhoods come only from fingerprinted source
bytes or tracked files at the recorded Git revision; their provenance is saved
in `source-association-audit.json`. If frozen source is unavailable, unmatched
points in the same file remain ambiguous. These associations never establish a
shared use from message text or line number alone. Unlocated diagnostics,
coverage-exclusive observations, ineligible-check observations, and other
lifetime warnings remain separate.

CIR UAM includes direct moved-from warnings and invalid-pointer warnings with
an attached move-cause note **in the same YAML group**. Invalid-pointer warnings
without such a note are cause-ambiguous: absent or truncated history does not
prove they are unrelated to a move. They and their potential bugprone matches
stay out of the exclusive sampling buckets pending review. Other lifetime
warnings are reported separately. Existing CIR policy warnings remain visible
and must be classified separately from demonstrated defects.

`review-sample.json` selects up to 100 sites from each of the three buckets with
seed `20261010`, aiming for 50 Clang-observed and 50 other sites per bucket.
A shared-header site is Clang-observed if any retained emitting TU is a Clang TU.
If one stratum is short, its unused quota goes to the other stratum of the same
bucket; a bucket with fewer than 100 sites is fully sampled. Every sampled site
records its population, inclusion probability, and inverse-probability weight.
All TU provenance survives this stratification.

Review each check's claim separately using the source, grouped notes, library
contract, and a reduced reproduction when needed. Fill `verdicts` with
`actual_defect`, `false_positive`, `policy_only`, `intentional_test`, or
`unresolved`, and record evidence. Verdicts start empty. A known moved-from state
alone does not prove an arbitrary use is invalid. Confirm provisional matches
with `match_review=confirmed`; incorrect matches require correcting the matching
record and recomputing the affected sampling population, not silently treating
them as shared. Preserve the original sample and review changes externally.

```bash
python3 clang-tools-extra/clang-tidy/tool/analyze-cir-lifetime.py \
  --reviewed-sample /path/to/reviewed-sample.json \
  --output-dir /path/to/artifacts/review-totals
```

The output reports weighted and unweighted category totals by checker. Empty
verdicts remain unresolved. Weighted results apply to the matched sampling
population, not failed or ineligible TUs, ambiguous matches, or all repository
bugs. Bugprone is a comparison check, not ground truth. Neither warning counts
nor this sample establish universal recall or accuracy.

`serial-selection.json` independently selects up to 100 Clang and 200 other TUs
from the common eligible, all-measured-success cohort, without inspecting any
warning contents. Run the same two checks on this selection with `--jobs 1
--execution-mode paired --warmups 1 --repetitions 3 --timeout 300` and a fresh
output directory; retain the same commands, inputs, executables, and options.
Short strata are reported rather than filled with warning-selected TUs.

## Measurements, outcomes, and validation

Per-process wall time surrounds the entire invocation using a monotonic clock;
it includes the small resource supervisor's startup and result reporting.
Each command runs as the child of a freshly exec'd Python supervisor, which
collects that exact child's `wait4` status and resource usage. This prevents the
benchmark process's accumulated memory from becoming every command's RSS floor.
The command retains the small supervisor image as a possible inherited floor;
peak RSS is not an exact post-exec-only or private-heap measurement. Linux KiB
and macOS bytes are normalized to bytes. The reported high-water mark is not an
aggregate of simultaneously resident descendants or concurrent workers.

The absolute monotonic deadline includes supervisor startup. Timeout kills the
command's process group while leaving the supervisor alive to reap the child
and report its resources. A separate bounded supervisor watchdog reports
`measurement_failure` when resource collection itself fails. Compiler crashes,
launch failures, and timeouts remain distinct, and failures never count as
successful measurements. stdout/stderr and diagnostic YAML are retained.
Warmup runs retain all raw outcomes but do not enter measured statistics.

Each variant summary also reports `max_wall_seconds_all_runs` and
`max_peak_rss_bytes_all_runs` across all measured invocations, including failures
and timeouts. Missing or invalid measurements are excluded; a maximum is null
when no valid value exists. These maxima expose resource costs outside the
common-success timing cohort and exclude warmups.

Separate batch records contain full elapsed batch time, order, completion count,
and statuses, including coverage failures. Batch duration includes scheduling
and result capture. Parallel per-process times and complete-batch throughput
cannot alone establish which analysis algorithm is faster. CIR runs additional
lifetime checks; label its cost as the full configured check, not pure UAM cost.

Only serial **paired** execution produces timing ratios, and only for TUs where
both checks were eligible and succeeded in all measured repetitions. Reports
include per-TU median wall time and median/maximum RSS, per-TU time/RSS ratios,
and the ratio of summed median wall times. Always present coverage and failure
counts alongside those ratios. This design does not remove effects from cache
state, host noise, or the selected workload.

Statuses retain the original meanings: `success`, `frontend_failure`,
`cir_failure`, `pass_failure`, `analysis_failure`, `tool_failure`,
`launch_failure`, `crash`, and `timeout`. Text-based stage classification is a
triage aid; raw logs are authoritative. A parsed error with exit zero is still a
failure. Inactive and unknown eligibility are independent of these statuses.
A complete campaign exits zero even if individual runs fail; setup/input errors
exit 2, and interruption exits 130 with an incomplete report.

`report.json`, per-run `run.json`, YAML, raw logs, eligibility evidence, input
fingerprints, small progress records, and analysis samples belong outside the
checkout. The runner verifies complete unique invocation accounting, retains
paired outcome counts, and records its configuration, host, and dependency
version. Keep independent audits and any manual classification evidence beside
these artifacts. No new full-corpus result is implied by the tooling tests.

The focused infrastructure tests cover original compatibility, failures,
timeouts/RSS, options, warmup/order/worker accounting, effective language,
provenance changes, YAML safety/grouping, matching, sampling, and manual totals:

```bash
/path/to/build/bin/llvm-lit -v \
  clang-tools-extra/test/clang-tidy/infrastructure/benchmark-cir-lifetime*.test
```

YAML-specific test cases skip when the test interpreter lacks PyYAML. Run them
with a PyYAML-enabled interpreter before claiming mixed-check validation.
