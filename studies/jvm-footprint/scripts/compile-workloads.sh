#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Compile once with OpenJDK 8 so identical Java 8 class files run on all VMs.

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
study_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
repo_dir=$(CDPATH= cd -- "$study_dir/../.." && pwd)
cache_dir=${JVM_STUDY_CACHE:-"$repo_dir/.study-cache/jvm-footprint"}
jdk8="$cache_dir/jdks/8"
classes="$cache_dir/workloads/classes"

if [ ! -x "$jdk8/bin/javac" ]; then
    echo "OpenJDK 8 is missing; run scripts/fetch-openjdk.sh 8 first" >&2
    exit 1
fi

mkdir -p "$classes"
"$jdk8/bin/javac" -source 8 -target 8 -d "$classes" \
    "$study_dir/src/StudySupport.java" \
    "$study_dir/src/HelloIdle.java" \
    "$study_dir/src/RuntimeExercise.java"

"$jdk8/bin/javap" -verbose -classpath "$classes" HelloIdle \
    | sed -n '/major version/p'
echo "Workloads compiled into $classes"
