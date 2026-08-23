#!/usr/bin/env python3
"""Run reproducible memftprt captures across official OpenJDK releases.

The runner owns target cleanup, records the exact inputs for every run, and is
resumable: successful run directories are skipped and failed attempts are kept
as evidence rather than overwritten.
"""

# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import argparse
import csv
import datetime as dt
import hashlib
import json
import os
import pathlib
import platform
import random
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from typing import Dict, Iterable, List, Sequence, Tuple


SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
STUDY_DIR = SCRIPT_DIR.parent
REPO_DIR = STUDY_DIR.parent.parent
DEFAULT_CACHE = REPO_DIR / ".study-cache" / "jvm-footprint"
DEFAULT_RESULTS = REPO_DIR / ".study-results" / "jvm-footprint"
CONTROLLED_FLAGS = [
    "-Xms64m",
    "-Xmx256m",
    "-Xss1m",
    "-XX:+UseG1GC",
    "-XX:ParallelGCThreads=2",
    "-XX:ConcGCThreads=1",
    "-XX:CICompilerCount=2",
    "-Xshare:off",
]


class StudyInterrupted(Exception):
    """Raised by the signal handler so the active target can be cleaned up."""

    def __init__(self, signal_number: int) -> None:
        super().__init__("received %s" % signal.Signals(signal_number).name)
        self.signal_number = signal_number


def interrupt_on_signal(signal_number: int, _frame: object) -> None:
    raise StudyInterrupted(signal_number)


def write_json(path: pathlib.Path, document: object) -> None:
    """Atomically replace a JSON manifest in the same filesystem directory."""
    descriptor, temporary_name = tempfile.mkstemp(prefix=".%s." % path.name,
                                                   dir=str(path.parent))
    temporary = pathlib.Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            json.dump(document, stream, indent=2, sort_keys=True)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def parse_range_list(value: str, minimum: int, maximum: int) -> List[int]:
    result = set()
    for part in value.split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            start_text, end_text = part.split("-", 1)
            start, end = int(start_text), int(end_text)
            if start > end:
                raise ValueError("descending version ranges are not supported")
            result.update(range(start, end + 1))
        else:
            result.add(int(part))
    if not result or min(result) < minimum or max(result) > maximum:
        raise ValueError("versions must be within %d-%d" % (minimum, maximum))
    return sorted(result)


def parse_choices(value: str, allowed: Sequence[str], label: str) -> List[str]:
    result = []
    for item in value.split(","):
        item = item.strip()
        if item and item not in result:
            if item not in allowed:
                raise ValueError("unknown %s %r; choose from %s" %
                                 (label, item, ", ".join(allowed)))
            result.append(item)
    if not result:
        raise ValueError("at least one %s is required" % label)
    return result


def load_manifest(path: pathlib.Path) -> Dict[int, Dict[str, str]]:
    records: Dict[int, Dict[str, str]] = {}
    with path.open("r", encoding="utf-8", newline="") as stream:
        lines = (line for line in stream if not line.startswith("#"))
        reader = csv.DictReader(
            lines,
            fieldnames=["version", "build", "archive_name", "sha256", "url"],
            delimiter="\t",
        )
        for row in reader:
            version = int(row["version"])
            digest = row["sha256"]
            if len(digest) != 64 or any(char not in "0123456789abcdef" for char in digest):
                raise ValueError("invalid SHA-256 for OpenJDK %d" % version)
            records[version] = row
    return records


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def command_output(command: Sequence[str]) -> str:
    completed = subprocess.run(command, check=True, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True)
    return completed.stdout.strip()


