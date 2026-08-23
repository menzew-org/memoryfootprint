<!-- SPDX-License-Identifier: Apache-2.0 -->

# Published controlled results

These are the compact, reviewable outputs from the OpenJDK 8–25 controlled
study captured on 2026-08-23. The matrix contains five successful repetitions
of both workloads on every version: 18 versions × 2 workloads × 5 repetitions,
or 180 captures.

- [`publication.csv`](publication.csv) and
  [`publication.json`](publication.json) contain the per-version medians and
  median absolute deviations used by the report.
- [`analysis.json`](analysis.json) contains cross-run calculations cited by the
  report.
- [`provenance.json`](provenance.json) records the host, exact OpenJDK archives,
  hashes of measurement inputs, and capture cardinality.
- [`pss-controlled.svg`](pss-controlled.svg) plots median process-tree PSS with
  MAD error bars.

The multi-file raw captures remain excluded from Git because they contain 180
complete memftprt bundles. Regenerate the publication files from a raw result
directory with:

```sh
studies/jvm-footprint/scripts/summarize.py \
  .study-results/jvm-footprint/controlled \
  --output .study-results/jvm-footprint/controlled-summary \
  --publish-output studies/jvm-footprint/results
```

The archived GA JDKs themselves are not redistributed. Their official URLs and
SHA-256 values are pinned in [`../jdk-manifest.tsv`](../jdk-manifest.tsv).
