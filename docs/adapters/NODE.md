---
layout: default
title: Node.js adapter design
description: Proposed cooperative Node.js and V8 memory snapshot contract, scope, and safety boundaries.
permalink: /adapters/node/
---

# Node.js adapter design

<p class="status planned">Design only — not yet implemented</p>

Node.js processes already receive the complete OS-level footprint. A future
adapter should explain V8 heaps, external allocations, and worker scopes
without automatically exposing the inspector or forcing garbage collection.

## Cooperative preload

The preferred design is a small opt-in preload package:

```sh
NODE_OPTIONS="--require=/path/to/memftprt-node-adapter" \
  ./build/memftprt --output-dir captures -- node server.js
```

The preload should communicate through an inherited file descriptor or a
mode-`0600` Unix-domain socket. It must authenticate the local peer, use a
versioned request, impose a short timeout, and remove the socket during normal
or interrupted cleanup.

## Metrics to collect

| API | Useful fields | Important scope caveat |
| --- | --- | --- |
| `process.memoryUsage()` | RSS, heap total/used, external, array buffers | Some fields are isolate/worker-specific while RSS is process-wide |
| `v8.getHeapStatistics()` | Heap size limit, total/used heap, executable heap, contexts | Describes the calling V8 isolate |
| `v8.getHeapSpaceStatistics()` | Per-space size, used, available, physical size | Space names and ordering can change by V8 version |

Every value needs an explicit unit and scope. Store heap-space rows by the name
returned by V8 rather than relying on positional ordering. Preserve Node.js and
V8 versions with the snapshot.

## Worker threads

A request handled only by the main thread cannot claim to describe every
worker isolate. The adapter should coordinate a snapshot request with each
registered worker and report one row per isolate plus clearly scoped
process-wide fields. Unresponsive workers should be listed as unavailable,
not silently omitted from a supposed aggregate.

## Inspector boundary

The inspector protocol is acceptable only when the application already exposes
and protects it. memftprt should not activate a TCP inspector as a side effect
of measurement: that changes application behavior and creates a new attack
surface. Heap snapshots and allocation profiles also belong to the higher-cost
attribution layer, not the default footprint path.

## Proposed output

The raw versioned response should be retained alongside normalized JSON rows.
Metadata should identify the transport, adapter duration, Node/V8 versions,
worker coverage, and `forced_gc=false`. Failure or timeout must not invalidate
the OS report.

Until this adapter exists, use process-tree PSS/USS and `--mappings` to measure
Node.js ownership. Anonymous executable mappings may contain JIT code, but they
must not be labelled as V8 without runtime evidence.
