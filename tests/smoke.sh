#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
binary="$root_dir/build/memftprt"
quiescence_target="$root_dir/build/quiescence_target"
signal_target="$root_dir/build/signal_target"
lifecycle_target="$root_dir/build/lifecycle_target"

bash -n "$root_dir/examples/capture-command.sh"
bash -n "$root_dir/examples/read-total.sh"

tmp_dir="$(mktemp -d)"
trap 'rm -rf "$tmp_dir"' EXIT

terminate_tree() {
    local tree_file="$1"
    while IFS=$'\t' read -r pid _; do
        if [[ $pid =~ ^[0-9]+$ ]]; then
            kill -TERM "$pid" 2>/dev/null || true
        fi
    done < "$tree_file"
}

set +e
"$binary" >"$tmp_dir/noargs.out" 2>&1
status=$?
set -e
if [[ $status -ne 2 ]]; then
    echo "expected no-argument exit 2, got $status" >&2
    exit 1
fi
grep -q "a command or --pid PID is required" "$tmp_dir/noargs.out"
grep -q "Try 'memftprt --help'" "$tmp_dir/noargs.out"

"$binary" --help >"$tmp_dir/help.out"
grep -q "consistent process-tree memory capture" "$tmp_dir/help.out"
grep -q -- "--output-dir DIR" "$tmp_dir/help.out"
grep -q -- "--format FORMAT" "$tmp_dir/help.out"
"$binary" --version >"$tmp_dir/version.out"
grep -Eq '^memftprt [0-9]+\.[0-9]+-reconstructed$' "$tmp_dir/version.out"

set +e
"$binary" --cgroup /tmp -t 1 /bin/sleep 2 >"$tmp_dir/bad-cgroup.out" 2>&1
status=$?
set -e
if [[ $status -ne 254 ]]; then
    echo "expected invalid cgroup exit 254, got $status" >&2
    exit 1
fi
grep -q "launch cgroup must be an existing directory below /sys/fs/cgroup" \
    "$tmp_dir/bad-cgroup.out"

mkdir "$tmp_dir/timed" "$tmp_dir/quiescent" "$tmp_dir/jvm" \
    "$tmp_dir/trap" "$tmp_dir/stop" "$tmp_dir/terminate" "$tmp_dir/cgroup" \
    "$tmp_dir/exec" "$tmp_dir/zombie" "$tmp_dir/orphan" "$tmp_dir/signal-race" \
    "$tmp_dir/attach" "$tmp_dir/daemon-exit" "$tmp_dir/symlink" \
    "$tmp_dir/lock-symlink" "$tmp_dir/ux"
cd "$tmp_dir/timed"
set +e
"$binary" -t 1 /bin/sleep 3 >metrics.out 2>&1
status=$?
set -e
if [[ $status -ne 0 ]]; then
    if [[ $status -eq 254 ]] && grep -q "PTRACE_TRACEME.*Operation not permitted" metrics.out; then
        echo "ptrace integration unavailable: nested sandbox denies PTRACE_TRACEME" >&2
        if [[ ${MEMFTPRT_ALLOW_PTRACE_SKIP:-0} == 1 ]]; then
            echo "ptrace integration skipped by MEMFTPRT_ALLOW_PTRACE_SKIP=1"
            exit 0
        fi
        exit 77
    fi
    cat metrics.out >&2
    echo "timed measurement failed with status $status" >&2
    exit 1
fi
grep -q "The page size" metrics.out
grep -q "capture complete" metrics.out
grep -q "VmSize: Virtual memory size" metrics.out
test -s proccess_maps_*.txt
test -s process_ps_*.txt
test -s process_top_*.txt
test -s process_tree_memory_*.tsv
test -s capture_metadata_*.tsv
"$root_dir/examples/read-total.sh" process_tree_memory_*.tsv >total.out
grep -Eq '^PSS: +[0-9]+ KB$' total.out
grep -Eq '^USS: +[0-9]+ KB$' total.out
for artifact in proccess_maps_*.txt process_ps_*.txt process_top_*.txt \
    process_tree_memory_*.tsv capture_metadata_*.tsv; do
    test "$(stat -c %a "$artifact")" = 600
done
if compgen -G '*.tmp.*' >/dev/null; then
    echo "temporary capture artifact was not committed or cleaned up" >&2
    exit 1
fi
terminate_tree process_tree_memory_*.tsv

cd "$tmp_dir/ux"
"$binary" --quiet --verbose --format all --output-dir artifacts --delay 1 -- /bin/sleep 3 \
    >quiet.out 2>&1
