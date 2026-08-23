---
layout: default
title: How much memory do OpenJDK 8–25 JVMs retain?
description: A controlled Linux study of the memory retained by official OpenJDK 8 through 25 builds at startup and after 150 seconds of JVM activity.
permalink: /studies/openjdk-8-25-memory-footprint/
date: 2026-08-23
author: menzew
---

<!-- SPDX-License-Identifier: Apache-2.0 -->

<p class="eyebrow">Measured study · 23 August 2026 · menzew</p>

# How much memory do OpenJDK 8–25 JVMs retain?

The answer depends on **when** you look. A JVM that has only started is not the
same thing as a JVM that has spent two and a half minutes allocating objects,
compiling hot code, running GC, and exercising runtime services.

This study captured both states on the same Linux host. It ran the same class
files and JVM limits on 18 official OpenJDK releases, repeated every
release-and-workload combination five times, and accepted 180 validated
captures.

<div class="study-metrics" aria-label="Study summary">
  <section><strong>18</strong><span>OpenJDK releases</span></section>
  <section><strong>2</strong><span>workload states</span></section>
  <section><strong>5</strong><span>runs per state</span></section>
  <section><strong>180</strong><span>validated captures</span></section>
</div>

## Start with the two snapshots

<div class="workload-grid">
  <section class="workload-card workload-idle">
    <p class="workload-step">Snapshot 1 · startup baseline</p>
    <h3><code>HelloIdle</code></h3>
    <p>Print <code>Hello, world!</code>, report readiness, and park. This asks:
    <strong>what is resident before application work?</strong></p>
    <p class="workload-result"><strong>20.4–36.5 MiB</strong><span>median PSS across releases</span></p>
  </section>
  <section class="workload-card workload-active">
    <p class="workload-step">Snapshot 2 · after activity</p>
    <h3><code>RuntimeExercise</code></h3>
    <p>Run four workers for 150 seconds, report completion, and park without
    requesting a GC. This asks: <strong>what does the JVM naturally retain after
    work?</strong></p>
    <p class="workload-result"><strong>204.7–303.3 MiB</strong><span>median PSS across releases</span></p>
  </section>
</div>

<aside class="study-scope">
  <strong>This is not a large-application benchmark.</strong>
  <span><code>RuntimeExercise</code> is a small synthetic workload designed to
  activate allocation, GC, JIT compilation, reflection, synchronization,
  hashing, and worker stacks. A framework or server application is a separate
  follow-up study.</span>
</aside>

## The result in one chart

The vertical axis is process-tree **PSS**, the physical memory charged to the
captured processes after shared resident pages are divided among their users.
For each OpenJDK release:

<div class="chart-guide" aria-label="How to read the chart">
  <span><i class="chart-swatch idle" aria-hidden="true"></i><strong>Blue</strong> is <code>HelloIdle</code>.</span>
  <span><i class="chart-swatch active" aria-hidden="true"></i><strong>Orange</strong> is <code>RuntimeExercise</code>.</span>
  <span><i class="chart-dot" aria-hidden="true"></i>The dot is the median of five captures.</span>
  <span><i class="chart-error" aria-hidden="true"></i>The bar is median absolute deviation (MAD).</span>
</div>

<figure class="study-figure">
  <img src="https://menzew.github.io/memoryfootprint/assets/studies/openjdk-8-25/pss-controlled.svg"
       alt="Line chart of median PSS for HelloIdle and RuntimeExercise on OpenJDK 8 through 25. HelloIdle stays between 20 and 37 MiB. RuntimeExercise ranges from 205 to 303 MiB and becomes more variable from OpenJDK 12 onward.">
  <figcaption>Median process-tree PSS from five captures. MAD bars show run-to-run
  spread; they are not confidence intervals. <span class="mobile-chart-note">Scroll
  the chart horizontally to inspect every release.</span></figcaption>
</figure>

## Three findings

