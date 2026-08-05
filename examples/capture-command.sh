#!/usr/bin/env bash
set -euo pipefail

if [[ $# -eq 0 ]]; then
    echo "usage: $0 command [args...]" >&2
    exit 2
fi

binary="${MEMFTPRT_BIN:-memftprt}"
output_dir="${MEMFTPRT_OUTPUT_DIR:-./captures}"
stable_window="${MEMFTPRT_STABLE_WINDOW:-3}"
timeout="${MEMFTPRT_TIMEOUT:-300}"

exec "$binary" \
    --output-dir "$output_dir" \
    --stable-window "$stable_window" \
    --timeout "$timeout" \
    -- "$@"
