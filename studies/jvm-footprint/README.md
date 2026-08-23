# OpenJDK 8–25 memory-footprint study

This study compares official OpenJDK Linux x86-64 builds using identical Java
8 class files and the same Linux host. It captures a minimal live JVM and a JVM
that has spent 150 seconds exercising allocation, garbage collection, tiered
compilation, class metadata, synchronization, and worker threads.

The harness is reproducible and resumable. Downloaded JDKs and raw captures are
ignored by Git; source code, exact archive URLs, published SHA-256 values, run
metadata, and summarization logic are reviewable in this directory.

## What the study asks

1. What physical memory does a minimal, settled OpenJDK process own?
2. What footprint remains after sustained compiler, GC, and runtime activity?
3. Which HotSpot subsystems explain changes in the OS-level footprint?

The published matrix uses the controlled profile so version defaults do not
change the heap ceiling, collector, compiler threads, CDS mode, or CPU affinity.
The harness also supports a separate default-ergonomics experiment, but those
results must not be merged with the controlled study.

The primary OS metric is process-tree PSS. USS and RSS are retained for context.
HotSpot Native Memory Tracking (NMT) is an overlapping explanatory view and is
never added to PSS, USS, or RSS.

## OpenJDK inputs

[`jdk-manifest.tsv`](jdk-manifest.tsv) pins the original official OpenJDK GA
build for releases 9 through 25 and the official Java SE 8 OpenJDK reference
implementation. These archived builds are intentionally historical and must not
be used for production or exposed to untrusted applications or data.

Using original GA builds avoids comparing, for example, an early feature release
with an LTS release that has accumulated years of patch updates. The result is a
study of release-to-release JVM evolution, not a comparison of currently
supported production patch levels.

## Workloads

### `HelloIdle`

Writes a PID file, prints `Hello, world!`, emits a readiness marker, and blocks.
Keeping the process alive prevents the collector from racing JVM shutdown.

### `RuntimeExercise`

Runs four workers for 150 seconds. The workers create short-lived objects,
maintain a bounded retained set, invoke hot numeric code, use collections,
cryptographic hashing, reflection, a generated proxy, and synchronization. When
the duration expires, the workers park with their stacks intact. The application
does not call `System.gc()` before capture.

The readiness line records duration, operation count, worker count, and checksum.
This provides evidence that the workload completed rather than merely surviving
until the collector timed out.

Both workloads use Java 8 language and library APIs. They are compiled once by
OpenJDK 8 with class-file major version 52 and those exact files run on every JVM.

## Profiles

`controlled` is the primary comparison:

```text
-Xms64m
-Xmx256m
-Xss1m
-XX:+UseG1GC
-XX:ParallelGCThreads=2
-XX:ConcGCThreads=1
-XX:CICompilerCount=2
-Xshare:off
```

The complete process inherits a two-CPU affinity. Explicit GC and compiler
thread counts are used instead of `ActiveProcessorCount`, which is unavailable
in the early OpenJDK 8 reference build.

`default` supplies no footprint-related JVM flags. It answers a different
question: what did that GA release select on this particular host? Default heap,
collector, CDS, and worker-thread ergonomics changed between releases, so these
results must not be presented as an isolated comparison of implementation
efficiency.

NMT is enabled at `summary` level for the main study. A separate `--nmt off`
control can quantify the observer effect, but an NMT-disabled run cannot explain
memory by HotSpot subsystem.

## Capture protocol

For each run, the harness:

1. clears inherited Java option variables and fixes locale and timezone;
2. puts the target JDK's `bin` directory first in `PATH`, ensuring `java` and
   `jcmd` come from the same build;
3. passes `-XX:NativeMemoryTracking=summary` directly as the first JVM option;
   the compatibility pilot found that environment-based injection did not
   initialize NMT on the OpenJDK 8–17 GA launchers;
4. waits until the complete process tree is stable for five seconds;
5. lets memftprt probe NMT, settle again, freeze the tree, and write all formats;
6. validates the completion marker, JSON schema, mapping capability, and NMT
   status; and
7. terminates the parked JVM and verifies that it exited before the next run.