<ol class="study-findings">
  <li>
    <strong>Startup stayed small and fairly flat.</strong>
    <span><code>HelloIdle</code> used 20.4–36.5 MiB. OpenJDK 8 was the low outlier;
    releases 9–25 all landed between 31.3 and 36.5 MiB.</span>
  </li>
  <li>
    <strong>Activity changed the scale of the result.</strong>
    <span><code>RuntimeExercise</code> used 204.7–303.3 MiB. OpenJDK 8–11 clustered
    around 293–303 MiB; OpenJDK 22–25 landed around 205–217 MiB. The releases
    between them did not form a smooth downward line.</span>
  </li>
  <li>
    <strong>The active line mostly followed retained heap.</strong>
    <span>Across all 90 <code>RuntimeExercise</code> captures, PSS and HotSpot's
    committed Java heap had a Pearson correlation of <strong>0.997153</strong>.
    This is why the chart should not be read as a simple “best JVM” ranking.</span>
  </li>
</ol>

## Exact `RuntimeExercise` results

This is the active-workload result set. Every row is the median of five
captures; `±` is MAD. “Committed Java heap” comes from HotSpot Native Memory
Tracking (NMT). It helps explain PSS, but it is **not added to PSS**.

| OpenJDK | RuntimeExercise PSS MiB | Committed Java heap MiB | Operations/s |
| ---: | ---: | ---: | ---: |
| 8 | 292.6 ± 0.1 | 256 | 208,850 |
| 9 | 303.3 ± 0.2 | 256 | 222,836 |
| 10 | 301.0 ± 0.3 | 256 | 222,693 |
| 11 | 301.7 ± 0.3 | 256 | 226,180 |
| 12 | 246.8 ± 36.5 | 201 | 207,924 |
| 13 | 267.0 ± 26.6 | 223 | 224,139 |
| 14 | 274.5 ± 25.6 | 232 | 232,662 |
| 15 | 268.0 ± 4.9 | 224 | 217,642 |
| 16 | 283.6 ± 11.4 | 240 | 236,425 |
| 17 | 228.8 ± 43.0 | 185 | 215,984 |
| 18 | 248.3 ± 4.3 | 204 | 215,965 |
| 19 | 244.6 ± 25.3 | 199 | 208,915 |
| 20 | 250.4 ± 10.3 | 210 | 213,766 |
| 21 | 259.4 ± 13.5 | 216 | 217,816 |
| 22 | 204.7 ± 20.7 | 162 | 207,624 |
| 23 | 216.9 ± 17.7 | 172 | 196,729 |
| 24 | 212.2 ± 9.8 | 163 | 209,332 |
| 25 | 208.9 ± 18.4 | 159 | 201,309 |

<details class="study-data" markdown="1">
  <summary>Show all <code>HelloIdle</code> baseline results</summary>

| OpenJDK | HelloIdle PSS MiB |
| ---: | ---: |
| 8 | 20.4 ± 0.0 |
| 9 | 33.2 ± 0.4 |
| 10 | 34.3 ± 0.3 |
| 11 | 34.6 ± 0.1 |
| 12 | 34.4 ± 0.0 |
| 13 | 32.5 ± 0.0 |
| 14 | 31.7 ± 0.0 |
| 15 | 33.2 ± 0.1 |
| 16 | 32.9 ± 0.0 |
| 17 | 31.3 ± 0.1 |
| 18 | 33.4 ± 0.4 |
| 19 | 34.6 ± 0.1 |
| 20 | 33.9 ± 0.2 |
| 21 | 35.7 ± 0.1 |
| 22 | 34.3 ± 0.1 |
| 23 | 34.6 ± 0.0 |
| 24 | 35.3 ± 0.0 |
| 25 | 36.5 ± 0.0 |

</details>

