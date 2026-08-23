---
layout: default
title: Go adapter design
description: Proposed cooperative runtime metrics contract for Go services and its profiling boundaries.
permalink: /adapters/go/
---

# Go adapter design

<p class="status planned">Design only — not yet implemented</p>

An arbitrary Go PID does not expose a stable external interface for runtime
memory classes. The safe design is an opt-in package inside the application;
without it, memftprt still captures the complete OS-level process-tree report.

## Cooperative package

The package should serve one versioned snapshot over an inherited descriptor
or a mode-`0600` Unix-domain socket. It should use `runtime/metrics.Read` rather
than private runtime symbols and return the Go version, metric name, metric
kind, value, unit, timestamp, and collection duration.

The collector should discover supported metric descriptions at runtime. Go’s
metric set evolves, so a fixed positional list would turn new or missing
metrics into mislabelled values.

## Metric families

Useful low-perturbation families include:

- `/memory/classes/...` for heap, stacks, metadata, cache, profiling buckets,
  and other runtime-managed memory classes;
- `/gc/heap/...` for live objects, allocated bytes, goal, and object counts;
- scheduler or goroutine metrics only as context, not memory totals; and
- process identity and Go version for later interpretation.

Each runtime metric explains memory management state. It does not replace
kernel PSS, USS, RSS, or swap accounting.

## Profiles are a separate layer

`runtime/pprof` heap profiles or an already protected `/debug/pprof/heap`
endpoint can attribute retained objects to allocation sites. They are
higher-cost diagnostics with different semantics from a footprint snapshot.
memftprt should not start a public pprof HTTP listener automatically.

If a protected pprof endpoint already exists, access must be explicitly
configured, bounded by an independent timeout, and labelled as attribution
data. A heap profile must never be presented as process RSS or total native
memory.

## Proposed output and failure behavior

Retain the raw response and normalize metrics by returned name. Metadata should
record adapter version, Go version, transport, duration, scope, and whether GC
was forced. The default is `forced_gc=false`; a future explicit GC mode would
need separate naming because it changes the measured state.

Adapter absence, protocol mismatch, or timeout must leave the OS capture valid
and emit an explicit unavailable status. Until the cooperative package is
implemented, `--mappings` can distinguish heap-like anonymous mappings,
stacks, executable/file mappings, and shared libraries without assigning them
to Go runtime categories.
