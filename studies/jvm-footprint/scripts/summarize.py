#!/usr/bin/env python3
"""Validate and summarize one or more JVM footprint result directories."""

# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import pathlib
import re
import statistics
import sys
from collections import defaultdict
from typing import Dict, Iterable, List, Mapping, Optional, Sequence, Tuple


READY_PATTERN = re.compile(
    r"MEMFTPRT_STUDY_READY workload=(?P<workload>\w+)(?P<details>[^\r\n]*)"
)
DETAIL_PATTERN = re.compile(r"(\w+)=([^ ]+)")
MEMORY_FIELDS = ("vsize", "rss", "pss", "uss", "pss_anon", "pss_file", "pss_shmem")


def read_nmt(path: Optional[pathlib.Path]) -> Dict[str, Dict[str, int]]:
    if path is None:
        return {}
    categories: Dict[str, Dict[str, int]] = {}
    with path.open("r", encoding="utf-8", newline="") as stream:
        for row in csv.DictReader(stream, delimiter="\t"):
            categories[row["category"]] = {
                "reserved_kb": int(row["reserved_kb"]),
                "committed_kb": int(row["committed_kb"]),
            }
    return categories


def ready_details(path: pathlib.Path) -> Dict[str, str]:
    match = READY_PATTERN.search(path.read_text(encoding="utf-8", errors="replace"))
    if not match:
        raise ValueError("workload readiness marker is missing from %s" % path)
    details = {"ready_workload": match.group("workload")}
    details.update(dict(DETAIL_PATTERN.findall(match.group("details"))))
    return details


def total_nmt(categories: Mapping[str, Mapping[str, int]]) -> Tuple[Optional[int], Optional[int]]:
    for name, values in categories.items():
        if name.strip().lower() == "total":
            return values["reserved_kb"], values["committed_kb"]
    return None, None


def load_run(run_file: pathlib.Path) -> Dict[str, object]:
    metadata = json.loads(run_file.read_text(encoding="utf-8"))
    if metadata.get("status") != "success" or metadata.get("phase") != "measure":
        return {}
    run_dir = run_file.parent
    artifacts = metadata.get("artifacts", {})
    report_path = run_dir / str(artifacts["report_json"])
    report = json.loads(report_path.read_text(encoding="utf-8"))
    if report.get("schema_version") != 1:
        raise ValueError("unsupported report schema in %s" % report_path)
    memory = report["total"]["memory_kb"]
    nmt_path = run_dir / str(artifacts["nmt_tsv"]) if "nmt_tsv" in artifacts else None
    nmt = read_nmt(nmt_path)
    nmt_reserved, nmt_committed = total_nmt(nmt)
    details = ready_details(run_dir / "collector.log")
    workload = str(metadata["workload"])
    if details["ready_workload"] != workload:
        raise ValueError("workload readiness marker does not match metadata in %s" % run_dir)
    operations = int(details["operations"]) if "operations" in details else None
    duration_ms = int(details["duration_ms"]) if "duration_ms" in details else None
    if workload == "exercise" and (not operations or not duration_ms):
        raise ValueError("exercise completion evidence is incomplete in %s" % run_dir)
    if metadata["nmt"] == "on" and nmt_committed is None:
        raise ValueError("NMT total is missing from %s" % run_dir)
    row: Dict[str, object] = {
        "version": int(metadata["version"]),
        "profile": metadata["profile"],
        "workload": workload,
        "nmt": metadata["nmt"],
        "repetition": int(metadata["repetition"]),
        "elapsed_seconds": metadata["elapsed_seconds"],
        "java_version": str(metadata["java_version"]).replace("\n", " | "),
        "run_dir": str(run_dir),
        "nmt_reserved_kb": nmt_reserved,
        "nmt_committed_kb": nmt_committed,
        "operations": operations,
        "operations_per_second": (operations * 1000.0 / duration_ms)
        if operations is not None and duration_ms is not None else None,
        "workload_duration_ms": duration_ms,
        "workers": int(details["workers"]) if "workers" in details else None,
        "checksum": details.get("checksum"),
    }
    for field in MEMORY_FIELDS:
        row[field + "_kb"] = memory.get(field)
    for category, values in nmt.items():
        safe = re.sub(r"[^a-z0-9]+", "_", category.lower()).strip("_")
        row["nmt_%s_reserved_kb" % safe] = values["reserved_kb"]
        row["nmt_%s_committed_kb" % safe] = values["committed_kb"]
    return row