<nav class="data-downloads" aria-label="Download study data">
  <strong>Use the data</strong>
  <a href="https://menzew.github.io/memoryfootprint/assets/studies/openjdk-8-25/publication.csv">Download CSV</a>
  <a href="https://menzew.github.io/memoryfootprint/assets/studies/openjdk-8-25/publication.json">Download JSON</a>
  <a href="https://menzew.github.io/memoryfootprint/assets/studies/openjdk-8-25/analysis.json">View derived analysis</a>
  <a href="https://menzew.github.io/memoryfootprint/assets/studies/openjdk-8-25/provenance.json">Audit capture provenance</a>
</nav>

## Why the active result jumps between runs

The workload stops after the same amount of **time**, not at the same point in a
GC cycle. Memory management continues asynchronously while the workers run.
When the deadline arrives, one JVM may have more heap committed than another,
even when both completed the same workload.

That difference is visible in the data. OpenJDK 8–11 committed the full 256 MiB
maximum heap in every active run, so their PSS results were tightly grouped.
From OpenJDK 12 onward, committed heap varied between repetitions. OpenJDK 17,
for example, committed 144, 181, 185, 239, and 242 MiB in its five runs. Its PSS
varied with it, which produces the large MAD bar in the chart.

The study deliberately does not call `System.gc()`. Forcing a full collection
would answer “how small can this JVM become after an intervention?” The question
here is “what did this JVM naturally retain when work stopped?”