def record_host_metadata(path: pathlib.Path, manifest: Dict[int, Dict[str, str]],
                         versions: Sequence[int], executable: pathlib.Path) -> None:
    if path.exists():
        return
    allowed_cpus = sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else []
    memory_kb = None
    with pathlib.Path("/proc/meminfo").open("r", encoding="ascii") as stream:
        for line in stream:
            if line.startswith("MemTotal:"):
                memory_kb = int(line.split()[1])
                break
    tracked_inputs = [
        STUDY_DIR / "jdk-manifest.tsv",
        STUDY_DIR / "src" / "StudySupport.java",
        STUDY_DIR / "src" / "HelloIdle.java",
        STUDY_DIR / "src" / "RuntimeExercise.java",
        pathlib.Path(__file__).resolve(),
        executable,
    ]
    metadata = {
        "schema_version": 1,
        "created_at": dt.datetime.now(dt.timezone.utc).isoformat(),
        "host": {
            "platform": platform.platform(),
            "uname": list(platform.uname()),
            "logical_cpus": os.cpu_count(),
            "allowed_cpus": allowed_cpus,
            "memory_kb": memory_kb,
        },
        "repository": {
            "commit": command_output(["git", "-C", str(REPO_DIR), "rev-parse", "HEAD"]),
            "status": command_output(["git", "-C", str(REPO_DIR), "status", "--short"]),
        },
        "input_sha256": {str(item.relative_to(REPO_DIR)): sha256_file(item)
                         for item in tracked_inputs},
        "openjdk": [manifest[version] for version in versions],
    }
    write_json(path, metadata)


def java_version(jdk: pathlib.Path) -> str:
    return command_output([str(jdk / "bin" / "java"), "-version"])


def validate_installations(cache: pathlib.Path, versions: Sequence[int]) -> Dict[int, pathlib.Path]:
    jdks: Dict[int, pathlib.Path] = {}
    for version in versions:
        jdk = cache / "jdks" / str(version)
        for tool in ("java", "jcmd"):
            if not os.access(jdk / "bin" / tool, os.X_OK):
                raise FileNotFoundError(
                    "OpenJDK %d is missing bin/%s; run fetch-openjdk.sh first" %
                    (version, tool)
                )
        jdks[version] = jdk
    return jdks


def compile_workloads(cache: pathlib.Path) -> pathlib.Path:
    script = SCRIPT_DIR / "compile-workloads.sh"
    environment = os.environ.copy()
    environment["JVM_STUDY_CACHE"] = str(cache)
    subprocess.run([str(script)], cwd=REPO_DIR, env=environment, check=True)
    return cache / "workloads" / "classes"


def target_is_expected(pid: int, workload_class: str, pid_file: pathlib.Path) -> bool:
    """Reject PID reuse or another concurrent study before sending a signal."""
    try:
        command = (pathlib.Path("/proc") / str(pid) / "cmdline").read_bytes()
    except OSError:
        return False
    arguments = command.split(b"\0")
    expected_pid_file = ("-Dmemftprt.study.pidFile=" + str(pid_file)).encode("utf-8")
    return (workload_class.encode("ascii") in arguments and
            expected_pid_file in arguments)


def stop_target(pid_file: pathlib.Path, workload_class: str) -> Dict[str, object]:
    cleanup: Dict[str, object] = {"pid_file": str(pid_file), "pid": None,
                                  "signal": None, "exited": True}
    if not pid_file.exists():
        cleanup["note"] = "pid_file_missing"
        return cleanup
    try:
        pid = int(pid_file.read_text(encoding="ascii").strip())
    except (OSError, ValueError):
        cleanup["note"] = "pid_file_invalid"
        cleanup["exited"] = False
        return cleanup
    cleanup["pid"] = pid
    if not target_is_expected(pid, workload_class, pid_file):
        cleanup["note"] = "target_already_exited_or_identity_mismatch"
        return cleanup

    try:
        os.kill(pid, signal.SIGTERM)
    except ProcessLookupError:
        cleanup["note"] = "target_exited_before_signal"
        return cleanup
    except PermissionError:
        cleanup["note"] = "target_signal_permission_denied"
        cleanup["exited"] = False
        return cleanup
    cleanup["signal"] = "SIGTERM"
    for _ in range(100):
        if not target_is_expected(pid, workload_class, pid_file):
            return cleanup
        time.sleep(0.05)
    if target_is_expected(pid, workload_class, pid_file):
        try:
            os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            return cleanup
        except PermissionError:
            cleanup["note"] = "target_kill_permission_denied"
            cleanup["exited"] = False
            return cleanup
        cleanup["signal"] = "SIGKILL"
        for _ in range(100):
            if not target_is_expected(pid, workload_class, pid_file):
                return cleanup
            time.sleep(0.05)
    cleanup["exited"] = not target_is_expected(pid, workload_class, pid_file)
    return cleanup


