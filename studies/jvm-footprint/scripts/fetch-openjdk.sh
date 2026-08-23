#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# Download and verify the official OpenJDK archives declared by the study.
# Archives and extracted JDKs live in the ignored study cache.

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
study_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
repo_dir=$(CDPATH= cd -- "$study_dir/../.." && pwd)
manifest=${JVM_STUDY_MANIFEST:-"$study_dir/jdk-manifest.tsv"}
cache_dir=${JVM_STUDY_CACHE:-"$repo_dir/.study-cache/jvm-footprint"}
downloads_dir="$cache_dir/downloads"
jdks_dir="$cache_dir/jdks"
requested_versions=${*:-}

usage() {
    cat <<'EOF'
Usage: fetch-openjdk.sh [VERSION ...]

Download checksum-pinned official OpenJDK Linux x86-64 archives. With no
versions, downloads every version in jdk-manifest.tsv. Override the cache with
JVM_STUDY_CACHE and the manifest with JVM_STUDY_MANIFEST.
EOF
}

if [ "${1:-}" = "--help" ] || [ "${1:-}" = "-h" ]; then
    usage
    exit 0
fi

command -v curl >/dev/null 2>&1 || { echo "curl is required" >&2; exit 1; }
command -v sha256sum >/dev/null 2>&1 || { echo "sha256sum is required" >&2; exit 1; }
command -v tar >/dev/null 2>&1 || { echo "tar is required" >&2; exit 1; }

mkdir -p "$downloads_dir" "$jdks_dir"

is_requested() {
    version=$1
    [ -z "$requested_versions" ] && return 0
    for requested in $requested_versions; do
        [ "$requested" = "$version" ] && return 0
    done
    return 1
}

while IFS="$(printf '\t')" read -r version build archive_name expected_sha url; do
    case "$version" in
        ''|'#'*) continue ;;
    esac
    is_requested "$version" || continue

    case "$expected_sha" in
        *[!0-9a-f]*|'') echo "invalid SHA-256 for OpenJDK $version" >&2; exit 1 ;;
    esac
    [ "${#expected_sha}" -eq 64 ] || {
        echo "invalid SHA-256 length for OpenJDK $version" >&2
        exit 1
    }

    archive="$downloads_dir/$archive_name"
    destination="$jdks_dir/$version"
    partial="$archive.part"
    temporary="$jdks_dir/.$version.extracting.$$"

    if [ ! -f "$archive" ] || ! printf '%s  %s\n' "$expected_sha" "$archive" | sha256sum -c - >/dev/null 2>&1; then
        echo "Downloading OpenJDK $version ($build)"
        curl --fail --location --retry 4 --retry-delay 2 --continue-at - \
            --output "$partial" "$url"
        printf '%s  %s\n' "$expected_sha" "$partial" | sha256sum -c -
        mv "$partial" "$archive"
    else
        echo "Verified cached OpenJDK $version archive"
    fi

    if [ -x "$destination/bin/java" ] && [ -x "$destination/bin/jcmd" ]; then
        echo "OpenJDK $version already extracted"
        continue
    fi

    if [ -e "$temporary" ]; then
        echo "stale extraction path exists: $temporary" >&2
        exit 1
    fi
    mkdir "$temporary"
    tar -xzf "$archive" --strip-components=1 -C "$temporary"
    if [ ! -x "$temporary/bin/java" ] || [ ! -x "$temporary/bin/jcmd" ]; then
        echo "OpenJDK $version archive lacks bin/java or bin/jcmd" >&2
        exit 1
    fi
    if [ -e "$destination" ]; then
        echo "incomplete destination exists: $destination" >&2
        exit 1
    fi
    mv "$temporary" "$destination"
    "$destination/bin/java" -version 2>&1 | sed -n '1,3p'
done < "$manifest"
