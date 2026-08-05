# Runtime-aware memory capture

`memftprt` should treat Linux memory accounting as the authoritative ownership
layer and runtime introspection as an explanatory layer. Runtime reservations,
committed heaps, and used heaps overlap RSS/PSS/USS and must not be added to
those totals.

## Capture sequence

1. Discover the root process and all descendants. Launch mode uses Linux
   child-subreaper semantics so double-forked descendants remain attributable;
   terminal zombie/dead entries are omitted.
2. Wait for tree-wide quiescence using CPU, resident/virtual memory, state, and
   tree-membership stability.
3. Detect runtimes and ask each available runtime adapter for a snapshot while
   the target can still answer.
4. Wait for a short quiet window again because introspection allocates memory
   and CPU itself. Record the adapter duration and whether it requested a GC.
5. Freeze the process tree. The implemented `auto` backend uses cgroup v2
   `cgroup.freeze` only when the target is in a writable dedicated cgroup;
   otherwise it uses the ptrace/SIGSTOP fallback.
6. Capture `/proc/PID/smaps_rollup` for every process and aggregate PSS, USS,
   RSS, sharing classes, and swap.
7. Resume exactly the processes that the tool stopped.

If the target was already in a job-control stop, preserve that state after the
capture. Common termination signals request the same orderly thaw/resume path;
they do not terminate the collector in the middle of a frozen snapshot.

The default must never force garbage collection. A deliberate GC-before-capture
mode can be useful for retained-heap experiments, but it changes the thing being
measured and therefore needs an explicit option and an output flag.

## Three result layers

| Layer | Examples | Meaning |
| --- | --- | --- |
| OS ownership | RSS, PSS, USS, swap | Physical and swapped memory charged to the process tree |
| Runtime state | heap reserved/committed/used, stacks, JIT/code, metadata, GC structures | Runtime explanation of its address space and allocators |
| Attribution | classes, allocation sites, heap-profile samples | Optional, higher-cost diagnostic detail |

Every runtime result should include the PID, runtime/version when obtainable
without another intrusive probe, timestamp,
collection duration, units, collection method, and whether the probe forced a
GC. Unknown or unavailable data must remain absent rather than be reported as
zero.

## JVM adapter

The implemented adapter detects HotSpot from the executable or a mapped
`libjvm.so`, then queries Native Memory Tracking with:

```text
jcmd PID VM.native_memory summary scale=KB
```

NMT provides useful categories such as Java heap, classes, threads, code, GC,
compiler, symbols, and internal memory. It is disabled by default, must be
enabled when the JVM starts with `-XX:NativeMemoryTracking=summary` or `detail`,
and adds overhead. The adapter reports `nmt_not_enabled`
instead of silently substituting another number. NMT also does not account for
all third-party native allocations, so kernel PSS/USS remains the ground truth.

For commands launched by `memftprt`, `--enable-jvm-nmt` injects summary NMT via
`JAVA_TOOL_OPTIONS`. Attaching to an arbitrary JVM cannot retroactively enable
it. An inherited NMT-off setting is overridden; an existing detail setting is
preserved. The adapter keeps the raw `jcmd` response, parses category-level reserved
and committed KB into TSV, has a ten-second independent timeout, services the
HotSpot SIGQUIT attach handshake when the JVM is traced, and re-runs the quiet
window before freezing. The TSV records runtime, timestamp, collection
duration, method, unit, and forced-GC state; unavailable runtime version remains
empty. `--no-jcmd` disables the probe.

## Freezer backend

`--freeze auto` first resolves the target's unified cgroup, requires writable
freezer controls, and verifies it was not already frozen. For attached targets,
the cgroup subtree must exactly equal the measured PPID tree. For a launch
cgroup that the tool verified was initially empty, the same exact-membership
rule applies after the child joins it. It writes `1` only after those checks,
waits for `cgroup.events` to report `frozen 1`, and revalidates membership
before and during capture. Any failed precondition falls back to signals.

`--freeze cgroup` turns those failed preconditions into an error. In launch
mode, `--cgroup PATH` places the child into a caller-provided existing cgroup
after verifying that the delegated cgroup is writable, thawed, and empty. The
tool never freezes a shared cgroup and never thaws a cgroup it did not freeze.
After writing `0`, it waits for `cgroup.events` to confirm `frozen 0`; failure
is fatal and the owned path is retained in diagnostics rather than silently
reporting success.

The signal fallback tracks stopped descendants with pidfds where supported, so
PID reuse cannot redirect the later resume signal. Kernel-reported ptrace exec
events are consumed separately from genuine application `SIGTRAP` delivery.

## Node.js adapter

Use a small cooperative preload module, enabled explicitly through
`NODE_OPTIONS=--require=...`, and communicate over an inherited file descriptor
or a mode-0600 Unix-domain socket. It should return:

- `process.memoryUsage()` for RSS, heap total/used, external memory, and array
  buffers
- `v8.getHeapStatistics()` for V8-wide heap limits and usage
- `v8.getHeapSpaceStatistics()` for individual V8 spaces

V8 heap-space names and ordering can vary, so store them by returned name and
preserve the Node/V8 versions. In worker-thread applications, several
`process.memoryUsage()` fields describe only the calling thread; the adapter
should collect in each worker when possible and label the scope.

The inspector protocol can be a secondary adapter when it is already enabled.
Do not automatically expose or activate a network inspector merely to capture
memory: that changes application behavior and expands its attack surface.

## Go adapter

Use a small opt-in package that calls `runtime/metrics.Read` and serves a
versioned snapshot over an inherited descriptor or Unix-domain socket. Capture
the supported `/memory/classes/...` and `/gc/heap/...` metrics by name, along
with the Go version and each metric's kind. The runtime metric set evolves, so
the collector must discover metrics rather than rely on a fixed positional
schema.

Heap profiles from `runtime/pprof` or an already-protected `/debug/pprof/heap`
endpoint are an optional attribution layer, not the default footprint capture.
There is no stable, general external interface for extracting Go runtime memory
classes from an arbitrary uninstrumented PID; such a target still receives the
complete OS-level process-tree report.

## Adapter protocol

Use one local, versioned request for every runtime:

```json
{"version":1,"operation":"memory_snapshot","force_gc":false}
```

A response has a common envelope and runtime-owned metrics:

```json
{
  "version": 1,
  "runtime": "node",
  "runtime_version": "v24.0.0",
  "pid": 1234,
  "timestamp_ns": 0,
  "duration_ns": 0,
  "forced_gc": false,
  "metrics": {
    "heap.used": {"value": 123456, "unit": "bytes", "scope": "process"}
  }
}
```

Keep raw adapter responses alongside a normalized JSON result so that new
runtime categories can be interpreted later without rerunning the workload.
Adapters should have a short timeout, fail independently, and never prevent the
OS capture.

## Recommended delivery order

1. Add a normalized JSON output containing the existing tree-level OS metrics.
2. Add the Node preload adapter and a fixture with worker threads and buffers.
3. Add the Go package/Unix-socket adapter and a fixture that exercises heap,
   stacks, and goroutine growth.
4. Add optional high-cost profiles and explicit GC modes only after the low-
   perturbation snapshots are stable.
