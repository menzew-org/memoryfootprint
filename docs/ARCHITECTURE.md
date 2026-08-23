---
layout: default
title: Code architecture
description: Module boundaries, lifecycle ownership, memory accounting, and artifact publication.
permalink: /architecture/
---

# Code architecture

`memftprt` separates user interface, capture policy, platform measurement, and
presentation. The current implementation targets Linux and keeps configuration,
measurement, and report serialization behind explicit module boundaries.

```text
main.cpp
  -> cli.* -> config.h
  -> capture.h
       -> linux/capture_linux.cpp
            -> linux/proc_memory.*
            -> linux/working_set.*
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
3. briefly resume a HotSpot target for a bounded, interruptible `jcmd` probe
   when runtime cooperation is needed, then settle it again;
4. freeze and revalidate the complete tree;
5. read all memory values while the tree is stationary;
6. resume only process identities that the collector itself stopped; and
7. serialize and publish reports, attempt bounded best-effort diagnostic
   snapshots, and publish the completion marker last.

With --working-set, the orchestrator performs an additional frozen boundary
after runtime probing and re-settling. It records process and mapping
identities, clears referenced-page state through the small working-set module,
resumes for the requested interval, then requires the same identities at the
final freeze. Freeze backend selection is locked across both boundaries. These
are endpoint comparisons: transient process or VMA churn that starts and ends
inside the interval is outside the observable validation scope.

The orchestrator keeps PID start times where an action is deferred. This
prevents PID reuse from redirecting a later signal to an unrelated process.
Cgroup membership is checked before and after freezing so a cgroup cannot
silently broaden the capture scope. Subtree enumeration is fail-closed: an
unreadable entry, missing `cgroup.procs`, parse error, or unexpected symlink
rejects the backend instead of hiding possible members.

### Linux memory accounting

`linux/proc_memory.*` reads `smaps_rollup` when available and falls back to
`smaps`. It keeps field availability separate from numeric values and combines
availability conservatively: a tree total is unavailable if any contributing
process did not expose that field.

Detailed mode reads full smaps once per frozen process, preserving each mapping
header and accounting block. Mapping and category reports come from that walk.
When available, all process-level aggregate fields come from one subsequent
`smaps_rollup` walk, keeping total PSS coherent with `Pss_Anon`, `Pss_File`, and
`Pss_Shmem`. Because these are two kernel walks, mapping-category sums can vary
slightly from the process aggregate and are presented as independently sampled
views. Classification is deliberately generic; anonymous executable memory is
not called JVM or V8 JIT memory without runtime-specific evidence.

Every parsed VMA must contain RSS and PSS. Detailed input is bounded to 100,000
VMAs and 128 MiB per process and to 200,000 VMAs and 256 MiB for the whole tree.
The parser checks capture cancellation between records. These limits keep an
extreme procfs input from causing unbounded allocation or delaying cleanup
indefinitely while a target is stopped. Full smaps cannot accurately derive
`Pss_Anon`, `Pss_File`, and `Pss_Shmem` from VMA pathnames because COW pages
change backing inside a mapping; process-level values therefore use the
complete `smaps_rollup` snapshot when the kernel exposes it.

### Referenced working set

`linux/working_set.*` owns only the Linux clear_refs write and stable mapping
identity representation. It does not stop or resume processes. Keeping those
side effects in the capture orchestrator preserves one cleanup authority for
signals, ptrace stops, pidfds, and cgroup freezing.

### Reports

`memory_report.*` serializes one normalized `MemoryReport` to TSV, JSON, and
HTML after the target tree has been resumed or released. Serializers never
inspect a live process. This prevents multiple formats from sampling at
different times, avoids holding a frozen application during potentially large
rendering allocations, and makes report tests independent of ptrace.

The JSON schema is explicitly versioned. HTML is self-contained and escapes
target-provided strings. Because procfs text is not guaranteed to be UTF-8,
every serializer replaces malformed sequences with U+FFFD. TSV remains the
default compatibility interface.

### Runtime parsing

`runtime/hotspot_nmt.*` converts raw NMT summaries into typed categories. It
does not launch `jcmd` or inspect processes. Probe execution remains in the
capture orchestrator because it owns target lifecycle and attach behavior.

### Artifact store

`artifact_store.*` creates output directories, obtains a per-target lock, and
writes through randomized descriptor-backed temporary files. Each artifact is
restricted to mode `0600`, flushed with `fsync`, atomically renamed only after
it is complete, and made durable with an output-directory `fsync`. The
store also synchronizes the parent chain for directory entries it created, so
the output directory itself survives a crash under normal Linux filesystem
`fsync` guarantees. The orchestrator durably invalidates an older completion
marker before replacing artifacts and writes `capture_complete_PID.tsv` last.
Individual files remain atomic; the marker is the crash-durable bundle-level
validity contract. Failed or interrupted output has no completion marker.

## Adding a runtime adapter

Runtime adapters explain portions of the OS-level footprint; they do not add
new memory to it. The HotSpot adapter records raw `jcmd` output, normalized NMT
categories, and an explicit status. Follow the contract in
[Runtime adapter design]({{ '/runtime-adapters/' | relative_url }}): bounded execution, stable process
identity, no forced GC by default, explicit unavailable states, and separate
runtime artifacts.
