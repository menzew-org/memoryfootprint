# Understanding memftprt output

`memftprt` deliberately reports several views of memory. They answer different
questions, use different units, and often have different values. That is not a
measurement error.

## The short version

For the memory physically attributable to the complete application, begin with:

1. `TOTAL` PSS in `process_tree_memory_PID.tsv`;
2. `TOTAL` USS to see the private, non-shared portion; and
3. per-process PSS rows to locate the worker responsible.

Use JVM NMT or another runtime adapter to explain how a runtime organizes its
address space. Never add runtime totals to RSS, PSS, or USS: they overlap.

## Why the console contains many numbers

The console has two families of metrics:

- **Process-tree totals** are the modern application-level measurement. They
  aggregate every captured process.
- **Legacy root metrics** preserve the original tool's views from
  `/proc/PID/status`, `stat`, `statm`, `ps`, and page-based counters.

Those root metrics are useful for comparison with older runs, but they are not
the complete footprint of an application with workers. The process-tree TSV is
the authoritative machine-readable result.

Some intentionally retained details can look unusual:

- `RSS - /proc/pid/stat` appears once in pages and once in KB.
- `proccess_maps_PID.txt` retains the historical misspelling for compatibility.
- Page-based values use the kernel's actual page size, which is also printed.
- Metrics are shown in the reconstructed legacy report order, while the TSV
  uses a stable schema designed for analysis.

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
- `swap_kb`, `swap_pss_kb`: swapped memory and proportional swap ownership.

An empty field means the running kernel did not provide that field. It does not
mean zero.

The `TOTAL` row sums the available fields across the frozen process tree. The
command cell states how many processes contributed. A simple view is:

```sh
./examples/read-total.sh captures/process_tree_memory_*.tsv
```

## Why `ps`, `top`, and the tree TSV differ

`process_ps_PID.txt` and `process_top_PID.txt` focus on the root PID for familiar
operator context. The tree TSV includes every captured descendant. Therefore a
worker-heavy service can have a small root-process RSS in `ps` but a large
process-tree PSS.

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
  `process_changed`, or another explicit status; and
- JVM timing, method, unit, and forced-GC fields.

This metadata should travel with the footprint. It prevents a missing runtime
probe or a fallback freeze from being mistaken for a zero measurement or a
different capture method.

## A practical analysis sequence

1. Confirm `freeze_backend` and runtime statuses in capture metadata.
2. Read `TOTAL` PSS and USS.
3. Sort per-process rows by PSS to find the dominant worker.
4. Compare anonymous, file-backed, and shared-memory PSS.
5. Check swap when RSS is lower than expected.
6. Use JVM NMT categories to explain runtime-owned reservations and commits.
7. Compare repeated captures only when workload phase, configuration, and
   capture method are equivalent.
