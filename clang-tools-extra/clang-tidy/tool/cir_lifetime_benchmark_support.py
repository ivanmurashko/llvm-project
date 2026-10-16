# Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Input and diagnostic handling shared by the CIR benchmark tools.

YAML parsing is optional and uses PyYAML's safe loader. Resource measurement uses
an internal, freshly exec'd supervisor; ordinary CIR-versus-CIR capture needs
only the standard library.
"""

import hashlib
import json
import math
import os
from pathlib import Path
import shlex
import subprocess
import signal
import socket
import sys
import time


def read_check_profile(directory, check, source, cwd):
    """Read one invocation's stock clang-tidy profile, never inventing zeroes."""
    paths = sorted(Path(directory).glob("*.json"))
    result = {"status": "missing", "files": [str(p) for p in paths]}
    if not paths:
        return result
    try:
        if len(paths) != 1:
            raise ValueError("expected exactly one clang-tidy profile")
        data = json.loads(paths[0].read_text())
        recorded = Path(data["file"])
        if not recorded.is_absolute():
            recorded = Path(cwd) / recorded
        if recorded.resolve() != Path(source).resolve():
            raise ValueError("profile source differs from invocation")
        times = {}
        for field in ("wall", "user", "sys"):
            value = data["profile"]["time.clang-tidy." + check + "." + field]
            if type(value) not in (int, float) or not math.isfinite(value) or value < 0:
                raise ValueError("invalid check timing: " + field)
            times[field + "_seconds"] = value
        result.update(status="parsed", **times)
    except (OSError, ValueError, KeyError, TypeError, OverflowError, RecursionError) as error:
        result.update(status="invalid", reason=str(error))
    return result


def read_tidy_phase_profile(directory, source, cwd):
    """Read frontend intervals; the TU callback interval contains check timers."""
    paths = sorted(Path(directory).glob("*.json"))
    result = {"status": "missing", "files": [str(p) for p in paths]}
    if not paths:
        return result
    try:
        if len(paths) != 1:
            raise ValueError("expected exactly one clang-tidy profile")
        data = json.loads(paths[0].read_text())
        recorded = Path(data["file"])
        if not recorded.is_absolute():
            recorded = Path(cwd) / recorded
        if recorded.resolve() != Path(source).resolve():
            raise ValueError("profile source differs from invocation")
        if type(data["phase_schema_version"]) is not int or data["phase_schema_version"] != 1:
            raise ValueError("unsupported clang-tidy phase schema")
        phases = data["phases"]
        names = {"consumer_setup", "frontend_before_tu", "tu_callbacks"}
        if not isinstance(phases, dict) or set(phases) != names:
            raise ValueError("missing clang-tidy phase intervals")
        parsed = {}
        for name in names:
            values = phases[name]
            if not isinstance(values, dict) or set(values) != {
                "wall_seconds", "user_seconds", "sys_seconds"
            }:
                raise ValueError("invalid clang-tidy phase: " + name)
            for value in values.values():
                if type(value) not in (int, float) or not math.isfinite(value) or value < 0:
                    raise ValueError("invalid clang-tidy phase duration: " + name)
            parsed[name] = values
        result.update(status="parsed", phases=parsed)
    except (OSError, ValueError, KeyError, TypeError, OverflowError, RecursionError) as error:
        result.update(status="invalid", reason=str(error))
    return result


