# Examples

The commands below assume `memftprt` is installed. Replace `memftprt` with
`./build/memftprt` when running directly from a checkout.

## Capture a server after it settles

```sh
memftprt --output-dir captures --stable-window 5 --timeout 120 -- \
  ./server --port 8080
```

The server remains running after capture. The result directory is created when
needed.

To produce every supported report in one capture:

```sh
memftprt --format all --output-dir captures -- ./server --port 8080
```

Open `memory_report_PID.html` for an annotated human view, consume
`memory_report_PID.json` in automation, or use the TSV with shell tools.

## Capture a continuously busy worker

```sh
memftprt --output-dir captures --delay 10 -- ./worker
```

Fixed-delay mode is appropriate when the application is expected to remain
busy and a controlled time point is more meaningful than quiescence.

## Attach to an existing PID

```sh
memftprt --output-dir captures --pid "$(pgrep -n my-service)"
```

Launch mode is preferred for daemonizing programs because descendants already
reparented before attachment cannot be inferred safely from PPID relationships.

## Capture an NMT-enabled JVM

```sh
memftprt --enable-jvm-nmt --output-dir captures -- \
  java -Xms256m -Xmx1g -jar app.jar
```

This enables summary NMT without forcing GC. If the JVM is started separately,
enable NMT at startup and then attach:

```sh
java -XX:NativeMemoryTracking=summary -jar app.jar &
java_pid=$!
memftprt --output-dir captures --pid "$java_pid"
```

## Require a delegated cgroup

```sh
memftprt --cgroup /sys/fs/cgroup/my-app --output-dir captures -- ./server
```

The supplied cgroup must already exist, be empty, be thawed, and have writable
`cgroup.procs` and `cgroup.freeze` controls.

## Quiet automation

```sh
if memftprt --quiet --output-dir captures --pid "$pid"; then
  ./examples/read-total.sh captures/process_tree_memory_*.tsv
fi
```

Successful tool output is suppressed, but target-process output is not
redirected.

## Reusable command wrapper

`capture-command.sh` accepts any command and uses environment variables for the
most common policy:

```sh
MEMFTPRT_OUTPUT_DIR=./captures \
MEMFTPRT_FORMAT=all \
MEMFTPRT_STABLE_WINDOW=5 \
MEMFTPRT_TIMEOUT=120 \
  ./examples/capture-command.sh ./server --port 8080
```

## Read the aggregate row

```sh
./examples/read-total.sh captures/process_tree_memory_*.tsv
```

For the meaning of the values and why they differ, read
[Understanding the output](../docs/UNDERSTANDING_OUTPUT.md).
