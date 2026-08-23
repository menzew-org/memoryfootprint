---
layout: default
title: Runtime adapters
description: Keep OS memory ownership separate from runtime explanations and attribution data.
permalink: /runtime-adapters/
---

# Runtime adapters

The kernel report answers **how much physical and virtual memory belongs to the
application tree**. A runtime adapter answers **what that runtime believes the
memory is for**. These layers overlap, so runtime totals must never be added to
RSS, PSS, or USS.

| Layer | Examples | Question answered |
| --- | --- | --- |
| OS ownership | RSS, PSS, USS, swap | How much memory is resident, private, proportionally shared, or swapped? |
| Runtime state | heap reserved/committed/used, stacks, code, metadata | How has the runtime organized its address space? |
| Attribution | classes, allocation sites, profiles | Which application objects or call sites retained memory? |

## Adapter status

<div class="guide-grid adapter-grid">
  <a href="{{ '/adapters/jvm/' | relative_url }}">
    <span>Available</span>
    <strong>HotSpot JVM Native Memory Tracking</strong>
    <em>JVM adapter →</em>
  </a>
  <a href="{{ '/adapters/node/' | relative_url }}">
    <span>Design contract</span>
    <strong>Node.js and V8 cooperative snapshot</strong>
    <em>Node.js adapter →</em>
  </a>
  <a href="{{ '/adapters/go/' | relative_url }}">
    <span>Design contract</span>
    <strong>Go runtime metrics snapshot</strong>
    <em>Go adapter →</em>
  </a>
</div>

Only the JVM adapter is currently implemented. Node.js and Go targets still
receive the complete OS-level process-tree report; their pages define a safe,
cooperative design rather than claiming functionality that is not present.

## Common adapter contract

An adapter result needs enough context to remain interpretable later:

- target PID and stable process identity;
- runtime name and version when available without another intrusive probe;
- snapshot timestamp and collection duration;
- collection method and units;
- metric scope, such as process, isolate, worker, or runtime-wide;
- whether the probe forced garbage collection; and
- an explicit status when data is unavailable.

Unknown values stay absent. An unavailable heap metric is not zero. Raw adapter
responses should be retained alongside normalized output so a newer parser can
interpret categories without rerunning the workload.

## Timing and perturbation

Runtime introspection runs before the final OS freeze because cooperative tools
need a responsive target. The collector then waits for the process tree to
settle again: an attach handshake, parser, JIT activity, or helper allocation
can change the footprint being measured.

The default capture must not force garbage collection. A forced-GC mode would
measure a different state and therefore requires an explicit option plus a
recorded `forced_gc` value.

## Local protocol for cooperative adapters

Node.js and Go adapters should use a local inherited file descriptor or a
mode-`0600` Unix-domain socket. They should not expose a new network listener.
A versioned request keeps the collector and runtime package independently
upgradable:

```json
{"version":1,"operation":"memory_snapshot","force_gc":false}
```

The response envelope carries runtime-owned metrics without pretending they
are OS ownership totals:

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

Adapter timeouts are independent. A missing, failed, or timed-out runtime probe
must not invalidate an otherwise sound kernel footprint.

See the [capture model](https://menzew.github.io/memoryfootprint/capture-model/) for where these
probes occur relative to settling, freezing, working-set boundaries, and
artifact publication.