[JEP 346](https://openjdk.org/jeps/346), delivered in JDK 12, gave G1 a
mechanism to return unused committed heap during idle periods. The break visible
at JDK 12 is compatible with a more elastic G1, but this experiment does not
isolate that feature: capture followed a five-second stable window, not a
separately controlled long-idle interval.

Across the 90 active captures, the arithmetic difference between PSS and the
NMT committed-heap value had a median of 45.0 MiB and a 36.5–51.0 MiB range.
That difference must not be relabelled “non-heap memory”—PSS and committed bytes
describe different things—but its stability reinforces that heap commitment is
driving most of the movement in this particular experiment.

## Why `-Xms64m` does not mean 64 MiB of PSS

`-Xms64m` tells HotSpot how much heap to commit initially. It does not guarantee
that Linux has backed every committed address with a physical page. Pages can
remain untouched, and therefore non-resident.

That is why `HelloIdle` showed only 20–36 MiB of PSS even though HotSpot usually
reported a 64 MiB committed heap plus other committed JVM subsystems. The
[Linux procfs documentation](https://www.kernel.org/doc/html/v6.15/filesystems/proc.html)
describes PSS's proportional accounting for resident shared pages. RSS counts
shared resident pages in full for each mapper; USS counts only private resident
pages.

OpenJDK 8's 20.4 MiB result should not be treated as a recommendation. The
matrix uses the official historical `8u44-b02` reference build, while later
rows use their corresponding official GA builds. Class-data sharing was also
disabled everywhere. These are controlled historical comparisons, not current
production-distribution recommendations.

## NMT explains the JVM; it is not another footprint total

PSS is the operating system's view of resident physical pages. NMT is HotSpot's
view of memory it reserved or committed by subsystem. A reserved address range
may use no RAM, and a committed range may be only partly resident. Adding NMT to
PSS would double-count concepts that overlap.

The OpenJDK 10 GA output is also a useful warning against treating every NMT
headline as directly comparable. Its `HelloIdle` capture used 34.3 MiB PSS,
while NMT's top-level total claimed about 1,354.5 MiB committed. The raw report
attributed about 1,054,893 KiB to `Class`, but its nested metadata and class-space
lines reported only 6,144 KiB and 640 KiB committed. That report is internally
inconsistent.

The raw value remains in the published data, but the study excludes OpenJDK
10's NMT total from cross-version conclusions. Its OS PSS and NMT Java-heap
lines do not share the anomaly. Category schemas also change over time:
[Elastic Metaspace in JDK 16](https://openjdk.org/jeps/387), for example,
changed class-metadata management and newer NMT reports expose a separate
`Metaspace` category.

## How the study ran

The study used official builds from the [OpenJDK archive](https://jdk.java.net/archive/)
for releases 9–25 and the official OpenJDK Java SE 8 reference build. Archive
URLs and SHA-256 checksums are pinned in the
[study manifest](https://github.com/menzew/memoryfootprint/blob/main/studies/jvm-footprint/jdk-manifest.tsv).
The downloaded JDKs are not redistributed.

Both workloads were compiled once with OpenJDK 8 to class-file version 52. The
same class files then ran on every release with the same limits:

```text
-XX:NativeMemoryTracking=summary
-Xms64m -Xmx256m -Xss1m
-XX:+UseG1GC
-XX:ParallelGCThreads=2 -XX:ConcGCThreads=1 -XX:CICompilerCount=2
-Xshare:off
```

Each capture followed the same sequence:

<ol class="study-sequence">
  <li><strong>Launch</strong><span>Pin the JVM to CPUs 0 and 1.</span></li>
  <li><strong>Wait</strong><span>Observe readiness or workload completion, then a five-second stable process tree.</span></li>
  <li><strong>Ask HotSpot</strong><span>Collect NMT with that release's matching <code>jcmd</code>.</span></li>
  <li><strong>Freeze</strong><span>Stop the process tree so the OS snapshot describes one instant.</span></li>
  <li><strong>Capture</strong><span>Collect mappings and JSON, HTML, and TSV output with memftprt.</span></li>
  <li><strong>Validate</strong><span>Accept only a complete marker and the expected artifacts.</span></li>
</ol>

The 180 jobs were shuffled with seed `20260823`. Locale, timezone, inherited
Java option variables, and workload files were fixed. The host was x86-64
Ubuntu 24.04 on Linux `6.8.0-137-generic`, with 12 logical CPUs available and
251.6 GiB RAM. All 90 active-workload markers reported 150,000–150,013 ms and a
positive operation count.

<details class="study-data" markdown="1">
  <summary>Data-integrity and instrumentation notes</summary>

NMT was enabled for every capture. Oracle's
[NMT documentation](https://docs.oracle.com/javase/8/docs/technotes/guides/vm/nmt-8.html)
documents both its overhead and gaps, including untracked third-party native
code and some JDK library allocations. Equal instrumentation improves the
comparison; it does not make observation free.

One OpenJDK 25 active-workload retry was found running alongside a stale target.
Because shared file-backed pages could bias proportional accounting, that retry
was marked invalid, replaced by a clean run, and every CSV, JSON, SVG, and
article value was regenerated. The provenance file records the accepted
attempts.

</details>

## What this study does not prove

- It measures one Linux host, one synthetic active workload, one instant after
  activity, and five repetitions. Differences smaller than the MAD are not
  findings.
- It does not measure a large framework, application server, or production
  service. Those workloads bring different class paths, thread pools, caches,
  native libraries, and allocation patterns.
- It compares historical GA/reference binaries, not current security-patched
  builds or downstream OpenJDK distributions.
- It fixes G1, heap limits, thread counts, CPU affinity, and disables CDS. It
  therefore does not describe each release's production defaults.
- Operations per second is a completion signal and useful context, not a Java
  performance benchmark. The workload mixes unrelated mechanisms and runs with
  NMT enabled.

## Reproduce or extend it

The workloads, pinned manifest, runner, and publication script live under
[`studies/jvm-footprint`](https://github.com/menzew/memoryfootprint/tree/main/studies/jvm-footprint).
After building memftprt and fetching the archives:

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

The runner is resumable. It revalidates completed runs before skipping them,
retires interrupted attempts, cleans up their exact target, and gives each retry
its own directory. The
[study README](https://github.com/menzew/memoryfootprint/blob/main/studies/jvm-footprint/README.md)
covers archive download, pilot runs, summarization, and the optional
default-profile comparison.
