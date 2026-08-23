/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Platform-neutral memory report model and serialization interface.
 * Missing kernel metrics remain explicit instead of being converted to zero.
 */
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace memftprt {

struct OptionalKb {
    // Availability is separate from value because an unavailable measurement
    // and a measured zero have fundamentally different meanings.
    unsigned long value = 0;
    bool available = false;
};

struct OptionalBool {
    bool value = false;
    bool available = false;
};

// Common accounting fields shared by process, mapping, category, and total
// rows. New values are appended to the stable process TSV contract.
struct MemoryValues {
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
    OptionalKb anonymous_kb;
    OptionalKb referenced_kb;
    OptionalKb lazy_free_kb;
    OptionalKb locked_kb;
    OptionalKb anon_huge_pages_kb;
    OptionalKb shmem_pmd_mapped_kb;
    OptionalKb file_pmd_mapped_kb;
    OptionalKb shared_hugetlb_kb;
    OptionalKb private_hugetlb_kb;
    OptionalKb swap_kb;
    OptionalKb swap_pss_kb;
};

struct ProcessMemory : MemoryValues {
    std::int64_t pid = 0;
    std::int64_t ppid = 0;
    std::string command;
};

struct MemoryMapping : MemoryValues {
    std::int64_t pid = 0;
    std::uint64_t start_address = 0;
    std::uint64_t end_address = 0;
    std::uint64_t offset = 0;
    std::uint64_t inode = 0;
    std::string permissions;
    std::string device;
    std::string pathname;
    std::string category;
    OptionalKb kernel_page_size_kb;
    OptionalKb mmu_page_size_kb;
    OptionalBool thp_eligible;
    std::string vm_flags;
};

struct MappingCategory : MemoryValues {
    std::string category;
    std::size_t mapping_count = 0;
};

struct WorkingSetInfo {
    unsigned requested_window_seconds = 0;
    unsigned long long actual_window_ns = 0;
    unsigned long long started_timestamp_ns = 0;
    unsigned long long ended_timestamp_ns = 0;
    std::string validation_scope = "boundary_snapshots";
    bool process_identity_matches_at_boundaries = false;
    bool mapping_topology_matches_at_boundaries = false;
};

struct MemoryReport {
    // Increment only when the JSON document contract changes incompatibly.
    unsigned schema_version = 1;
    std::int64_t root_pid = 0;
    std::int64_t launcher_pid = 0;
    unsigned long long timestamp_ns = 0;
    std::string freeze_backend;
    std::string cgroup_path;
    std::string pidfd_status = "not_applicable";
    std::string reference_scope = "kernel_current";
    bool mappings_captured = false;
    std::map<std::int64_t, std::string> smaps_sources;
    std::optional<WorkingSetInfo> working_set;
    std::map<std::int64_t, std::string> runtime_status;
    std::vector<ProcessMemory> processes;
    ProcessMemory total;
    std::vector<MemoryMapping> mappings;
    std::vector<MappingCategory> mapping_categories;
};

std::string render_tsv(const MemoryReport& report);
std::string render_json(const MemoryReport& report);
std::string render_html(const MemoryReport& report);
std::string render_mappings_tsv(const MemoryReport& report);
std::string render_mapping_categories_tsv(const MemoryReport& report);

// Normalize an untrusted byte string to UTF-8 and flatten TSV delimiters.
// Capture metadata and runtime adapters share the report serializer contract.
std::string sanitize_tsv_field(const std::string& value);

}  // namespace memftprt
