/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Platform-neutral memory report model and serialization interface.
 * Missing kernel metrics remain explicit instead of being converted to zero.
 */
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace memftprt {

struct OptionalKb {
    // Availability is separate from value because an unavailable measurement
    // and a measured zero have fundamentally different meanings.
    unsigned long value = 0;
    bool available = false;
};

struct ProcessMemory {
    std::int64_t pid = 0;
    std::int64_t ppid = 0;
    std::string command;
    unsigned long vsize_kb = 0;
    OptionalKb rss_kb;
    OptionalKb pss_kb;
    OptionalKb uss_kb;
    OptionalKb pss_anon_kb;
    OptionalKb pss_file_kb;
    OptionalKb pss_shmem_kb;
    OptionalKb shared_clean_kb;
    OptionalKb shared_dirty_kb;
    OptionalKb private_clean_kb;
    OptionalKb private_dirty_kb;
    OptionalKb swap_kb;
    OptionalKb swap_pss_kb;
};

struct MemoryReport {
    // Increment only when the JSON document contract changes incompatibly.
    unsigned schema_version = 1;
    std::int64_t root_pid = 0;
    std::int64_t launcher_pid = 0;
    unsigned long long timestamp_ns = 0;
    std::string freeze_backend;
    std::string cgroup_path;
    std::map<std::int64_t, std::string> runtime_status;
    std::vector<ProcessMemory> processes;
    ProcessMemory total;
};

std::string render_tsv(const MemoryReport& report);
std::string render_json(const MemoryReport& report);
std::string render_html(const MemoryReport& report);

}  // namespace memftprt
