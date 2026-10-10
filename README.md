# memftprt

[![CodSpeed](https://img.shields.io/endpoint?url=https://codspeed.io/badge.json)](https://app.codspeed.io/menzew-org/memoryfootprint?utm_source=badge)

`memftprt` captures a consistent, process-tree-wide memory footprint on Linux.
It waits for an application to settle, optionally collects HotSpot JVM Native
Memory Tracking data, freezes the complete application tree, and writes both a
readable summary and TSV, JSON, or self-contained HTML reports.

`memftprt` combines Linux process-tree accounting, quiescence detection,
runtime-aware capture, and defensive lifecycle cleanup.

Read the hosted documentation at
[menzew.github.io/memoryfootprint](https://menzew.github.io/memoryfootprint/).

## What it measures

- the root process and all discoverable descendants;
- virtual size, RSS, PSS, USS, anonymous/file/shared memory, and swap;
- optional per-mapping categories, huge-page use, locked memory, and kernel
  reference state;
- optional referenced-working-set estimates over a controlled observation
  window;
- one per-process row plus a process-tree `TOTAL` row;
- additional root-process `status`, `stat`, `statm`, `ps`, `top`, and maps
  views for operator context; and
- HotSpot JVM Native Memory Tracking categories when available.

PSS is normally the best single number for the physical memory owned by a
multi-process application because shared pages are divided among their users.
See [Understanding the output](docs/UNDERSTANDING_OUTPUT.md) before comparing
the OS totals with JVM heap or NMT values.

## Requirements

- Linux with `/proc`
- GNU Make and a C++17 compiler
- permission to trace the target process
- permission to read full `smaps`, and to write `clear_refs` when using
  `--working-set`
- optional: `jcmd` for HotSpot JVM Native Memory Tracking
- optional: a delegated cgroup v2 for cgroup freezing

Launching a command through `memftprt` normally permits tracing its child.
Attaching to an unrelated PID may require matching credentials, a relaxed Yama
policy, or `CAP_SYS_PTRACE`. Container seccomp policies can also deny ptrace.

## Build

```sh
make
```

The executable is written to `build/memftprt`.

Install it under `/usr/local/bin`:

```sh
sudo make install
```

Or install under a user-owned prefix:

```sh
make install PREFIX="$HOME/.local"
```

## Five-minute how-to

### 1. Capture a command

```sh
./build/memftprt --format all --output-dir captures -- ./server --port 8080
```

The command starts immediately. `memftprt` waits until the whole process tree
is stable for three seconds, captures it, and leaves the application running.
`--format all` writes TSV for shell analysis, JSON for automation, and an HTML
report that opens directly in a browser. TSV remains the default for backward
compatibility.

### 2. Attach to a running process

```sh
./build/memftprt --output-dir captures --pid 1234
```

Attach mode can see current PPID descendants. For daemonizing applications,
launch mode is more reliable because the collector acts as a child subreaper
and follows surviving workers when a wrapper exits.

### 3. Inspect the result

```sh
column -s $'\t' -t captures/process_tree_memory_*.tsv | less -S
./examples/read-total.sh captures/process_tree_memory_*.tsv
xdg-open captures/memory_report_*.html
```

Start with the `TOTAL` row's PSS and USS. Use per-process rows to find which
worker owns the footprint, then use runtime-specific output to explain it.

### 4. Capture a JVM

NMT must be enabled when the JVM starts. When launching through the tool:

```sh
./build/memftprt --enable-jvm-nmt --output-dir captures -- \
  java -jar app.jar
```

Attaching cannot enable NMT retroactively. If it was not enabled, metadata says
`nmt_not_enabled` rather than inventing a zero value.

More copy-and-paste recipes are in [examples/](examples/README.md).

### 5. Explain the address space

~~~sh
./build/memftprt --mappings --format all --output-dir captures -- ./server
~~~

This reads full smaps while the tree is frozen and classifies mappings such
as the main executable, shared libraries, heap, stacks, shared memory, deleted
files, and executable anonymous memory. The last category often includes JIT
code, but it is intentionally not attributed to a specific runtime without
runtime evidence. A detailed read is rejected if any VMA lacks complete RSS/PSS
accounting, if one process exceeds 100,000 mappings or 128 MiB of smaps input,
or if the whole frozen tree exceeds 200,000 mappings or 256 MiB. Interrupts are
checked while smaps is parsed so cleanup does not wait for the remaining tree.

### 6. Estimate the referenced working set

~~~sh
./build/memftprt --working-set 10 --format all --output-dir captures -- ./server
~~~

After the application settles, the tool freezes the exact tree, clears Linux
page-reference state, lets the application run for ten seconds, then freezes
and reads it again. The --working-set option implies --mappings. The capture
fails if process identities or mapping topology differ between those two frozen
boundaries. Churn that begins and ends entirely inside the running interval is
not observable from endpoint snapshots and is disclosed as
`validation_scope=boundary_snapshots`.

`clear_refs` deliberately resets kernel reference state for the target. This
changes what other reference-bit diagnostics observe and may influence kernel
page-aging decisions, so use working-set mode only when that perturbation is
acceptable.

## Common options

| Option | Purpose |
| --- | --- |
| `-p, --pid PID` | Attach instead of launching a command |
| `-q, --stable-window SEC` | Required stable period; default `3` seconds |
| `-m, --timeout SEC` | Maximum quiescence wait; default `300` seconds |
| `-t, --delay SEC` | Use a fixed delay instead of quiescence detection |
| `-o, --output-dir DIR` | Write all capture artifacts to `DIR` |
| `-f, --format tsv\|json\|html\|all` | Select the primary report format; default `tsv` |
| `--freeze MODE` | Select `auto`, `cgroup`, or `signals` freezing |
| `--cgroup PATH` | Launch into an empty delegated cgroup v2 |
| `--enable-jvm-nmt` | Enable NMT for a JVM launched by the tool |
| `--no-jcmd` | Disable automatic JVM probing |
| `--mappings` | Capture and classify every mapping from full smaps |
| `--working-set SEC` | Estimate pages referenced during an observation window |
| `--quiet` | Suppress successful-run output; artifacts are still written |
| `-v, --verbose` | Show capture decisions; repeat for more detail |

Use `--` before a launched command so its options are not interpreted by
`memftprt`. Run `memftprt --help` for the built-in reference.

## How consistency is established

The default detector samples the complete process tree every 500 ms. A sample
is stable when:

- no process is running (`R`) or in uninterruptible sleep (`D`);
- aggregate CPU grows by no more than one clock tick;
- RSS changes by no more than one page;
- virtual size changes by no more than one page; and
- the PID/start-time identity set is unchanged.

The tree must remain stable for the configured window. At the timeout,
`memftprt` warns and captures the current state instead of waiting forever.
Fixed-delay mode bypasses quiescence detection but still freezes the tree before
reading memory.

Working-set mode adds a second freeze. Runtime probes run before reference
state is cleared, so jcmd activity is excluded from the observation window.
The same freeze backend must be used at both boundaries, and PID/start-time and
mapping identities must match exactly at those boundaries. This check detects
persistent churn but cannot prove that no short-lived child or mapping existed
and disappeared between snapshots.

`--freeze auto` uses cgroup v2 only when its controls are writable and its
membership exactly equals the application tree. Membership is revalidated
after freezing and during capture. Otherwise it uses a ptrace/SIGSTOP fallback
that resumes only the exact process identities it stopped. The tool confirms a
cgroup has thawed before reporting success.

## Output artifacts

| Artifact | Contents |
| --- | --- |
| `process_tree_memory_PID.tsv` | Per-process memory and aggregate `TOTAL` row |
| `memory_report_PID.json` | Versioned machine-readable capture, process rows, and totals |
| `memory_report_PID.html` | Self-contained browser report with interpretation guidance |
| `memory_mappings_PID.tsv` | One row per mapping when --mappings is enabled |
| `memory_mapping_categories_PID.tsv` | Tree-wide mapping-category totals |
| `capture_metadata_PID.tsv` | Freeze backend, cgroup, launcher, and runtime status |
| `capture_complete_PID.tsv` | Final bundle-completion marker and payload artifact list |
| `proccess_maps_PID.txt` | Best-effort `/proc/PID/maps`; filename spelling is part of the artifact contract |
| `process_ps_PID.txt` | Best-effort root-process `ps` snapshot |
| `process_top_PID.txt` | Best-effort root-process `top` snapshot |
| `jvm_nmt_PID.txt` | Raw HotSpot NMT response, when detected |
| `jvm_nmt_PID.tsv` | Normalized NMT categories, when parsing succeeds |

Artifacts are mode `0600`. They stay on descriptor-backed randomized temporary
files through write and `fsync`, then are atomically renamed and followed by an
output-directory `fsync`. When the output directory is newly created, each new
directory entry is also synchronized through its existing ancestor. A
per-target lock prevents concurrent collectors from writing the same PID-based
result set.

Capture metadata is written regardless of primary report format because it
explains how the measurement was obtained. Diagnostic snapshots are attempted
for every format, but missing or failing `cat`, `ps`, or `top` commands do not
invalidate the primary memory capture; metadata records each snapshot as
`captured` or `unavailable`.

Each file is atomic, but a multi-file capture is not one filesystem transaction.
At capture start, memftprt removes any prior completion marker for that PID and
durably records that removal. It resumes or releases the target, publishes every
requested report, then writes `capture_complete_PID.tsv` last. Automation
should treat a bundle as valid only when that marker exists and should use its
artifact list rather than directory globs that may include files from an older
capture.

Linux process names and mapping paths are byte sequences rather than guaranteed
UTF-8. Serializers preserve valid UTF-8 and replace malformed sequences with
the Unicode replacement character so JSON, HTML, and TSV remain decodable.

JSON and HTML keep mapping detail in the main report. TSV uses separate mapping
and category tables so the existing process table remains easy to consume.

## Exit behavior

- `0`: capture succeeded
- `2`: command-line usage error
- `128 + signal`: cleanup completed after `SIGINT`, `SIGTERM`, `SIGHUP`, or
  `SIGQUIT`
- `254` or `255`: runtime capture or internal invariant failure

Termination signals follow the same thaw/resume path before exit. If a cgroup
cannot be confirmed thawed, the diagnostic includes its path for recovery. An
interrupted capture never publishes a completion marker; individually atomic
payload files may remain if interruption occurred during publication.

## Tests

```sh
make test
```

The suite covers CLI behavior, nested output directories, quiet mode,
fixed-delay and quiescent capture, attach mode, exec events, signals, external
stops, zombies, reparented descendants, launcher handoff, JVM NMT, secure
artifacts, missing diagnostic utilities, interruption during waits, smaps, and
jcmd, completion-marker suppression, and cleanup.

Successful cgroup freezing requires a delegated cgroup:

```sh
MEMFTPRT_TEST_CGROUP=/sys/fs/cgroup/my-test make test
```

Set `MEMFTPRT_REQUIRE_CGROUP_TEST=1` in CI to require that path. If a nested
environment denies ptrace, the suite exits `77`; use
`MEMFTPRT_ALLOW_PTRACE_SKIP=1` only when another job provides ptrace coverage.
The GitHub Actions workflow provisions a delegated cgroup and requires this
integration test rather than silently skipping it.

## Benchmarks

Google Benchmark microbenchmarks in `benchmarks/` cover smaps parsing, mapping
classification, report rendering (TSV, JSON, HTML), and HotSpot NMT parsing
using synthetic inputs. They require CMake 3.16+ and network access to fetch
the benchmark library:

```sh
cmake -S benchmarks -B build/benchmarks
cmake --build build/benchmarks -j
./build/benchmarks/memftprt_bench
```

The CodSpeed workflow runs them on every pull request and push to `main` with
CodSpeed's CPU simulation instrument to track performance changes.

## Further reading

- [Understanding the output](docs/UNDERSTANDING_OUTPUT.md)
- [Capture model and freeze guarantees](docs/CAPTURE_MODEL.md)
- [Runtime adapter design](docs/RUNTIME_ADAPTERS.md)
- [HotSpot JVM adapter](docs/adapters/JVM.md)
- [OpenJDK 8–25 memory-footprint study](docs/studies/openjdk-8-25-memory-footprint.md)
- [Node.js adapter design](docs/adapters/NODE.md)
- [Go adapter design](docs/adapters/GO.md)
- [Code architecture](docs/ARCHITECTURE.md)
- [Examples](examples/README.md)
- [Contributing](CONTRIBUTING.md)

## License

Licensed under the [Apache License 2.0](LICENSE).
