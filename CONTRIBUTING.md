# Contributing

Contributions are welcome. Keep changes focused on reliable, low-perturbation
memory capture and preserve the safety guarantees around ptrace, signals,
cgroups, output artifacts, and process identity.

## Development

```sh
make clean all
make test
```

The integration suite uses ptrace. If the environment denies tracing, it exits
with status 77. Do not convert that to a successful release-CI skip unless a
separate job provides ptrace coverage.

## Code layout

- `src/main.cpp` is the minimal process entry point.
- `src/cli.*` owns platform-neutral arguments and help text.
- `src/config.h` is the shared configuration contract.
- `src/artifact_store.*` securely publishes all output artifacts.
- `src/memory_report.*` owns the normalized report model and serializers.
- `src/runtime/hotspot_nmt.*` parses JVM NMT summaries without platform calls.
- `src/linux/capture_linux.cpp` coordinates Linux target lifecycle, freezing,
  runtime probing, and capture.
- `src/linux/proc_memory.*` reads Linux `smaps_rollup`/`smaps` accounting.

Platform backends should populate the shared report model rather than adding
platform conditions to serializers. A Windows backend can therefore replace
Linux process discovery, quiescence, freezing, and accounting while preserving
the CLI and output contracts.

## Change expectations

- Add a regression test for behavior changes.
- Never freeze or signal processes outside the verified application tree.
- Identify a process by PID and start time when an operation is deferred.
- Keep unknown measurements absent; do not substitute zero.
- Keep runtime metrics separate from OS RSS/PSS/USS totals.
- Write artifacts with mode `0600` through secure atomic temporary files.
- Update `--help`, examples, and user documentation when behavior changes.

Before submitting a change:

```sh
make clean test
git diff --check
```

Freezer changes should also be tested with an empty delegated cgroup through
`MEMFTPRT_TEST_CGROUP`.