def attempt_is_complete(directory: pathlib.Path) -> bool:
    """Validate status and retire an abandoned target before scheduling a retry."""
    run_file = directory / "run.json"
    if not run_file.exists():
        return False
    try:
        metadata = json.loads(run_file.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return False
    if metadata.get("status") == "success":
        try:
            if not metadata.get("cleanup", {}).get("exited"):
                raise RuntimeError("recorded target cleanup did not complete")
            validate_capture(directory, str(metadata["nmt"]))
            return True
        except (KeyError, OSError, RuntimeError, TypeError, ValueError) as error:
            metadata.update({
                "status": "invalidated",
                "invalidated_at": dt.datetime.now(dt.timezone.utc).isoformat(),
                "error": "resume validation failed: %s" % error,
            })
            write_json(run_file, metadata)
            print("INVALID", directory, metadata["error"], file=sys.stderr, flush=True)
            return False
    if metadata.get("status") != "running":
        return False

    workload_class = metadata.get("workload_class")
    if workload_class not in ("HelloIdle", "RuntimeExercise"):
        raise RuntimeError("stale attempt has an unknown workload class: %s" % run_file)
    cleanup = stop_target(directory / "target.pid", str(workload_class))
    metadata.update({
        "finished_at": dt.datetime.now(dt.timezone.utc).isoformat(),
        "status": "abandoned" if cleanup.get("exited") else "failed",
        "error": "previous runner did not finalize this attempt",
        "cleanup": cleanup,
    })
    write_json(run_file, metadata)
    if not cleanup.get("exited"):
        raise RuntimeError("could not clean the stale target recorded by %s" % run_file)
    print("RECOVER", directory, "retired stale attempt", flush=True)
    return False


def next_attempt_directory(base: pathlib.Path) -> Tuple[pathlib.Path, bool]:
    if not base.exists():
        return base, False
    if attempt_is_complete(base):
        return base, True
    attempt = 2
    while True:
        candidate = base.with_name(base.name + "-retry-%02d" % attempt)
        if not candidate.exists():
            return candidate, False
        if attempt_is_complete(candidate):
            return candidate, True
        attempt += 1


def find_one(directory: pathlib.Path, pattern: str) -> pathlib.Path:
    matches = list(directory.glob(pattern))
    if len(matches) != 1:
        raise RuntimeError("expected one %s in %s, found %d" %
                           (pattern, directory, len(matches)))
    return matches[0]


def validate_capture(run_dir: pathlib.Path, nmt: str) -> Dict[str, str]:
    report = find_one(run_dir, "memory_report_*.json")
    complete = find_one(run_dir, "capture_complete_*.tsv")
    document = json.loads(report.read_text(encoding="utf-8"))
    if document.get("schema_version") != 1:
        raise RuntimeError("unexpected memftprt JSON schema")
    if document.get("capabilities", {}).get("mapping_detail") != "captured":
        raise RuntimeError("mapping capture was not completed")
    artifacts = {"report_json": report.name, "completion_marker": complete.name}
    if nmt == "on":
        nmt_tsv = find_one(run_dir, "jvm_nmt_*.tsv")
        nmt_raw = find_one(run_dir, "jvm_nmt_*.txt")
        metadata_rows = find_one(run_dir, "capture_metadata_*.tsv").read_text(
            encoding="utf-8"
        ).splitlines()
        if not any(row.startswith("jvm_nmt\t") and row.endswith("\tcaptured")
                   for row in metadata_rows):
            raise RuntimeError("JVM NMT was not reported as captured")
        artifacts.update({"nmt_tsv": nmt_tsv.name, "nmt_raw": nmt_raw.name})
    return artifacts


def run_job(job: Dict[str, object], args: argparse.Namespace, jdk: pathlib.Path,
            classes: pathlib.Path, executable: pathlib.Path) -> bool:
    version = int(job["version"])
    workload = str(job["workload"])
    profile = str(job["profile"])
    phase = str(job["phase"])
    repetition = int(job["repetition"])
    base = (args.results / "runs" / ("openjdk-%d" % version) / profile / workload /
            ("%s-%02d" % (phase, repetition)))
    run_dir, already_complete = next_attempt_directory(base)
    if already_complete:
        print("SKIP", base.relative_to(args.results), "already successful", flush=True)
        return True

    run_dir.mkdir(parents=True)
    pid_file = run_dir / "target.pid"
    workload_class = "HelloIdle" if workload == "hello" else "RuntimeExercise"
    java_command = [str(jdk / "bin" / "java")]
    if args.nmt == "on":
        # Early HotSpot releases must see NMT in the native java launcher before
        # class-path processing. JAVA_TOOL_OPTIONS is too late for OpenJDK 8-17.
        java_command.append("-XX:NativeMemoryTracking=summary")
    if profile == "controlled":
        java_command.extend(CONTROLLED_FLAGS)
    java_command.extend([
        "-Dmemftprt.study.pidFile=" + str(pid_file),
        "-cp", str(classes), workload_class,
    ])
    if workload == "exercise":
        java_command.append(str(args.exercise_seconds))

    command = [str(executable), "--freeze", "signals"]
    if args.nmt == "off":
        command.append("--no-jcmd")
    command.extend([
        "--mappings", "--format", "all", "--stable-window", str(args.stable_window),
        "--timeout", str(args.exercise_seconds + args.quiescence_margin),
        "--output-dir", str(run_dir), "--",
    ])
    command.extend(java_command)
    affinity: List[int] = []
    if profile == "controlled" and shutil.which("taskset") and hasattr(os, "sched_getaffinity"):
        affinity = sorted(os.sched_getaffinity(0))[:2]
        if len(affinity) == 2:
            command = ["taskset", "-c", ",".join(str(cpu) for cpu in affinity)] + command

    environment = os.environ.copy()
    for name in ("JAVA_TOOL_OPTIONS", "JDK_JAVA_OPTIONS", "_JAVA_OPTIONS", "CLASSPATH"):
        environment.pop(name, None)
    environment.update({
        "JAVA_HOME": str(jdk),
        "PATH": str(jdk / "bin") + os.pathsep + environment.get("PATH", ""),
        "LC_ALL": "C",
        "LANG": "C",
        "TZ": "UTC",
    })

    metadata: Dict[str, object] = {
        "schema_version": 1,
        "status": "running",
        "version": version,
        "java_version": java_version(jdk),
        "profile": profile,
        "profile_flags": CONTROLLED_FLAGS if profile == "controlled" else [],
        "cpu_affinity": affinity,
        "workload": workload,
        "workload_class": workload_class,
        "exercise_seconds": args.exercise_seconds if workload == "exercise" else None,
        "nmt": args.nmt,
        "phase": phase,
        "repetition": repetition,
        "command": command,
        "started_at": dt.datetime.now(dt.timezone.utc).isoformat(),
    }
    run_file = run_dir / "run.json"
    write_json(run_file, metadata)
    print("RUN ", run_dir.relative_to(args.results), flush=True)

    return_code = None
    error = None
    interrupted = None
    started = time.monotonic()
    with (run_dir / "collector.log").open("w", encoding="utf-8") as log:
        try:
            completed = subprocess.run(command, cwd=REPO_DIR, env=environment,
                                       stdout=log, stderr=subprocess.STDOUT,
                                       timeout=args.exercise_seconds + args.quiescence_margin + 90)
            return_code = completed.returncode
        except subprocess.TimeoutExpired as caught:
            error = "runner_timeout: %s" % caught
        except OSError as caught:
            error = "launch_failed: %s" % caught
        except StudyInterrupted as caught:
            error = "runner_interrupted: %s" % caught
            interrupted = caught

    cleanup = stop_target(pid_file, workload_class)
    metadata.update({
        "finished_at": dt.datetime.now(dt.timezone.utc).isoformat(),
        "elapsed_seconds": round(time.monotonic() - started, 6),
        "return_code": return_code,
        "cleanup": cleanup,
    })
    try:
        if error is not None:
            raise RuntimeError(error)
        if return_code != 0:
            raise RuntimeError("memftprt exited with status %s" % return_code)
        if not cleanup.get("exited"):
            raise RuntimeError("target cleanup did not complete")
        metadata["artifacts"] = validate_capture(run_dir, args.nmt)
        metadata["status"] = "success"
    except Exception as caught:  # preserve the failed run as evidence
        metadata["status"] = "failed"
        metadata["error"] = str(caught)
    write_json(run_file, metadata)
    if interrupted is not None:
        raise interrupted
    if metadata["status"] != "success":
        print("FAIL", run_dir.relative_to(args.results), metadata["error"], file=sys.stderr,
              flush=True)
        return False
    return True


def build_schedule(versions: Iterable[int], profiles: Iterable[str], workloads: Iterable[str],
                   repetitions: int, warmups: int, seed: int) -> List[Dict[str, object]]:
    warmup_jobs = [
        {"version": version, "profile": profile, "workload": workload,
         "phase": "warmup", "repetition": repetition}
        for version in versions for profile in profiles for workload in workloads
        for repetition in range(1, warmups + 1)
    ]
    measured_jobs = [
        {"version": version, "profile": profile, "workload": workload,
         "phase": "measure", "repetition": repetition}
        for version in versions for profile in profiles for workload in workloads
        for repetition in range(1, repetitions + 1)
    ]
    generator = random.Random(seed)
    generator.shuffle(warmup_jobs)
    generator.shuffle(measured_jobs)
    return warmup_jobs + measured_jobs


def argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--versions", default="8-25", help="versions/ranges, default: 8-25")
    parser.add_argument("--profiles", default="controlled",
                        help="controlled, default, or both comma-separated")
    parser.add_argument("--workloads", default="hello,exercise",
                        help="hello, exercise, or both comma-separated")
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--warmups", type=int, default=0)
    parser.add_argument("--exercise-seconds", type=int, default=150)
    parser.add_argument("--stable-window", type=int, default=5)
    parser.add_argument("--quiescence-margin", type=int, default=90)
    parser.add_argument("--nmt", choices=("on", "off"), default="on")
    parser.add_argument("--seed", type=int, default=20260823)
    parser.add_argument("--cache", type=pathlib.Path, default=DEFAULT_CACHE)
    parser.add_argument("--results", type=pathlib.Path, default=DEFAULT_RESULTS)
    parser.add_argument("--memftprt", type=pathlib.Path, default=REPO_DIR / "build" / "memftprt")
    parser.add_argument("--continue-on-error", action="store_true")
    return parser


def main() -> int:
    parser = argument_parser()
    args = parser.parse_args()
    for signal_number in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP, signal.SIGQUIT):
        signal.signal(signal_number, interrupt_on_signal)
    try:
        versions = parse_range_list(args.versions, 8, 25)
        profiles = parse_choices(args.profiles, ("controlled", "default"), "profile")
        workloads = parse_choices(args.workloads, ("hello", "exercise"), "workload")
        if not 1 <= args.repetitions <= 20:
            raise ValueError("repetitions must be between 1 and 20")
        if not 0 <= args.warmups <= 5:
            raise ValueError("warmups must be between 0 and 5")
        if not 1 <= args.exercise_seconds <= 900:
            raise ValueError("exercise-seconds must be between 1 and 900")
        args.cache = args.cache.resolve()
        args.results = args.results.resolve()
        executable = args.memftprt.resolve()
        if not os.access(executable, os.X_OK):
            raise FileNotFoundError("memftprt is not executable: %s" % executable)
        manifest = load_manifest(STUDY_DIR / "jdk-manifest.tsv")
        missing = [version for version in versions if version not in manifest]
        if missing:
            raise ValueError("manifest lacks OpenJDK versions: %s" % missing)
        jdks = validate_installations(args.cache, versions)
        classes = compile_workloads(args.cache)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.error(str(error))

    args.results.mkdir(parents=True, exist_ok=True)
    record_host_metadata(args.results / "study.json", manifest, versions, executable)
    schedule = build_schedule(versions, profiles, workloads, args.repetitions,
                              args.warmups, args.seed)
    write_json(args.results / "schedule.json", schedule)
    print("Study contains %d jobs; seed=%d" % (len(schedule), args.seed), flush=True)
    failures = 0
    try:
        for job in schedule:
            if not run_job(job, args, jdks[int(job["version"])], classes, executable):
                failures += 1
                if not args.continue_on_error:
                    break
    except StudyInterrupted as interrupted:
        print("Study interrupted by %s; active target cleanup was attempted" %
              signal.Signals(interrupted.signal_number).name, file=sys.stderr, flush=True)
        return 128 + interrupted.signal_number
    print("Study finished: %d failed job(s)" % failures, flush=True)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