def discover(results: Sequence[pathlib.Path]) -> Tuple[List[Dict[str, object]], List[str]]:
    rows: List[Dict[str, object]] = []
    errors: List[str] = []
    for root in results:
        for run_file in sorted((root / "runs").glob("**/run.json")):
            try:
                row = load_run(run_file)
                if row:
                    rows.append(row)
            except (KeyError, OSError, TypeError, ValueError) as error:
                errors.append("%s: %s" % (run_file, error))
    logical_runs: Dict[Tuple[object, ...], List[str]] = defaultdict(list)
    for row in rows:
        key = (row["version"], row["profile"], row["workload"], row["nmt"],
               row["repetition"])
        logical_runs[key].append(str(row["run_dir"]))
    for key, directories in logical_runs.items():
        if len(directories) > 1:
            errors.append("duplicate successful logical run %r: %s" %
                          (key, ", ".join(directories)))
    return rows, errors


def median(values: Iterable[object]) -> Optional[float]:
    numbers = [float(value) for value in values if value is not None]
    return statistics.median(numbers) if numbers else None


def mad(values: Iterable[object]) -> Optional[float]:
    numbers = [float(value) for value in values if value is not None]
    if not numbers:
        return None
    center = statistics.median(numbers)
    return statistics.median(abs(value - center) for value in numbers)


def summarize(rows: Sequence[Mapping[str, object]]) -> List[Dict[str, object]]:
    groups: Dict[Tuple[object, ...], List[Mapping[str, object]]] = defaultdict(list)
    for row in rows:
        key = (row["version"], row["profile"], row["workload"], row["nmt"])
        groups[key].append(row)
    output: List[Dict[str, object]] = []
    metrics = [field + "_kb" for field in MEMORY_FIELDS] + [
        "nmt_reserved_kb", "nmt_committed_kb", "operations",
        "operations_per_second", "workload_duration_ms"
    ]
    metrics.extend(sorted({field for row in rows for field in row
                           if field.startswith("nmt_") and field.endswith("_kb")
                           and field not in metrics}))
    for key, group in sorted(groups.items()):
        item: Dict[str, object] = {
            "version": key[0], "profile": key[1], "workload": key[2], "nmt": key[3],
            "runs": len(group),
        }
        for metric in metrics:
            values = [row.get(metric) for row in group]
            present = [float(value) for value in values if value is not None]
            item[metric + "_median"] = median(values)
            item[metric + "_mad"] = mad(values)
            item[metric + "_min"] = min(present) if present else None
            item[metric + "_max"] = max(present) if present else None
        output.append(item)
    return output


def write_csv(path: pathlib.Path, rows: Sequence[Mapping[str, object]]) -> None:
    fields = sorted({field for row in rows for field in row})
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)


def publication_rows(summary: Sequence[Mapping[str, object]]) -> List[Dict[str, object]]:
    """Build a compact table suitable for a report or static documentation page."""
    indexed = {(row["profile"], row["version"], row["workload"]): row
               for row in summary}
    output: List[Dict[str, object]] = []
    profiles = sorted({str(row["profile"]) for row in summary})
    versions = sorted({int(row["version"]) for row in summary})

    def mib(row: Mapping[str, object], field: str) -> Optional[float]:
        value = row.get(field)
        return round(float(value) / 1024.0, 3) if value is not None else None

    for profile in profiles:
        for version in versions:
            hello = indexed.get((profile, version, "hello"))
            exercise = indexed.get((profile, version, "exercise"))
            if hello is None or exercise is None:
                continue
            hello_pss = mib(hello, "pss_kb_median")
            exercise_pss = mib(exercise, "pss_kb_median")
            output.append({
                "version": version,
                "profile": profile,
                "runs_per_workload": min(int(hello["runs"]), int(exercise["runs"])),
                "hello_pss_mib": hello_pss,
                "hello_pss_mad_mib": mib(hello, "pss_kb_mad"),
                "exercise_pss_mib": exercise_pss,
                "exercise_pss_mad_mib": mib(exercise, "pss_kb_mad"),
                "exercise_minus_hello_pss_mib": round(exercise_pss - hello_pss, 3)
                if hello_pss is not None and exercise_pss is not None else None,
                "exercise_heap_committed_mib": mib(
                    exercise, "nmt_java_heap_committed_kb_median"
                ),
                "exercise_nmt_committed_mib": mib(exercise, "nmt_committed_kb_median"),
                "exercise_operations_per_second": round(
                    float(exercise["operations_per_second_median"]), 3
                ) if exercise.get("operations_per_second_median") is not None else None,
            })
    return output


