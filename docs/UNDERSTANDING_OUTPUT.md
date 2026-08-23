---
layout: default
title: Understanding the output
description: Read PSS, USS, mappings, working-set measurements, metadata, and runtime reports correctly.
permalink: /understanding-output/
---

# Understanding memftprt output

`memftprt` deliberately reports several views of memory. They answer different
questions, use different units, and often have different values. That is not a
measurement error.

## The short version

For the memory physically attributable to the complete application, begin with:

1. total PSS in the selected TSV, JSON, or HTML report;
2. `TOTAL` USS to see the private, non-shared portion; and
3. per-process PSS rows to locate the worker responsible.

Use JVM NMT or another runtime adapter to explain how a runtime organizes its
address space. Never add runtime totals to RSS, PSS, or USS: they overlap.

## Why the console contains many numbers

The console has two families of metrics:

- **Process-tree totals** are the modern application-level measurement. They
  aggregate every captured process.
- **Root-process diagnostics** provide familiar views from
  `/proc/PID/status`, `stat`, `statm`, `ps`, and page-based counters.

Those root metrics are useful for comparison with older runs, but they are not
the complete footprint of an application with workers. All primary formats are
rendered from the same frozen in-memory report. TSV is the compatibility
format, JSON is the structured automation format, and HTML is the annotated
human-readable format.

Some intentionally retained details can look unusual:

- `RSS - /proc/pid/stat` appears once in pages and once in KB.
- `proccess_maps_PID.txt` uses the spelling defined by the artifact contract.
- Page-based values use the kernel's actual page size, which is also printed.
- Console metrics use a stable report order, while the TSV uses a stable schema
  designed for analysis.

## Virtual size, RSS, PSS, and USS

| Metric | Meaning | What commonly surprises users |
| --- | --- | --- |
| VSize | Reserved virtual address space | Can be much larger than physical RAM; reservations are not residency |
| RSS | Resident pages mapped by a process | Summing RSS across processes double-counts shared pages |
| PSS | RSS with each shared page divided among its mappers | Best approximation of physical ownership for a process tree |
| USS | Private resident pages | Memory that would be released if that process alone exited |
| Swap | Pages moved to swap | Not currently resident, but still part of the process state |

For a typical process or tree:

```text
VSize >= RSS >= PSS >= USS
```

This is a useful intuition, not an absolute invariant across every kernel field
and sampling interface.

### Why tree RSS can look too large

Suppose a parent and two workers each map the same 30 MB shared library. A
straight RSS sum can charge that mapping three times. PSS divides each shared
page among the processes mapping it, so the tree total is much closer to the
physical cost owned by the application.

USS excludes that library entirely unless a page became private. The gap
between PSS and USS is therefore a useful indication of shared memory cost.

## Reading `process_tree_memory_PID.tsv`

The file contains a header, one row per captured process, and a `TOTAL` row.
Important columns are:

- `pid`, `ppid`, `command`: process identity at capture time;
- `vsize_kb`: virtual address space from `/proc/PID/stat`;
- `rss_kb`, `pss_kb`, `uss_kb`: resident, proportionally shared, and private
  ownership;
- `pss_anon_kb`, `pss_file_kb`, `pss_shmem_kb`: PSS split by backing type;
- clean/dirty private and shared columns: page sharing and modification state;
- `swap_kb`, `swap_pss_kb`: swapped memory and proportional swap ownership;
- `anonymous_kb`, `referenced_kb`, and `locked_kb`: mapped anonymous
  pages, kernel reference state, and locked resident memory; and
- huge-page columns: transparent and explicit huge-page accounting when the
  running kernel exposes it.

An empty field means the running kernel did not provide that field. It does not
mean zero.

The kernel computes PSS backing splits per physical folio. They cannot be
inferred reliably from a VMA pathname because a file mapping may contain
anonymous copy-on-write pages. In detailed mode, memftprt takes all
process-level aggregate fields from one `smaps_rollup` snapshot when available,
so process PSS and its backing split reconcile. Per-mapping values come from the
full `smaps` walk and split fields remain unavailable rather than being guessed.
Because the kernel performs these walks separately, mapping-category totals may
differ slightly from the process aggregate.