def read_phase_profile(directory, check, source, cwd, stock_profiling=False):
    """Validate independent steady-clock boundaries for this exact invocation.

    Completion of the TU callback does not establish checker success. Frontend
    errors and language-filtered checks are explicitly incomplete measurements.
    """
    paths = sorted(Path(directory).glob("*.phases.json"))
    result = {"status": "missing", "files": [str(p) for p in paths]}
    if not paths:
        return result
    try:
        if len(paths) != 1:
            raise ValueError("expected exactly one independent phase profile")
        data = json.loads(paths[0].read_text())
        if type(data["schema_version"]) is not int or data["schema_version"] != 1:
            raise ValueError("unsupported independent phase schema")
        recorded = Path(data["file"])
        directory = Path(data["build_directory"])
        if not directory.is_absolute() or directory.resolve() != Path(cwd).resolve():
            raise ValueError("phase profile build directory differs from invocation")
        if not recorded.is_absolute():
            recorded = directory / recorded
        if recorded.resolve() != Path(source).resolve():
            raise ValueError("phase profile source differs from invocation")
        if data["stock_profiling"] is not stock_profiling:
            raise ValueError("phase profile stock-profiling mode differs")
        for key, minimum in (("pid", 1), ("sequence", 0)):
            if type(data[key]) is not int or data[key] < minimum:
                raise ValueError("invalid phase profile " + key)
        checks = data["checks"]
        if (not isinstance(checks, list)
                or not all(isinstance(c, str) for c in checks)
                or len(set(checks)) != len(checks)):
            raise ValueError("invalid instantiated check list")
        clock = data["clock"]
        if (clock["name"] != "std::chrono::steady_clock"
                or clock["is_steady"] is not True
                or clock["unit"] != "nanoseconds"
                or any(type(clock[k]) is not int or clock[k] <= 0
                       for k in ("period_num", "period_den"))):
            raise ValueError("invalid phase profile clock")
        reads = data["clock_reads"]
        names = ("consumer_entry", "consumer_ready", "tu_entry", "tu_exit")
        if type(reads) is not int or reads not in (2, 3, 4):
            raise ValueError("invalid phase clock-read count")
        boundaries = data["boundaries_ns"]
        if not isinstance(boundaries, dict) or set(boundaries) != set(names[:reads]):
            raise ValueError("missing or unexpected phase boundaries")
        values = [boundaries[name] for name in names[:reads]]
        if any(type(v) is not int or not -(2**63) <= v < 2**63 for v in values):
            raise ValueError("invalid nanosecond boundary")
        if values != sorted(values):
            raise ValueError("unordered phase boundaries")
        if type(data["complete"]) is not bool or data["complete"] != (reads == 4):
            raise ValueError("phase completion disagrees with boundaries")
        if reads >= 3 and type(data["frontend_error_before_tu"]) is not bool:
            raise ValueError("missing frontend error status")
        result.update(metadata=data)
        if reads != 4 or data.get("frontend_error_before_tu") or check not in checks:
            result.update(status="incomplete", reason=(
                "TU analysis did not complete" if reads != 4 else
                "frontend error before TU analysis" if data["frontend_error_before_tu"] else
                "requested check was not instantiated for this language"))
            return result
        result.update(status="parsed", phases={
            name: {"wall_seconds": (values[i + 1] - values[i]) / 1e9}
            for i, name in enumerate(("consumer_setup", "frontend_before_tu", "tu_callbacks"))})
    except (OSError, ValueError, KeyError, TypeError, OverflowError, RecursionError) as error:
        result.update(status="invalid", reason=str(error))
    return result


def read_cir_profile(path, source, cwd, allow_incomplete=False):
    """Extract wall durations from MLIR timers; their `user` is not OS CPU."""
    result = {"status": "missing", "file": str(path)}
    if not Path(path).exists():
        return result
    try:
        data = json.loads(Path(path).read_text())
        recorded = Path(data["file"])
        if not recorded.is_absolute():
            recorded = Path(cwd) / recorded
        if type(data["schema_version"]) is not int or data["schema_version"] != 1 or recorded.resolve() != Path(source).resolve():
            raise ValueError("CIR profile schema or source differs")
        if data["complete"] is not True:
            if allow_incomplete and data["complete"] is False:
                return dict(result, status="incomplete", reason="CIR profiling did not complete")
            raise ValueError("CIR profiling did not complete")
        threads = data["effective_threads"]
        requested = data["requested_threads"]
        if type(threads) is not int or threads < 1 or type(requested) is not int or requested < 0:
            raise ValueError("invalid CIR thread counts")
        if requested and threads > requested:
            raise ValueError("effective CIR threads exceed requested cap")
        stages = {}

        def visit(nodes):
            if not isinstance(nodes, list):
                raise ValueError("invalid CIR timer tree")
            for node in nodes:
                if not isinstance(node, dict):
                    raise ValueError("invalid CIR timer node")
                if not node:
                    continue  # MLIR's JSON tree ends children with an empty object.
                name = node["name"]
                key = next((key for token, key in (
                    ("CIR generation", "generation"),
                    ("CIR verification", "verification"),
                    ("CIRCanonicalize", "canonicalization"),
                    ("LifetimeCheck", "lifetime")) if token in name), None)
                if key:
                    value = node["wall"]["duration"]
                    if key in stages or type(value) not in (int, float) or not math.isfinite(value) or value < 0:
                        raise ValueError("invalid or duplicate CIR stage: " + key)
                    stages[key] = value
                visit(node.get("passes", []))

        visit(data["timing"])
        if set(stages) != {"generation", "verification", "canonicalization", "lifetime"}:
            raise ValueError("missing CIR stage timers")
        result.update(status="parsed", wall_seconds=stages,
                      requested_threads=requested, effective_threads=threads,
                      wall_resolution_seconds=0.0001)
    except (OSError, ValueError, KeyError, TypeError, OverflowError, RecursionError) as error:
        result.update(status="invalid", reason=str(error))
    return result


