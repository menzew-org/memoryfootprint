---
layout: default
title: Capture model
description: The timing, freeze ownership, validation, cancellation, and publication guarantees behind a footprint.
permalink: /capture-model/
---

# Capture model

A useful footprint needs a defensible answer to two questions: **which
processes were included?** and **when were their memory values read?** memftprt
answers both at the process-tree boundary rather than sampling one PID and
assuming its workers are negligible.

## End-to-end sequence

1. Launch or attach to the root process and record its process identity.
2. Discover descendants and wait for a stable tree, or use an explicit fixed
   delay.
3. Run cooperative runtime probes while the target can respond.
4. Settle the tree again after probing.
5. Freeze the validated tree with a cgroup or signal fallback.
6. Read kernel accounting while the tree is stationary.
7. Thaw or resume only the process identities stopped by memftprt.
8. Render reports from the in-memory snapshot.
9. Publish the bundle-completion marker last.

The target is resumed before JSON, HTML, or TSV rendering. Large serializers
therefore cannot extend the application pause or produce different samples for
different formats.

## What “settled” means

The default detector samples the complete tree every 500 ms. A sample resets
the quiet window when any of these conditions is observed:

- a member is running (`R`) or blocked in uninterruptible sleep (`D`);
- aggregate CPU advances by more than one clock tick;
- aggregate RSS or virtual size changes by more than one page; or
- the PID/start-time identity set changes.

The tree must remain within those thresholds for the configured stable window,
which defaults to three seconds. The overall wait defaults to 300 seconds. At
that timeout, memftprt warns and captures the current state instead of waiting
forever. `--delay SEC` bypasses this heuristic and chooses an explicit time
point, but the final memory read is still frozen.

Quiescence is a practical acquisition rule, not proof that every allocator,
cache, or runtime subsystem is semantically idle.

## Why cgroup freezing is conditional

`cgroup.freeze` can stop a complete cgroup atomically from the collector’s
point of view, including descendants that are difficult to coordinate one at a
time. That is useful only when the cgroup contains exactly the application
tree. Freezing a shared service or session cgroup would stop unrelated work.

Before choosing the cgroup backend, memftprt requires:

- cgroup v2 membership that resolves beneath `/sys/fs/cgroup`;
- writable `cgroup.freeze` control;
- a currently thawed group;
- readable membership for the complete cgroup subtree; and
- exact equality between cgroup membership and the discovered process tree.

Membership is checked again after the kernel reports `frozen 1` and during the
capture. The tool writes `0` only for a cgroup it froze and waits for
`cgroup.events` to confirm `frozen 0`. It never assumes ownership of an already
frozen cgroup.

For launch mode, `--cgroup PATH` requires an existing, empty, delegated cgroup.
Delegation means the caller can move the child into the group and operate its
freezer controls; merely creating a directory in cgroupfs is not sufficient.

## Signal fallback and process identity

When the cgroup preconditions fail in `--freeze auto` mode, memftprt uses its
ptrace/SIGSTOP fallback. It tracks every process it stops by PID and start time,
and uses pidfds when the kernel supports them. This prevents a later resume
from being sent to an unrelated process that reused the same PID.

An existing external job-control stop is preserved. The collector resumes only
states it owns, consumes kernel ptrace-exec events separately, and forwards
genuine application signals rather than treating every `SIGTRAP` as internal.
Use `--freeze cgroup` when falling back would make the experiment invalid; that
mode fails if the cgroup safety contract cannot be established.

## Referenced working set

`--working-set SEC` measures pages marked referenced during a controlled
interval:

```text
freeze A → record identities and mappings → write clear_refs → run SEC
         → freeze B → revalidate identities and mappings → read Referenced
```

The same freeze backend must be used at both boundaries. PID/start-time
identities and mapping topology must match exactly. A persistent child or VMA
change therefore fails closed rather than producing an ambiguous working set.

This is endpoint validation. A mapping or process that appears and disappears
entirely between the two freezes cannot be observed. Metadata labels that
limitation as `boundary_snapshots`. Writing `clear_refs` also perturbs kernel
reference state, so the operation is opt-in.

Without `--working-set`, `Referenced` means the kernel’s current accumulated
reference state (`kernel_current`). With it, the field means referenced since
memftprt cleared the state (`since_clear_refs`). Neither is an exact trace of
every memory load.

## Cancellation and cleanup

`SIGINT`, `SIGTERM`, `SIGHUP`, and `SIGQUIT` request orderly cancellation.
Long-running waits, detailed smaps parsing, optional snapshots, and `jcmd`
polling check that request. Helpers are terminated, temporary files discarded,
owned stops resumed, and owned cgroups thawed before the collector returns
`128 + signal`.

An interrupted capture does not publish `capture_complete_PID.tsv`. Some
individually atomic payloads can exist if interruption arrived during report
publication, so automation must use the marker rather than directory globs.

## Failure policy

| Condition | Result |
| --- | --- |
| Optional `ps`, `top`, or maps snapshot unavailable | Primary capture continues; metadata says `unavailable` |
| Runtime probe unavailable or timed out | OS capture continues; runtime status explains why |
| Tree identity or working-set mapping topology changes | Capture fails closed |
| Cgroup membership broadens or becomes unreadable | Cgroup backend is rejected or capture fails |
| Target cannot be confirmed resumed or cgroup thawed | Capture fails and reports recovery context |
| Report bundle lacks a completion marker | Consumers must treat it as incomplete |

Read [Understanding the output](https://menzew.github.io/memoryfootprint/understanding-output/)
for the accounting fields and [Code architecture](https://menzew.github.io/memoryfootprint/architecture/)
for the modules enforcing these rules.