def publication_provenance(results: Sequence[pathlib.Path],
                           rows: Sequence[Mapping[str, object]]) -> Dict[str, object]:
    """Retain the load-bearing host and input identity without raw local paths."""
    studies: List[Dict[str, object]] = []
    for root in results:
        study_file = root / "study.json"
        if not study_file.exists():
            continue
        source = json.loads(study_file.read_text(encoding="utf-8"))
        studies.append({
            "created_at": source.get("created_at"),
            "host": source.get("host"),
            "input_sha256": source.get("input_sha256"),
            "openjdk": source.get("openjdk"),
        })
    digest = hashlib.sha256(pathlib.Path(__file__).read_bytes()).hexdigest()
    return {
        "schema_version": 1,
        "successful_captures": len(rows),
        "versions": sorted({int(row["version"]) for row in rows}),
        "profiles": sorted({str(row["profile"]) for row in rows}),
        "workloads": sorted({str(row["workload"]) for row in rows}),
        "nmt_modes": sorted({str(row["nmt"]) for row in rows}),
        "summarizer_sha256": digest,
        "studies": studies,
    }


def publication_analysis(rows: Sequence[Mapping[str, object]]) -> Dict[str, object]:
    """Compute the cross-run relationships cited by the published study."""
    exercise = [row for row in rows if row["workload"] == "exercise"]
    paired = [row for row in exercise
              if row.get("pss_kb") is not None
              and row.get("nmt_java_heap_committed_kb") is not None]
    pss = [float(row["pss_kb"]) for row in paired]
    heap = [float(row["nmt_java_heap_committed_kb"]) for row in paired]
    residual_mib = [(left - right) / 1024.0 for left, right in zip(pss, heap)]
    correlation = None
    if len(paired) > 1:
        pss_mean = statistics.mean(pss)
        heap_mean = statistics.mean(heap)
        numerator = sum((left - pss_mean) * (right - heap_mean)
                        for left, right in zip(pss, heap))
        denominator = math.sqrt(
            sum((value - pss_mean) ** 2 for value in pss) *
            sum((value - heap_mean) ** 2 for value in heap)
        )
        if denominator:
            correlation = round(numerator / denominator, 6)
    durations = [int(row["workload_duration_ms"]) for row in exercise
                 if row.get("workload_duration_ms") is not None]
    return {
        "exercise_captures": len(exercise),
        "pss_vs_committed_java_heap_pearson": correlation,
        "pss_minus_committed_java_heap_mib": {
            "median": round(statistics.median(residual_mib), 3) if residual_mib else None,
            "minimum": round(min(residual_mib), 3) if residual_mib else None,
            "maximum": round(max(residual_mib), 3) if residual_mib else None,
        },
        "exercise_duration_ms": {
            "minimum": min(durations) if durations else None,
            "maximum": max(durations) if durations else None,
        },
    }


def number(value: object) -> str:
    if value is None:
        return "n/a"
    return "%.1f" % (float(value) / 1024.0)


def rate(value: object) -> str:
    if value is None:
        return "n/a"
    return "%.3f" % (float(value) / 1_000_000.0)