Referenced-working-set mode is deliberately excluded. Clearing kernel reference
bits would create a different, perturbing experiment. This study measures a
settled footprint snapshot.

Measured jobs are shuffled with a recorded seed. Three repetitions are the
runner default; the published matrix uses five. The report uses medians and
median absolute deviation rather than selecting a single favorable run.

## Published results

The 2026-08-23 controlled matrix completed all 180 captures: 18 versions, two
workloads, and five repetitions. The exercised workload markers reported
150,000–150,013 ms and positive operation counts in every run.

- [Read the GitHub Pages study](../../docs/studies/openjdk-8-25-memory-footprint.md).
- [`results/publication.csv`](results/publication.csv) and
  [`results/publication.json`](results/publication.json) contain the compact
  aggregate data.
- [`results/analysis.json`](results/analysis.json) records cited cross-run
  calculations.
- [`results/provenance.json`](results/provenance.json) preserves host and input
  identity.

## Run it

Build memftprt:

```sh
make
```

Download and verify all official OpenJDK archives, then compile the workloads:

```sh
studies/jvm-footprint/scripts/fetch-openjdk.sh
studies/jvm-footprint/scripts/compile-workloads.sh
```

Run a short compatibility pilot before committing hours to the full study:

```sh
studies/jvm-footprint/scripts/run_study.py \
  --versions 8-25 \
  --profiles controlled \
  --workloads hello,exercise \
  --exercise-seconds 5 \
  --repetitions 1 \
  --results .study-results/jvm-footprint/pilot
```

Run the primary controlled matrix:

```sh
studies/jvm-footprint/scripts/run_study.py \
  --versions 8-25 \
  --profiles controlled \
  --workloads hello,exercise \
  --exercise-seconds 150 \
  --stable-window 5 \
  --repetitions 5 \
  --results .study-results/jvm-footprint/controlled
```

Optionally run a default-ergonomics matrix into a separate result root:

```sh
studies/jvm-footprint/scripts/run_study.py \
  --versions 8-25 \
  --profiles default \
  --workloads hello,exercise \
  --exercise-seconds 150 \
  --repetitions 3 \
  --results .study-results/jvm-footprint/default
```

Successful jobs are skipped when the same command is resumed. Failed attempts
remain on disk and a retry receives a new directory; evidence is never silently
overwritten. If an earlier runner disappeared while its manifest still said
`running`, resume verifies the exact workload class and PID-file argument,
terminates only that matching stale target, marks the attempt `abandoned`, and
then schedules the retry.

Summarize and publish the controlled matrix:

```sh
studies/jvm-footprint/scripts/summarize.py \
  .study-results/jvm-footprint/controlled \
  --output .study-results/jvm-footprint/controlled-summary \
  --publish-output studies/jvm-footprint/results
```

The summary directory contains raw-run CSV, aggregate CSV and JSON, a Markdown
report, derived analysis, provenance, and self-contained SVG charts. The
optional publication directory receives only compact CSV/JSON, provenance,
analysis, and charts suitable for Git. Each raw row points back to its capture
directory, which contains the memftprt JSON, TSV, HTML, mappings, raw NMT output,
collector log, and run manifest.

## Interpretation boundaries

- PSS estimates proportional physical ownership at the capture instant. It is
  the headline footprint value for this single-process workload.
- USS counts pages private to the JVM. RSS counts every resident mapping in full
  and therefore overstates ownership of shared JDK and system-library pages.
- NMT `reserved` is address space, not resident memory. NMT `committed` is memory
  made available to a HotSpot subsystem, not proof that every page is resident.
- NMT omits some JDK-library and third-party native allocations and adds its own
  tracking overhead.
- File-page cache warmth, kernel state, firmware, CPU topology, and background
  host activity can influence results. The report must identify the host and
  avoid generalizing small differences below run-to-run variability.
- OpenJDK 8 is the official reference build `8u44-b02`; it is not a contemporary
  patched OpenJDK 8 production build. That limitation must remain visible in any
  blog post or chart.

## License

The study harness is licensed under Apache License 2.0 with the rest of this
repository. Downloaded OpenJDK binaries retain their upstream GPLv2 with
Classpath Exception licensing and are not redistributed by this repository.