The `TOTAL` row sums the available fields across the frozen process tree. The
command cell states how many processes contributed. A simple view is:

```sh
./examples/read-total.sh captures/process_tree_memory_*.tsv
```

## Reading JSON and HTML reports

`memory_report_PID.json` is a versioned document. Its top-level fields are:

- `schema_version`: currently `1`;
- `unit`: `KB` for every memory value;
- `capture`: root/launcher identity, timestamp, freeze backend, and cgroup;
- `runtime_status`: per-PID runtime probe outcomes;
- `processes`: one normalized memory object per captured process; and
- `total`: the aggregate of the frozen process tree.

Unavailable measurements are JSON `null`, while a measured zero remains `0`.
This distinction matters when comparing hosts or kernels. Consumers should
reject unsupported future schema versions rather than silently guessing their
meaning.

When mapping detail is requested, JSON additionally contains
`mapping_categories` and `mappings`; otherwise both are `null`. The
`capabilities` object states the smaps source used for each PID, pidfd usage,
and whether mapping or working-set capture was requested. These are additive
schema-version-1 fields.

`memory_report_PID.html` contains the same core measurements, totals cards, a
per-process table, runtime statuses, and a short explanation of VSize, RSS,
PSS, USS, and runtime overlap. It is self-contained: no JavaScript, network
requests, fonts, or other external assets are required. Target command names
and metadata are HTML-escaped before rendering.

## Reading mapping detail

`--mappings` switches each process from the faster aggregate rollup to one
full smaps read while frozen. TSV writes `memory_mappings_PID.tsv` and
`memory_mapping_categories_PID.tsv`; JSON and HTML include the same data in
their main report.

Categories are navigation aids, not allocator attribution. In particular,
`anonymous_executable` often includes JIT code but does not prove whether a
JVM, V8, another runtime, or application code created it. Use JVM NMT or a
future runtime adapter for that explanation.

Category PSS is the most useful comparison because shared pages are
proportioned. Category RSS and virtual size may count the same file once per
process, just as process-level RSS does.

## Referenced working set

Without `--working-set`, `referenced_kb` means pages currently marked
referenced by the kernel; metadata labels this `kernel_current`.

With `--working-set SEC`, memftprt:

1. settles the application and completes runtime probes;
2. freezes the exact tree and records PID/start-time and mapping identities;
3. clears referenced state for every process;
4. resumes the tree for the requested interval; and
5. freezes again, verifies identical topology, and reads Referenced values.

Metadata labels the result `since_clear_refs` and records both requested
seconds and actual monotonic duration. This is a kernel working-set estimate,
not an exact instruction-level trace. It is not RSS: pages can be resident but
untouched during the window.

The mode fails if `clear_refs` is unavailable, if the target enters an external
job-control stop before the requested interval elapses, or if process/mapping
identities do not match at the two frozen boundaries. It detects persistent
churn, but endpoint snapshots cannot observe a short-lived child or mapping
that appears and disappears entirely inside the running interval. JSON and TSV
metadata therefore label validation as `boundary_snapshots`.

Writing `clear_refs` is an intentional perturbation: it resets the target's
kernel reference state. Other reference-bit diagnostics will see that reset,
and kernel page-aging behavior may be influenced. Do not use this mode when
preserving that state is more important than measuring the interval.

## Knowing whether a capture bundle is complete

`capture_complete_PID.tsv` is written only after the target is resumed or
released and every requested report and metadata artifact has been committed.
Its artifact rows enumerate the payload files belonging to that run. The marker
from an earlier capture of the same PID is removed before any file is replaced.

Each artifact is independently atomic; the whole directory is not. Automation
should require the completion marker and consume its list. A missing marker
means the run failed, was interrupted, or is still publishing—even if some
individual files exist.

The marker and payload renames are followed by output-directory `fsync` calls.
This makes the marker contract durable across a host crash on filesystems that
provide normal Linux `fsync` guarantees; capture fails closed if the directory
cannot be synchronized. If memftprt creates the output directory, it also
synchronizes every newly created directory entry through the first existing
ancestor.