test ! -s quiet.out
test -s artifacts/process_tree_memory_*.tsv
test -s artifacts/memory_report_*.json
test -s artifacts/memory_report_*.html
test -s artifacts/capture_metadata_*.tsv
test ! -e process_tree_memory_*.tsv
python3 -m json.tool artifacts/memory_report_*.json >/dev/null
grep -q '"schema_version": 1' artifacts/memory_report_*.json
grep -q '"processes":' artifacts/memory_report_*.json
grep -qi '<!doctype html>' artifacts/memory_report_*.html
grep -q 'Process-tree totals' artifacts/memory_report_*.html
grep -q 'How to read this' artifacts/memory_report_*.html
for artifact in artifacts/memory_report_*.json artifacts/memory_report_*.html; do
    test "$(stat -c %a "$artifact")" = 600
done
terminate_tree artifacts/process_tree_memory_*.tsv

cd "$tmp_dir/symlink"
printf 'SAFE\n' >victim
"$binary" --freeze signals -t 1 /bin/sleep 3 >metrics.out 2>&1 &
collector_pid=$!
target_pid=""
for _ in {1..100}; do
    target_pid="$(pgrep -P "$collector_pid" -x sleep 2>/dev/null || true)"
    [[ $target_pid =~ ^[0-9]+$ ]] && break
    sleep 0.01
done
[[ $target_pid =~ ^[0-9]+$ ]]
legacy_temp="process_tree_memory_${target_pid}.tsv.tmp.${collector_pid}"
ln -s victim "$legacy_temp"
wait "$collector_pid"
grep -qx SAFE victim
test -s "process_tree_memory_${target_pid}.tsv"
rm "$legacy_temp"
terminate_tree "process_tree_memory_${target_pid}.tsv"

cd "$tmp_dir/lock-symlink"
printf 'SAFE\n' >victim
set +e
(
    sleep 5 &
    lock_target_pid=$!
    ln -s victim ".memftprt_${lock_target_pid}.lock"
    exec "$binary" --freeze signals -t 1 -p "$lock_target_pid"
) >metrics.out 2>&1
status=$?
set -e
test "$status" -eq 254
grep -q "could not create output lock" metrics.out
grep -qx SAFE victim

cd "$tmp_dir/quiescent"
"$binary" -q 1 -m 10 -v "$quiescence_target" >metrics.out 2>&1
grep -q "quiescence reached" metrics.out
grep -q "freeze backend:" metrics.out
grep -Eq "Process tree count +2 +Processes" metrics.out
grep -q "The page size" metrics.out
grep -q "VmSize: Virtual memory size" metrics.out
test -s proccess_maps_*.txt
test -s process_ps_*.txt
test -s process_top_*.txt
test -s process_tree_memory_*.tsv
grep -Eq $'^freeze_backend\t[0-9]+\t(cgroup|signals)$' capture_metadata_*.tsv
test "$(wc -l < process_tree_memory_*.tsv)" -eq 4
terminate_tree process_tree_memory_*.tsv

cd "$tmp_dir/exec"
"$binary" --freeze signals -t 1 "$lifecycle_target" exec >metrics.out 2>&1
grep -Eq $'^[0-9]+\t[0-9]+\tsleep\t' process_tree_memory_*.tsv
terminate_tree process_tree_memory_*.tsv

cd "$tmp_dir/zombie"
"$binary" --freeze signals -t 1 "$lifecycle_target" zombie >metrics.out 2>&1
grep -Eq "Process tree count +1 +Processes" metrics.out
terminate_tree process_tree_memory_*.tsv

cd "$tmp_dir/orphan"
"$binary" --freeze signals -t 1 "$lifecycle_target" orphan "$tmp_dir/orphan.marker" \
    >metrics.out 2>&1
test -s "$tmp_dir/orphan.marker"
grep -Eq "Process tree count +2 +Processes" metrics.out
test "$(wc -l < process_tree_memory_*.tsv)" -eq 4
terminate_tree process_tree_memory_*.tsv

cd "$tmp_dir/daemon-exit"
"$binary" --freeze signals -t 1 -v "$lifecycle_target" daemon-exit \
    "$tmp_dir/daemon-exit.marker" >metrics.out 2>&1
test -s "$tmp_dir/daemon-exit.marker"
grep -q "continuing with adopted application process" metrics.out
grep -Eq "Process tree count +1 +Processes" metrics.out
grep -Eq $'^launcher_pid\t[0-9]+\t[0-9]+$' capture_metadata_*.tsv
terminate_tree process_tree_memory_*.tsv

cd "$tmp_dir/signal-race"
"$binary" --freeze signals -t 1 "$lifecycle_target" signal-storm \
    "$tmp_dir/signal-race.marker" >metrics.out 2>&1
for _ in {1..60}; do
    [[ -s "$tmp_dir/signal-race.marker" ]] && break
    sleep 0.05
