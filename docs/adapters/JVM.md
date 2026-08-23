---
layout: default
title: HotSpot JVM adapter
description: Native Memory Tracking requirements, capture timing, artifacts, statuses, and interpretation.
permalink: /adapters/jvm/
---

# HotSpot JVM adapter

<p class="status available">Available now</p>

The JVM adapter uses HotSpot Native Memory Tracking (NMT) to explain memory
that also appears in the process-tree OS report. NMT is not a second ownership
total and must not be added to RSS, PSS, or USS.

## Enable NMT before startup

HotSpot cannot enable NMT retroactively for an already running JVM. Launch a
command through memftprt with:

```sh
./build/memftprt --enable-jvm-nmt --output-dir captures -- \
  java -jar app.jar
```

This adds `-XX:NativeMemoryTracking=summary` through `JAVA_TOOL_OPTIONS`. An
existing `detail` setting is preserved. For an independently started JVM:

```sh
java -XX:NativeMemoryTracking=summary -jar app.jar &
java_pid=$!
./build/memftprt --output-dir captures --pid "$java_pid"
```

If NMT was not enabled at startup, capture metadata reports
`nmt_not_enabled`; it does not substitute zero.

## Probe sequence

The collector detects HotSpot from the executable or a mapped `libjvm.so`, then
runs:

```text
jcmd PID VM.native_memory summary scale=KB
```

Because HotSpot’s dynamic attach handshake needs a responsive target, the
collector briefly resumes the traced JVM and services its SIGQUIT handshake.
The probe has an independent ten-second timeout and responds promptly to
collector termination signals. After probing, memftprt runs the quiet-window
logic again before the final OS freeze so attach activity is not mistaken for
the settled footprint.

No garbage collection is forced. The `forced_gc` metadata value remains
`false`.

## Artifacts and metadata

| Artifact or key | Meaning |
| --- | --- |
| `jvm_nmt_PID.txt` | Raw `jcmd` response for debugging and future parsing |
| `jvm_nmt_PID.tsv` | Normalized NMT category, reserved KB, and committed KB |
| `jvm_nmt` | Per-PID adapter outcome in capture metadata |
| `jvm_timestamp_ns` | Time at which the probe started |
| `jvm_duration_ns` | Adapter wall-clock duration |
| `jvm_method` | Recorded collection command |
| `jvm_forced_gc` | Whether the probe deliberately requested GC |

The adapter reports explicit states including `captured`, `nmt_not_enabled`,
`timeout`, `jcmd_failed`, `process_changed`, `unparsed`, and `interrupted`.
Missing runtime-version data remains empty.

## Read NMT correctly

NMT categories include Java heap, class metadata, threads, code cache, GC,
compiler, symbols, and internal allocations.

- **Reserved** is virtual address space held for possible future use.
- **Committed** is memory HotSpot has made available to the subsystem.
- **Resident ownership** is still described by OS PSS/USS.

Reserved memory can be far larger than RSS. Committed memory can differ from
RSS because commitment is not residency, pages can be shared or swapped, and
NMT does not include every third-party native allocation.

```text
OS PSS/USS = ownership layer
JVM NMT    = runtime explanation layer
```

Use `--no-jcmd` when runtime introspection would add unacceptable perturbation.
The OS-level process-tree capture remains available without NMT.