def file_hash(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def fingerprint(path):
    path = Path(path).absolute()
    return {
        "path": str(path),
        "size": path.stat().st_size,
        "sha256": file_hash(path),
        "symlink_target": os.readlink(path) if path.is_symlink() else None,
    }


def verify_fingerprints(records):
    for expected in records:
        path = Path(expected["path"])
        try:
            actual = fingerprint(path)
        except OSError as error:
            raise ValueError("input fingerprint mismatch: " + str(path)) from error
        if any(
            actual[key] != expected[key]
            for key in ("sha256", "size", "symlink_target")
            if key in expected
        ):
            raise ValueError("input fingerprint mismatch: " + str(path))


def read_input_manifest(path):
    data = json.loads(Path(path).read_text())
    if isinstance(data, dict):
        data = data.get("files")
    if not isinstance(data, list):
        raise ValueError(
            "input manifest must be an array of file fingerprints: " + str(path)
        )
    for record in data:
        if (
            not isinstance(record, dict)
            or not isinstance(record.get("path"), str)
            or not isinstance(record.get("sha256"), str)
        ):
            raise ValueError("invalid input fingerprint in " + str(path))
        if not Path(record["path"]).is_absolute():
            raise ValueError(
                "input fingerprint paths must be absolute: " + record["path"]
            )
    verify_fingerprints(data)
    return data


def frozen_selection(path):
    data = json.loads(Path(path).read_text())
    if isinstance(data, dict):
        data = data.get("selection", data)
        data = data.get("units") if isinstance(data, dict) else data
    if not isinstance(data, list) or not data:
        raise ValueError("selection file must contain a nonempty units array")
    ids, names, files = set(), set(), set()
    for unit in data:
        if not isinstance(unit, dict):
            raise ValueError("invalid frozen unit")
        if not all(
            isinstance(unit.get(key), str) and unit[key]
            for key in ("id", "relative_file")
        ):
            raise ValueError("frozen unit needs id and relative_file")
        if "/" in unit["id"] or unit["id"] in (".", ".."):
            raise ValueError("unsafe frozen unit id")
        command = unit.get("command", {})
        if not all(
            isinstance(command.get(key), str) and Path(command[key]).is_absolute()
            for key in ("file", "directory")
        ):
            raise ValueError("frozen commands need absolute file and directory paths")
        if (
            not Path(command["file"]).is_file()
            or not Path(command["directory"]).is_dir()
        ):
            raise ValueError(
                "missing source file or compilation directory: " + unit["relative_file"]
            )
        command_arguments(command)
        canonical = str(Path(command["file"]).resolve())
        if unit["id"] in ids or unit["relative_file"] in names or canonical in files:
            raise ValueError("duplicate frozen unit: " + unit["relative_file"])
        ids.add(unit["id"])
        names.add(unit["relative_file"])
        files.add(canonical)
    return data


def command_arguments(command):
    if "arguments" in command:
        args = command["arguments"]
        if (
            not isinstance(args, list)
            or not args
            or not all(isinstance(a, str) for a in args)
        ):
            raise ValueError("invalid compilation arguments")
        return args[:]
    text = command.get("command")
    if not isinstance(text, str) or not text:
        raise ValueError("missing compilation command")
    return shlex.split(text)


def require_yaml():
    try:
        import yaml
    except ImportError as error:
        raise ValueError(
            "diagnostic YAML parsing requires PyYAML; install it for this Python interpreter"
        ) from error
    return yaml


def parse_yaml_diagnostics(path, cwd, expected_warnings=0):
    """Preserve diagnostic-note grouping and byte offsets from Clang's export."""
    yaml = require_yaml()
    path = Path(path)
    if not path.exists():
        return {"status": "missing" if expected_warnings else "empty", "groups": []}
    try:
        data = yaml.safe_load(path.read_text())
        if data is None:
            data = {}
        if not isinstance(data, dict) or not isinstance(
            data.get("Diagnostics", []), list
        ):
            raise ValueError("expected a Diagnostics array")

        def message(item, directory):
            if not isinstance(item, dict) or not isinstance(item.get("Message"), str):
                raise ValueError("invalid diagnostic message")
            file = item.get("FilePath", "")
            if not isinstance(file, str):
                raise ValueError("invalid diagnostic path")
            if file:
                file = str(
                    (
                        Path(file) if Path(file).is_absolute() else directory / file
                    ).resolve()
                )
            offset = item.get("FileOffset")
            if offset is not None and (
                not isinstance(offset, int) or isinstance(offset, bool) or offset < 0
            ):
                raise ValueError("invalid diagnostic offset")
            ranges = []
            for span in item.get("Ranges", []):
                if not isinstance(span, dict):
                    raise ValueError("invalid diagnostic range")
                span_file = span.get("FilePath", "")
                begin, length = span.get("FileOffset"), span.get("Length")
                if not isinstance(span_file, str) or not all(
                    isinstance(n, int) and not isinstance(n, bool) and n >= 0
                    for n in (begin, length)
                ):
                    raise ValueError("invalid diagnostic range")
                if span_file:
                    span_file = str(
                        (
                            Path(span_file)
                            if Path(span_file).is_absolute()
                            else directory / span_file
                        ).resolve()
                    )
                ranges.append(
                    {"file": span_file or None, "offset": begin, "length": length}
                )
            return {
                "message": item["Message"],
                "file": file or None,
                "offset": offset,
                "ranges": ranges,
            }

        groups = []
        for diagnostic in data.get("Diagnostics", []):
            if not isinstance(diagnostic, dict) or not isinstance(
                diagnostic.get("DiagnosticName"), str
            ):
                raise ValueError("invalid diagnostic group")
            directory = Path(diagnostic.get("BuildDirectory") or cwd)
            if not directory.is_absolute():
                directory = Path(cwd) / directory
            notes = diagnostic.get("Notes", [])
            if not isinstance(notes, list):
                raise ValueError("invalid diagnostic notes")
            groups.append(
                {
                    "check": diagnostic["DiagnosticName"],
                    "level": str(diagnostic.get("Level", "Warning")).lower(),
                    "primary": message(diagnostic.get("DiagnosticMessage"), directory),
                    "notes": [message(note, directory) for note in notes],
                }
            )
        warnings = sum(group["level"] == "warning" for group in groups)
        return {
            "status": "incomplete" if warnings < expected_warnings else "parsed",
            "groups": groups,
            "sha256": file_hash(path),
        }
    except (OSError, UnicodeError, ValueError, TypeError, yaml.YAMLError) as error:
        return {"status": "invalid", "groups": [], "error": str(error)}


def effective_language(command, driver, output, environment, feature_cache):
    """Ask the selected driver to translate the real argv, honoring -x/@files.

    argv[0] is retained for clang++/target-prefix driver behavior; executable
    selects the explicitly frozen driver. No compilation job is executed.
    """
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    arguments = command_arguments(command)
    if Path(arguments[0]).name in ("ccache", "sccache", "env", "distcc"):
        return {
            "eligible": None,
            "reason": "compiler launcher requires an explicit unwrapped command",
        }
    argv = arguments[:]
    argv.insert(argv.index("--") if "--" in argv else len(argv), "-###")
    process = subprocess.run(
        argv,
        executable=str(driver),
        cwd=command["directory"],
        capture_output=True,
        env=environment,
        timeout=30,
    )
    (output / "driver.stdout").write_bytes(process.stdout)
    (output / "driver.stderr").write_bytes(process.stderr)
    result = {
        "driver": str(driver),
        "argv": argv,
        "cwd": command["directory"],
        "returncode": process.returncode,
        "eligible": None,
    }
    if process.returncode:
        return dict(result, reason="driver translation failed")
    jobs = []
    for line in process.stderr.decode(errors="replace").splitlines():
        try:
            tokens = shlex.split(line)
        except ValueError:
            continue
        if "-cc1" in tokens:
            jobs.append(tokens)
    if len(jobs) != 1:
        return dict(result, reason="expected exactly one frontend job")
    cc1 = jobs[0]
    language, standard = None, None
    for index, token in enumerate(cc1):
        if token == "-x" and index + 1 < len(cc1):
            language = cc1[index + 1]
        elif token.startswith("-std="):
            standard = token.split("=", 1)[1]
    result.update(cc1=cc1, language=language, standard=standard)
    if language not in (
        "c++",
        "objective-c++",
        "c++-header",
        "objective-c++-header",
        "c++-module",
    ):
        return dict(result, eligible=False, reason="not a C++ frontend job")
    # A tiny frontend feature query handles default and future standard names
    # without guessing from suffixes or parsing __cplusplus under MSVC modes.
    key = (language, standard)
    if key not in feature_cache:
        probe = [str(driver), "-cc1", "-E", "-P", "-x", language]
        if standard:
            probe.append("-std=" + standard)
        probe.append("-")
        source = "#if __has_feature(cxx_rvalue_references)\nCIR_BENCH_CXX11\n#else\nCIR_BENCH_PRE_CXX11\n#endif\n"
        queried = subprocess.run(
            probe,
            input=source,
            capture_output=True,
            text=True,
            env=environment,
            timeout=30,
        )
        text = queried.stdout.strip()
        eligible = (
            (text == "CIR_BENCH_CXX11")
            if queried.returncode == 0
            and text in ("CIR_BENCH_CXX11", "CIR_BENCH_PRE_CXX11")
            else None
        )
        feature_cache[key] = {
            "eligible": eligible,
            "argv": probe,
            "stdin": source,
            "stdout": queried.stdout,
            "stderr": queried.stderr,
            "returncode": queried.returncode,
        }
    result["feature_probe"] = feature_cache[key]
    result["eligible"] = feature_cache[key]["eligible"]
    result["reason"] = (
        "C++11 or later" if result["eligible"] else "pre-C++11 or failed feature query"
    )
    return result



# A freshly exec'd supervisor prevents a growing benchmark parent from becoming
# every command's inherited RSS floor. Its own small pre-exec image remains part
# of the compiler child's kernel lifetime accounting; this is not heap/PSS data.
_RESOURCE_SUPERVISOR_GRACE = 5.0


def _resource_clock():
    # Python 3.9's monotonic() on macOS has a process-local origin. Deadlines
    # cross an exec boundary, so use the kernel's process-shared clock directly.
    return time.clock_gettime(time.CLOCK_MONOTONIC)


def _resource_kill_group(pid):
    if pid is not None:
        try:
            os.killpg(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass


def _resource_send(connection, message):
    connection.sendall((json.dumps(message) + "\n").encode())


def _resource_supervisor(fd):
    # This process has freshly exec'd and has one thread. Only this supervisor
    # forks explicitly; the multithreaded benchmark uses Popen without a
    # preexec_fn. Publish the PID before waiting for exec, so even a stuck exec
    # setup has a known cleanup target.
    connection = socket.socket(fileno=fd)
    os.set_inheritable(fd, False)
    compiler_pid = None
    timed_out = False
    reaped = False

    def expire(signum, frame):
        nonlocal timed_out
        timed_out = True
        _resource_kill_group(compiler_pid)
        # setsid may not have completed yet. Killing the leader too closes that
        # startup race; once exec succeeds, the group kill covers descendants.
        if compiler_pid is not None:
            try:
                os.kill(compiler_pid, signal.SIGKILL)
            except ProcessLookupError:
                pass

    inherited_handlers = {sig: signal.getsignal(sig)
                          for sig in (signal.SIGALRM, signal.SIGTERM)}
    signal.signal(signal.SIGALRM, expire)
    signal.signal(signal.SIGTERM, expire)
    try:
        with connection.makefile("rb") as incoming:
            request = json.loads(incoming.readline())
        remaining = request["deadline"] - _resource_clock()
        if remaining <= 0:
            _resource_send(connection, {"kind": "result", "compiler_pid": None,
                "returncode": None, "timed_out": True, "rusage": None})
            return
        signal.setitimer(signal.ITIMER_REAL, remaining)
        # CLOEXEC is automatic for os.pipe(). An exec error is bounded and
        # returned separately from the checker's own nonzero exit status.
        error_read, error_write = os.pipe()
        start_read, start_write = os.pipe()
        try:
            compiler_pid = os.fork()
        except OSError as error:
            os.close(error_read)
            os.close(error_write)
            os.close(start_read)
            os.close(start_write)
            print(str(error), file=sys.stderr, flush=True)
            _resource_send(connection, {"kind": "result", "compiler_pid": None,
                "returncode": None, "timed_out": timed_out,
                "rusage": None, "launch_error": str(error)})
            return
        if compiler_pid == 0:
            connection.close()
            os.close(error_read)
            os.close(start_write)
            try:
                os.setsid()
                # Match Popen's restore_signals behavior and discard the
                # supervisor's timeout handlers before executing the command.
                for name in ("SIGPIPE", "SIGXFZ", "SIGXFSZ"):
                    if hasattr(signal, name):
                        signal.signal(getattr(signal, name), signal.SIG_DFL)
                for sig, disposition in inherited_handlers.items():
                    signal.signal(sig, disposition)
                # A dying supervisor must not leave an unknown executing child.
                # The benchmark acknowledges the published PID before release.
                token = os.read(start_read, 1)
                os.close(start_read)
                if token != b"G":
                    os._exit(125)
                os.chdir(request["cwd"])
                os.execvpe(request["command"][0], request["command"], request["environment"])
            except BaseException as error:
                os.write(error_write, str(error).encode(errors="replace")[:4096])
                os._exit(127)
        os.close(error_write)
        os.close(start_read)
        if timed_out:
            expire(None, None)
        _resource_send(connection, {"kind": "started", "compiler_pid": compiler_pid})
        with connection.makefile("rb") as incoming:
            acknowledgement = json.loads(incoming.readline())
        if acknowledgement != {"kind": "ack", "compiler_pid": compiler_pid}:
            raise ValueError("compiler start was not acknowledged")
        try:
            if not timed_out:
                os.write(start_write, b"G")
        finally:
            os.close(start_write)
        try:
            launch_error = os.read(error_read, 4096)
        finally:
            os.close(error_read)
        pid, status, usage = os.wait4(compiler_pid, 0)
        reaped = True
        signal.setitimer(signal.ITIMER_REAL, 0)
        message = {"kind": "result", "compiler_pid": pid,
            "returncode": os.waitstatus_to_exitcode(status), "timed_out": timed_out,
            "rusage": {name: getattr(usage, name) for name in (
                "ru_maxrss", "ru_utime", "ru_stime", "ru_minflt", "ru_majflt")}}
        if launch_error:
            message["launch_error"] = launch_error.decode(errors="replace")
            print(message["launch_error"], file=sys.stderr, flush=True)
        _resource_send(connection, message)
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        if compiler_pid and not reaped:
            expire(None, None)
            try:
                os.wait4(compiler_pid, 0)
            except ChildProcessError:
                pass
        connection.close()


def measure_process(command, cwd, environment, timeout, stdout_path, stderr_path):
    """Return compiler status/wall/RSS with bounded, exact-child supervision.

    Wall includes supervisor startup/reporting. RSS comes only from its compiler
    child's wait4 record, never from the large original parent's supervisor
    child. The residual inherited floor is the small fresh Python supervisor.
    """
    # Use the deadline clock for elapsed wall time as well. On Darwin,
    # perf_counter can exclude host suspension while CLOCK_MONOTONIC includes
    # it; mixing them can report a short wall time for an expired invocation.
    start = _resource_clock()
    deadline = start + timeout
    connection, child_connection = socket.socketpair()
    supervisor = None
    compiler_pid = None
    compiler_reaped = False
    try:
        with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr:
            try:
                supervisor = subprocess.Popen(
                    [sys.executable, "-I", "-S", str(Path(__file__).resolve()),
                     "--resource-supervisor", str(child_connection.fileno())],
                    stdout=stdout, stderr=stderr,
                    start_new_session=True, pass_fds=(child_connection.fileno(),))
                child_connection.close()
                hard_deadline = deadline + _RESOURCE_SUPERVISOR_GRACE
                connection.settimeout(max(0.001, hard_deadline - _resource_clock()))
                _resource_send(connection, {"command": command, "cwd": str(cwd),
                                            "deadline": deadline,
                                            "environment": environment})
                with connection.makefile("rb") as responses:
                    result = None
                    while result is None:
                        connection.settimeout(max(0.001, hard_deadline - _resource_clock()))
                        line = responses.readline(65537)
                        if not line or len(line) > 65536 or not line.endswith(b"\n"):
                            raise ValueError("missing or oversized supervisor result")
                        message = json.loads(line)
                        if message.get("kind") == "started":
                            if compiler_pid is not None:
                                raise ValueError("duplicate compiler start")
                            compiler_pid = message["compiler_pid"]
                            if type(compiler_pid) is not int or compiler_pid <= 0:
                                raise ValueError("invalid compiler PID")
                            _resource_send(connection, {"kind": "ack", "compiler_pid": compiler_pid})
                        elif message.get("kind") == "result":
                            if message.get("compiler_pid") != compiler_pid:
                                raise ValueError("compiler result PID differs")
                            if compiler_pid is not None:
                                usage = message.get("rusage")
                                if (type(message.get("returncode")) is not int or
                                        not isinstance(usage, dict) or
                                        type(usage.get("ru_maxrss")) is not int or
                                        usage["ru_maxrss"] < 0):
                                    raise ValueError("invalid compiler resource result")
                                # A validated result is sent only after wait4.
                                # Never signal this PID after it has been reaped.
                                compiler_reaped = True
                            result = message
                        else:
                            raise ValueError("invalid supervisor message")
                # A validated result already contains the compiler's final
                # status and resources. Supervisor teardown has its own bound:
                # an expired compiler deadline must not discard that result.
                supervisor.wait(timeout=_RESOURCE_SUPERVISOR_GRACE)
                if supervisor.returncode != 0:
                    raise ValueError("resource supervisor failed")
                elapsed = _resource_clock() - start
                if result.get("launch_error"):
                    return None, elapsed, None, "launch_failure"
                forced = "timeout" if result["timed_out"] else None
                usage = result["rusage"]
                if usage is None:
                    if forced != "timeout" or compiler_pid is not None:
                        raise ValueError("compiler resource result missing")
                    return None, elapsed, None, forced
                peak = int(usage["ru_maxrss"]) * (1024 if sys.platform.startswith("linux") else 1)
                return result["returncode"], elapsed, peak, forced
            except OSError as error:
                if supervisor is None:
                    stderr.write(str(error).encode())
                    return None, _resource_clock() - start, None, "launch_failure"
                stderr.write(("resource measurement failed: " + str(error) + "\n").encode())
                return None, _resource_clock() - start, None, "measurement_failure"
            except (ValueError, KeyError, TypeError, subprocess.TimeoutExpired) as error:
                stderr.write(("resource measurement failed: " + str(error) + "\n").encode())
                return None, _resource_clock() - start, None, "measurement_failure"
    finally:
        connection.close()
        child_connection.close()
        # Cleanup must not replace the measured outcome. In particular, Darwin
        # can return EPERM for the process group of a dead, unreaped supervisor.
        # Reap before signaling, and handle the exit race after poll as well.
        def cleanup_error(error):
            with stderr_path.open("ab") as stream:
                stream.write(("resource cleanup failed: " + str(error) + "\n").encode())

        if compiler_pid is not None and not compiler_reaped:
            for kill in (_resource_kill_group,
                         lambda pid: os.kill(pid, signal.SIGKILL)):
                try:
                    kill(compiler_pid)
                except ProcessLookupError:
                    pass
                except OSError as error:
                    cleanup_error(error)
        if supervisor is not None and supervisor.poll() is None:
            try:
                _resource_kill_group(supervisor.pid)
            except OSError as error:
                if supervisor.poll() is None:
                    cleanup_error(error)
                    try:
                        supervisor.kill()
                    except ProcessLookupError:
                        pass
                    except OSError as kill_error:
                        cleanup_error(kill_error)
            try:
                supervisor.wait(timeout=_RESOURCE_SUPERVISOR_GRACE)
            except subprocess.TimeoutExpired as error:
                cleanup_error(error)


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] != "--resource-supervisor":
        raise SystemExit("internal resource supervisor only")
    _resource_supervisor(int(sys.argv[2]))