def markdown_report(summary: Sequence[Mapping[str, object]], errors: Sequence[str]) -> str:
    lines = [
        "# OpenJDK 8–25 memory-footprint results",
        "",
        "Generated from successful measured runs. Values are medians in MiB; variability is",
        "reported as median absolute deviation (MAD). JVM NMT is an explanatory view and is",
        "not added to OS PSS, USS, or RSS.",
        "",
    ]
    profiles = sorted({str(row["profile"]) for row in summary})
    workloads = sorted({str(row["workload"]) for row in summary})
    for profile in profiles:
        for workload in workloads:
            selected = [row for row in summary
                        if row["profile"] == profile and row["workload"] == workload]
            if not selected:
                continue
            lines.extend([
                "## %s / %s" % (profile.capitalize(), workload),
                "",
                "| OpenJDK | Runs | PSS MiB | PSS MAD | USS MiB | RSS MiB | NMT committed MiB | Operations M/s |",
                "| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
            ])
            for row in selected:
                lines.append(
                    "| {version} | {runs} | {pss} | {pss_mad} | {uss} | {rss} | {nmt} | {rate} |".format(
                        version=row["version"], runs=row["runs"],
                        pss=number(row.get("pss_kb_median")),
                        pss_mad=number(row.get("pss_kb_mad")),
                        uss=number(row.get("uss_kb_median")),
                        rss=number(row.get("rss_kb_median")),
                        nmt=number(row.get("nmt_committed_kb_median")),
                        rate=rate(row.get("operations_per_second_median")),
                    )
                )
            lines.append("")
    if errors:
        lines.extend(["## Validation errors", ""] + ["- " + error for error in errors] + [""])
    return "\n".join(lines)


