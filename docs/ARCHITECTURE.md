# Code architecture

`memftprt` separates user interface, capture policy, platform measurement, and
presentation. Capturing memory consistently is platform-specific, but
configuration and report semantics should not change between Linux and a
future Windows backend.

```text
main.cpp
  -> cli.* -> config.h
  -> capture.h
       -> linux/capture_linux.cpp
            -> linux/proc_memory.*
            -> runtime/hotspot_nmt.*
            -> artifact_store.*
            -> memory_report.*
```

## Module responsibilities

### Entry point and CLI

`main.cpp` translates parser and runtime exceptions into conventional exit
codes. `cli.*` validates arguments without touching processes or files. Shared
options live in `CaptureConfig`, including capture timing, freeze policy,
runtime probes, output directory, and report format.

### Linux capture orchestrator

`linux/capture_linux.cpp` owns the stateful Linux workflow:

1. launch or attach to the target;
2. wait for a stable process-tree sample or a fixed delay;
3. briefly resume a HotSpot target when `jcmd` needs cooperation, then settle
   it again;
4. freeze and revalidate the complete tree;
5. read all memory values while the tree is stationary; and
6. resume only process identities that the collector itself stopped.

The orchestrator keeps PID start times where an action is deferred. This
prevents PID reuse from redirecting a later signal to an unrelated process.
Cgroup membership is checked before and after freezing so a cgroup cannot
silently broaden the capture scope.

### Linux memory accounting

`linux/proc_memory.*` reads `smaps_rollup` when available and falls back to
`smaps`. It keeps field availability separate from numeric values and combines
availability conservatively: a tree total is unavailable if any contributing
process did not expose that field.

This is the primary replacement point for Windows. A Windows implementation
will need native process-tree discovery, a consistent suspension mechanism,
and working-set/private/shared accounting mapped carefully onto the common
report model. Fields without an honest equivalent must remain unavailable.

### Reports

`memory_report.*` serializes one normalized `MemoryReport` to TSV, JSON, and
HTML. Serializers never inspect a live process. This prevents multiple formats
from sampling at different times and makes report tests independent of ptrace.

The JSON schema is explicitly versioned. HTML is self-contained and escapes
target-provided strings. TSV remains the default compatibility interface.

### Runtime parsing

`runtime/hotspot_nmt.*` converts raw NMT summaries into typed categories. It
does not launch `jcmd` or inspect processes, so Linux and Windows capture
backends can share it. Probe execution remains backend-specific because target
lifecycle and attach behavior differ by operating system.

### Artifact store

`artifact_store.*` creates output directories, obtains a per-target lock, and
writes through randomized descriptor-backed temporary files. Each artifact is
restricted to mode `0600`, flushed with `fsync`, and atomically renamed only
after it is complete. Failed or interrupted output remains uncommitted.

## Adding a runtime adapter

Runtime adapters explain portions of the OS-level footprint; they do not add
new memory to it. The HotSpot adapter records raw `jcmd` output, normalized NMT
categories, and an explicit status. Follow the contract in
[Runtime adapter design](RUNTIME_ADAPTERS.md): bounded execution, stable process
identity, no forced GC by default, explicit unavailable states, and separate
runtime artifacts.
