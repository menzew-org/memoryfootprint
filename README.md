# memftprt

`memftprt` captures a consistent, process-tree-wide memory footprint on Linux.
It waits for an application to settle, optionally collects HotSpot JVM Native
Memory Tracking data, freezes the complete application tree, and writes both a
readable summary and machine-readable artifacts.

The maintained code is a C++17 reconstruction of the useful behavior of
`memftprt` 1.39, extended with modern Linux process-tree accounting,
quiescence detection, runtime-aware capture, and defensive cleanup.

## What it measures

- the root process and all discoverable descendants;
- virtual size, RSS, PSS, USS, anonymous/file/shared memory, and swap;
- one per-process row plus a process-tree `TOTAL` row;
- the root process's legacy `status`, `stat`, `statm`, `ps`, `top`, and maps
  views for compatibility; and
- HotSpot JVM Native Memory Tracking categories when available.

PSS is normally the best single number for the physical memory owned by a
multi-process application because shared pages are divided among their users.
See [Understanding the output](docs/UNDERSTANDING_OUTPUT.md) before comparing
the OS totals with JVM heap or NMT values.

## Requirements

- Linux with `/proc`
- GNU Make and a C++17 compiler
- permission to trace the target process
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
./build/memftprt --output-dir captures -- ./server --port 8080
```

The command starts immediately. `memftprt` waits until the whole process tree
is stable for three seconds, captures it, and leaves the application running.

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

## Common options

| Option | Purpose |
| --- | --- |
| `-p, --pid PID` | Attach instead of launching a command |
| `-q, --stable-window SEC` | Required stable period; default `3` seconds |
| `-m, --timeout SEC` | Maximum quiescence wait; default `300` seconds |
| `-t, --delay SEC` | Use a fixed delay instead of quiescence detection |
| `-o, --output-dir DIR` | Write all capture artifacts to `DIR` |
| `--freeze auto|cgroup|signals` | Select or require a freeze backend |
| `--cgroup PATH` | Launch into an empty delegated cgroup v2 |
| `--enable-jvm-nmt` | Enable NMT for a JVM launched by the tool |
| `--no-jcmd` | Disable automatic JVM probing |
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

`--freeze auto` uses cgroup v2 only when its controls are writable and its
membership exactly equals the application tree. Membership is revalidated
after freezing and during capture. Otherwise it uses a ptrace/SIGSTOP fallback
that resumes only the exact process identities it stopped. The tool confirms a
cgroup has thawed before reporting success.

## Output artifacts

| Artifact | Contents |
| --- | --- |
| `process_tree_memory_PID.tsv` | Per-process memory and aggregate `TOTAL` row |
| `capture_metadata_PID.tsv` | Freeze backend, cgroup, launcher, and runtime status |
| `proccess_maps_PID.txt` | `/proc/PID/maps`; legacy misspelling retained |
| `process_ps_PID.txt` | Root-process `ps` snapshot |
| `process_top_PID.txt` | Root-process `top` snapshot |
| `jvm_nmt_PID.txt` | Raw HotSpot NMT response, when detected |
| `jvm_nmt_PID.tsv` | Normalized NMT categories, when parsing succeeds |

Artifacts are mode `0600`. They stay on descriptor-backed randomized temporary
files through write and `fsync`, then are atomically renamed. A per-target lock
prevents concurrent collectors from writing the same PID-based result set.

## Exit behavior

- `0`: capture succeeded
- `2`: command-line usage error
- `128 + signal`: cleanup completed after `SIGINT`, `SIGTERM`, `SIGHUP`, or
  `SIGQUIT`
- `254` or `255`: runtime capture or internal invariant failure

Termination signals follow the same thaw/resume path before exit. If a cgroup
cannot be confirmed thawed, the diagnostic includes its path for recovery.

## Tests

```sh
make test
```

The suite covers CLI behavior, output directories, quiet mode, fixed-delay and
quiescent capture, attach mode, exec events, signals, external stops, zombies,
reparented descendants, launcher handoff, JVM NMT, secure artifacts, and
cleanup.

Successful cgroup freezing requires a delegated cgroup:

```sh
MEMFTPRT_TEST_CGROUP=/sys/fs/cgroup/my-test make test
```

Set `MEMFTPRT_REQUIRE_CGROUP_TEST=1` in CI to require that path. If a nested
environment denies ptrace, the suite exits `77`; use
`MEMFTPRT_ALLOW_PTRACE_SKIP=1` only when another job provides ptrace coverage.

## Further reading

- [Understanding the output](docs/UNDERSTANDING_OUTPUT.md)
- [Runtime adapter design](docs/RUNTIME_ADAPTERS.md)
- [Examples](examples/README.md)
- [Contributing](CONTRIBUTING.md)

## License

Licensed under the [Apache License 2.0](LICENSE).