## Why `ps`, `top`, and the tree TSV differ

`process_ps_PID.txt` and `process_top_PID.txt` focus on the root PID for familiar
operator context. The tree TSV includes every captured descendant. Therefore a
worker-heavy service can have a small root-process RSS in `ps` but a large
process-tree PSS.

These diagnostic snapshots are best-effort. A minimal container may not ship
`ps` or `top`, and command execution may fail. The primary memory report remains
valid in that case, while capture metadata marks the corresponding
`snapshot_process_maps`, `snapshot_process_ps`, or `snapshot_process_top` value
as `unavailable` rather than pretending that the measurement was zero.

The kernel interfaces also define and round fields differently. `ps` commonly
reports RSS, while the tree TSV uses `smaps_rollup` or `smaps` for PSS and USS.
Compare like with like, in the same units and scope.

## JVM NMT versus OS memory

HotSpot Native Memory Tracking reports runtime categories such as Java heap,
classes, threads, code cache, GC, compiler, symbols, and internal allocations.
Each category can contain:

- **reserved** memory: virtual address space held for possible future use;
- **committed** memory: pages the JVM has committed for use; and
- runtime-specific used values that may be below committed capacity.

Reserved NMT memory can be far larger than RSS because most reserved addresses
need not have physical pages. Committed memory can also differ from RSS because
commitment and physical residency are not identical, pages may be shared or
swapped, and NMT does not account for every third-party native allocation.

Consequently:

```text
OS PSS/USS = ownership layer
JVM NMT    = explanatory runtime layer
```

Do not calculate `PSS + NMT committed`. The same JVM pages are represented in
both measurements.

The raw NMT response is preserved in `jvm_nmt_PID.txt`. Parsed categories are
written to `jvm_nmt_PID.tsv`. If NMT was disabled, `capture_metadata_PID.tsv`
records `nmt_not_enabled`; absence is never converted to a numeric zero.

## Why capture can happen later than expected

The default is not a timer. It waits until the entire process tree remains
stable for the quiet window. A background worker, JIT compiler, GC cycle, new
child, growing cache, or changing virtual mapping resets that window.

If runtime introspection runs, the target is responsive during the probe and is
then stabilized again because `jcmd` itself can allocate memory and consume CPU.
Only after that second wait does the final freeze and OS capture happen.

Use `--verbose` to see each stability decision. Use `--delay SEC` only when a
workload is intentionally continuous and a known time point is more useful than
quiescence.

## Understanding capture metadata

`capture_metadata_PID.tsv` explains how the sample was obtained:

- `freeze_backend`: `cgroup` or `signals`;
- `cgroup_path`: present when the cgroup freezer was used;
- `launcher_pid`: present when a wrapper exited and capture followed an adopted
  surviving worker;
- `jvm_nmt`: `captured`, `nmt_not_enabled`, `timeout`, `jcmd_failed`,
  `process_changed`, `interrupted`, or another explicit status; and
- JVM timing, method, unit, and forced-GC fields;
- `snapshot_process_maps`, `snapshot_process_ps`, and `snapshot_process_top`:
  `captured` or `unavailable` for the optional operator diagnostics;
- `mapping_detail`, `smaps_source`, and `pidfd`: how OS accounting was
  collected; and
- `working_set`, `clear_refs`, `reference_scope`, and window timing:
  whether Referenced is bounded by an observation interval.

This metadata should travel with the footprint. It prevents a missing runtime
probe or a fallback freeze from being mistaken for a zero measurement or a
different capture method.

## A practical analysis sequence

1. Confirm `freeze_backend` and runtime statuses in capture metadata.
2. Read `TOTAL` PSS and USS.
3. Sort per-process rows by PSS to find the dominant worker.
4. Compare anonymous, file-backed, and shared-memory PSS.
5. When mapping detail exists, inspect categories and then the largest mappings.
6. Check swap when RSS is lower than expected.
7. Use JVM NMT categories to explain runtime-owned reservations and commits.
8. Compare repeated captures only when workload phase, configuration, and
   capture method are equivalent.