def svg_chart(summary: Sequence[Mapping[str, object]], profile: str) -> str:
    selected = [row for row in summary if row["profile"] == profile]
    versions = sorted({int(row["version"]) for row in selected})
    width, height = 960, 520
    left, right, top, bottom = 72, 24, 44, 64
    plot_width, plot_height = width - left - right, height - top - bottom
    values = [(float(row["pss_kb_median"]) + float(row.get("pss_kb_mad") or 0)) / 1024.0
              for row in selected
              if row.get("pss_kb_median") is not None]
    maximum = max(values) if values else 1.0
    maximum *= 1.12

    def x(version: int) -> float:
        return left + (version - versions[0]) * plot_width / max(1, versions[-1] - versions[0])

    def y(value: float) -> float:
        return top + plot_height - value * plot_height / maximum

    parts = [
        '<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d" role="img" aria-labelledby="title desc">'
        % (width, height, width, height),
        '<!-- SPDX-License-Identifier: Apache-2.0 -->',
        '<title id="title">%s OpenJDK footprint by release</title>' % profile.capitalize(),
        '<desc id="desc">Median process-tree proportional set size for the HelloIdle and RuntimeExercise workloads. Error bars show median absolute deviation.</desc>',
        '<rect width="100%" height="100%" fill="#fbfaf7"/>',
        '<g font-family="system-ui,sans-serif" font-size="12" fill="#24343a">',
    ]
    for tick in range(6):
        value = maximum * tick / 5.0
        position = y(value)
        parts.append('<line x1="%g" y1="%g" x2="%g" y2="%g" stroke="#d8dedf"/>' %
                     (left, position, width - right, position))
        parts.append('<text x="%g" y="%g" text-anchor="end">%.0f</text>' %
                     (left - 8, position + 4, value))
    for version in versions:
        parts.append('<text x="%g" y="%g" text-anchor="middle">%d</text>' %
                     (x(version), height - bottom + 24, version))
    colors = {"hello": "#087e8b", "exercise": "#d95d39"}
    for workload in ("hello", "exercise"):
        rows = sorted((row for row in selected if row["workload"] == workload),
                      key=lambda row: int(row["version"]))
        point_rows = [row for row in rows if row.get("pss_kb_median") is not None]
        points = [(x(int(row["version"])), y(float(row["pss_kb_median"]) / 1024.0))
                  for row in point_rows]
        if not points:
            continue
        parts.append('<polyline fill="none" stroke="%s" stroke-width="3" points="%s"/>' %
                     (colors[workload], " ".join("%g,%g" % point for point in points)))
        for row, point in zip(point_rows, points):
            spread = float(row.get("pss_kb_mad") or 0) / 1024.0
            center = float(row["pss_kb_median"]) / 1024.0
            upper, lower = y(center + spread), y(max(0.0, center - spread))
            parts.append('<line x1="%g" y1="%g" x2="%g" y2="%g" stroke="%s" stroke-width="1.5" opacity="0.7"/>' %
                         (point[0], upper, point[0], lower, colors[workload]))
            parts.append('<line x1="%g" y1="%g" x2="%g" y2="%g" stroke="%s" stroke-width="1.5" opacity="0.7"/>' %
                         (point[0] - 3, upper, point[0] + 3, upper, colors[workload]))
            parts.append('<line x1="%g" y1="%g" x2="%g" y2="%g" stroke="%s" stroke-width="1.5" opacity="0.7"/>' %
                         (point[0] - 3, lower, point[0] + 3, lower, colors[workload]))
            parts.append('<circle cx="%g" cy="%g" r="3.5" fill="%s"/>' %
                         (point[0], point[1], colors[workload]))
    parts.extend([
        '<text x="%g" y="24" font-size="18" font-weight="700">OpenJDK process-tree PSS — %s profile</text>' %
        (left, profile),
        '<text x="18" y="%g" transform="rotate(-90 18 %g)" text-anchor="middle">Median PSS (MiB)</text>' %
        (top + plot_height / 2, top + plot_height / 2),
        '<circle cx="%g" cy="%g" r="4" fill="%s"/><text x="%g" y="%g">HelloIdle</text>' %
        (width - 220, 24, colors["hello"], width - 210, 28),
        '<circle cx="%g" cy="%g" r="4" fill="%s"/><text x="%g" y="%g">RuntimeExercise</text>' %
        (width - 120, 24, colors["exercise"], width - 110, 28),
        "</g></svg>\n",
    ])
    return "".join(parts)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("results", nargs="+", type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument(
        "--publish-output",
        type=pathlib.Path,
        help="also write only compact CSV/JSON and SVG charts to this directory",
    )
    args = parser.parse_args()
    roots = [path.resolve() for path in args.results]
    rows, errors = discover(roots)
    if not rows:
        parser.error("no successful measured runs found")
    summary = summarize(rows)
    published = publication_rows(summary)
    provenance = publication_provenance(roots, rows)
    analysis = publication_analysis(rows)
    args.output.mkdir(parents=True, exist_ok=True)
    write_csv(args.output / "raw-runs.csv", rows)
    write_csv(args.output / "summary.csv", summary)
    write_csv(args.output / "publication.csv", published)
    (args.output / "summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    (args.output / "publication.json").write_text(
        json.dumps(published, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    (args.output / "provenance.json").write_text(
        json.dumps(provenance, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    (args.output / "analysis.json").write_text(
        json.dumps(analysis, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    (args.output / "report.md").write_text(markdown_report(summary, errors), encoding="utf-8")
    for profile in sorted({str(row["profile"]) for row in summary}):
        (args.output / ("pss-%s.svg" % profile)).write_text(
            svg_chart(summary, profile), encoding="utf-8"
        )
    if args.publish_output is not None:
        args.publish_output.mkdir(parents=True, exist_ok=True)
        write_csv(args.publish_output / "publication.csv", published)
        (args.publish_output / "publication.json").write_text(
            json.dumps(published, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        (args.publish_output / "provenance.json").write_text(
            json.dumps(provenance, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        (args.publish_output / "analysis.json").write_text(
            json.dumps(analysis, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        for profile in sorted({str(row["profile"]) for row in summary}):
            (args.publish_output / ("pss-%s.svg" % profile)).write_text(
                svg_chart(summary, profile), encoding="utf-8"
            )
    for error in errors:
        print("ERROR", error, file=sys.stderr)
    print("Summarized %d measured runs into %s" % (len(rows), args.output))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
