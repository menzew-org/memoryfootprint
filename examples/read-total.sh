#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: $0 process_tree_memory_PID.tsv" >&2
    exit 2
fi

awk -F '\t' '
    NR == 1 {
        for (column = 1; column <= NF; ++column) {
            index_by_name[$column] = column
        }
        next
    }
    $1 == "TOTAL" {
        printf "processes: %s\n", $3
        printf "virtual size: %s KB\n", $(index_by_name["vsize_kb"])
        printf "RSS:          %s KB\n", $(index_by_name["rss_kb"])
        printf "PSS:          %s KB\n", $(index_by_name["pss_kb"])
        printf "USS:          %s KB\n", $(index_by_name["uss_kb"])
        printf "swap:         %s KB\n", $(index_by_name["swap_kb"])
        found = 1
    }
    END {
        if (!found) {
            print "TOTAL row not found" > "/dev/stderr"
            exit 1
        }
    }
' "$1"
