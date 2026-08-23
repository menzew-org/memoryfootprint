---
layout: default
title: Consistent process-tree memory capture
description: Wait for an application to settle, freeze its complete process tree, and capture an explainable memory footprint.
permalink: /
---

<section class="hero">
  <p class="eyebrow">Linux memory measurement</p>
  <h1>Capture memory at a moment you can explain.</h1>
  <p class="hero-copy">memftprt waits for the whole application to settle, freezes the verified process tree, reads kernel memory accounting, and resumes only what it stopped.</p>
  <div class="hero-actions">
    <a class="button primary" href="#quick-start">Start a capture</a>
    <a class="button secondary" href="{{ '/understanding-output/' | relative_url }}">Read the numbers</a>
  </div>
</section>

<div class="capture-strip" aria-label="Capture sequence: settle, freeze, measure, resume">
  <span><b>01</b> Settle</span>
  <span><b>02</b> Freeze</span>
  <span><b>03</b> Measure</span>
  <span><b>04</b> Resume</span>
</div>

## What you get

<div class="feature-grid">
  <section>
    <p class="feature-label">Ownership</p>
    <h3>Whole-tree PSS and USS</h3>
    <p>Measure the root process and its workers without double-counting every shared page.</p>
  </section>
  <section>
    <p class="feature-label">Explanation</p>
    <h3>Mappings and JVM NMT</h3>
    <p>Connect OS ownership to address-space categories and optional HotSpot runtime data.</p>
  </section>
  <section>
    <p class="feature-label">Automation</p>
    <h3>TSV, JSON, and HTML</h3>
    <p>Use stable machine-readable output or open a self-contained annotated report.</p>
  </section>
</div>

## Quick start {#quick-start}

Build the C++17 executable:

```sh
git clone https://github.com/menzew/memoryfootprint.git
cd memoryfootprint
make
```

Capture a command after its complete process tree remains stable for three seconds:

```sh
./build/memftprt --format all --output-dir captures -- \
  ./server --port 8080
```

Or attach to an existing process:

```sh
./build/memftprt --output-dir captures --pid 1234
```

Start with `TOTAL` PSS in `process_tree_memory_PID.tsv`. PSS is the most useful
single estimate of physical ownership because shared pages are divided among
their mappers. Then inspect per-process rows and mappings to locate the cost.

<div class="callout">
  <strong>Do not add runtime totals to OS totals.</strong>
  JVM reserved or committed memory explains how the runtime organizes its
  address space. It overlaps RSS, PSS, and USS rather than adding to them.
</div>

## Choose the next guide

<div class="guide-grid">
  <a href="{{ '/understanding-output/' | relative_url }}">
    <span>Interpretation guide</span>
    <strong>Understand PSS, USS, mappings, and working set</strong>
    <em>Read the output →</em>
  </a>
  <a href="{{ '/runtime-adapters/' | relative_url }}">
    <span>Runtime model</span>
    <strong>Relate JVM, Node.js, and Go memory to the OS view</strong>
    <em>Explore runtimes →</em>
  </a>
  <a href="{{ '/architecture/' | relative_url }}">
    <span>Implementation</span>
    <strong>Follow lifecycle ownership and artifact publication</strong>
    <em>Read the architecture →</em>
  </a>
</div>

More commands are available in the
[repository examples](https://github.com/menzew/memoryfootprint/tree/main/examples),
including JVM NMT, fixed-delay capture, cgroup freezing, mapping reports, and
referenced-working-set measurement.