done
test -s "$tmp_dir/signal-race.marker"
terminate_tree process_tree_memory_*.tsv

cd "$tmp_dir/attach"
(
    sleep 5 &
    attached_pid=$!
    exec "$binary" --freeze signals -t 1 -p "$attached_pid"
) >metrics.out 2>&1
grep -Eq "Process tree count +1 +Processes" metrics.out
terminate_tree process_tree_memory_*.tsv

cd "$tmp_dir/trap"
"$binary" --freeze signals -t 1 "$signal_target" trap "$tmp_dir/trap.marker" \
    >metrics.out 2>&1
test -s "$tmp_dir/trap.marker"
terminate_tree process_tree_memory_*.tsv

cd "$tmp_dir/stop"
"$binary" --freeze signals -q 1 -m 10 -v "$signal_target" stop "$tmp_dir/stop.marker" \
    >metrics.out 2>&1
grep -q "external job-control stop" metrics.out
stopped_pid="$(awk -F '\t' '$1 == "freeze_backend" {print $2}' capture_metadata_*.tsv)"
stopped_state="$(ps -o stat= -p "$stopped_pid" | tr -d ' ')"
[[ $stopped_state == T* ]]
kill -CONT "$stopped_pid"
for _ in {1..40}; do
    [[ -s "$tmp_dir/stop.marker" ]] && break
    sleep 0.05
done
test -s "$tmp_dir/stop.marker"
kill -TERM "$stopped_pid" 2>/dev/null || true

cd "$tmp_dir/terminate"
set +e
"$binary" --freeze signals -q 10 -m 30 /bin/sleep 20 >metrics.out 2>&1 &
tool_pid=$!
set -e
sleep 0.5
kill -TERM "$tool_pid"
set +e
wait "$tool_pid"
status=$?
set -e
test "$status" -eq 143
term_target_pid="$(awk -F '\t' '$1 == "freeze_backend" {print $2}' capture_metadata_*.tsv)"
term_target_state="$(ps -o stat= -p "$term_target_pid" | tr -d ' ')"
[[ $term_target_state != T* ]]
terminate_tree process_tree_memory_*.tsv

if command -v java >/dev/null 2>&1 && command -v javac >/dev/null 2>&1 &&
   command -v jcmd >/dev/null 2>&1; then
    javac -d "$tmp_dir/jvm" "$root_dir/tests/JvmMemoryTarget.java"
    cd "$tmp_dir/jvm"
    JAVA_TOOL_OPTIONS=-XX:NativeMemoryTracking=off \
    "$binary" --freeze signals --enable-jvm-nmt -t 1 -v -v -v -v \
        java -cp "$tmp_dir/jvm" JvmMemoryTarget >metrics.out 2>&1
    grep -q "JVM detected" metrics.out
    grep -q "captured .* NMT categories" metrics.out
    test -s jvm_nmt_*.txt
    test -s jvm_nmt_*.tsv
    grep -q $'^pid\truntime\truntime_version\ttimestamp_ns\tduration_ns\tmethod\tforced_gc\tcategory\treserved_kb\tcommitted_kb\tunit$' jvm_nmt_*.tsv
    grep -q $'^jvm_nmt\t[0-9].*\tcaptured$' capture_metadata_*.tsv
    grep -Eq $'^jvm_timestamp_ns\t[0-9]+\t[1-9][0-9]+$' capture_metadata_*.tsv
    grep -Eq $'^jvm_duration_ns\t[0-9]+\t[1-9][0-9]*$' capture_metadata_*.tsv
    grep -Eq $'^jvm_forced_gc\t[0-9]+\tfalse$' capture_metadata_*.tsv
    test "$(grep -c 'timer elapsed... stopping target' metrics.out)" -eq 2
    terminate_tree process_tree_memory_*.tsv
fi

cgroup_test_status="skipped (set MEMFTPRT_TEST_CGROUP to an empty delegated cgroup)"
if [[ -n ${MEMFTPRT_TEST_CGROUP:-} ]]; then
    cd "$tmp_dir/cgroup"
    "$binary" --cgroup "$MEMFTPRT_TEST_CGROUP" -t 1 -v /bin/sleep 10 \
        >metrics.out 2>&1
    grep -q "freeze backend: cgroup" metrics.out
    grep -Eq $'^freeze_backend\t[0-9]+\tcgroup$' capture_metadata_*.tsv
    terminate_tree process_tree_memory_*.tsv
    cgroup_test_status="passed"
elif [[ ${MEMFTPRT_REQUIRE_CGROUP_TEST:-0} == 1 ]]; then
    echo "cgroup integration required but MEMFTPRT_TEST_CGROUP is unset" >&2
    exit 1
fi

echo "smoke tests passed; cgroup integration: $cgroup_test_status"
