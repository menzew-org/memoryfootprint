/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Linux /proc memory-accounting backend.
 *
 * This module deliberately exposes raw smaps concepts rather than report
 * formats. The capture orchestrator owns process-tree consistency; this
 * backend only reads and combines one frozen process's kernel accounting.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <istream>
#include <string>
#include <vector>

#include <sys/types.h>

namespace memftprt::linux_procfs {

// Detailed captures are opt-in, but they still need a hard safety boundary:
// allocation failure while the target is frozen could otherwise strand it.
constexpr std::size_t MAX_SMAPS_MAPPINGS = 100000;
constexpr std::size_t MAX_SMAPS_INPUT_BYTES = 128U * 1024U * 1024U;
constexpr std::size_t MAX_TREE_SMAPS_MAPPINGS = 200000;
constexpr std::size_t MAX_TREE_SMAPS_INPUT_BYTES = 256U * 1024U * 1024U;

using SmapsCancellationCheck = bool (*)();

// One budget is shared by every process in a frozen-tree read. The per-process
// limits above still apply independently.
struct SmapsReadBudget {
    std::size_t remaining_input_bytes = MAX_TREE_SMAPS_INPUT_BYTES;
    std::size_t remaining_mappings = MAX_TREE_SMAPS_MAPPINGS;
};

struct SmapsMetrics {
    unsigned long size_kb = 0;
    unsigned long rss_kb = 0;
    unsigned long pss_kb = 0;
    unsigned long pss_anon_kb = 0;
    unsigned long pss_file_kb = 0;
    unsigned long pss_shmem_kb = 0;
    unsigned long shared_clean_kb = 0;
    unsigned long shared_dirty_kb = 0;
    unsigned long private_clean_kb = 0;
    unsigned long private_dirty_kb = 0;
    unsigned long shared_hugetlb_kb = 0;
    unsigned long private_hugetlb_kb = 0;
    unsigned long anonymous_kb = 0;
    unsigned long referenced_kb = 0;
    unsigned long lazy_free_kb = 0;
    unsigned long locked_kb = 0;
    unsigned long anon_huge_pages_kb = 0;
    unsigned long shmem_pmd_mapped_kb = 0;
    unsigned long file_pmd_mapped_kb = 0;
    unsigned long swap_kb = 0;
    unsigned long swap_pss_kb = 0;
    bool rss_available = false;
    bool pss_available = false;
    bool pss_anon_available = false;
    bool pss_file_available = false;
    bool pss_shmem_available = false;
    bool shared_clean_available = false;
    bool shared_dirty_available = false;
    bool private_clean_available = false;
    bool private_dirty_available = false;
    bool shared_hugetlb_available = false;
    bool private_hugetlb_available = false;
    bool anonymous_available = false;
    bool referenced_available = false;
    bool lazy_free_available = false;
    bool locked_available = false;
    bool anon_huge_pages_available = false;
    bool shmem_pmd_mapped_available = false;
    bool file_pmd_mapped_available = false;
    bool swap_available = false;
    bool swap_pss_available = false;
};

// One virtual-memory area from /proc/PID/smaps. Addresses and offsets remain
// numeric internally; serializers choose a lossless hexadecimal presentation.
struct SmapsMapping {
    std::uint64_t start_address = 0;
    std::uint64_t end_address = 0;
    std::uint64_t offset = 0;
    std::uint64_t inode = 0;
    std::string permissions;
    std::string device;
    std::string pathname;
    SmapsMetrics memory;
    unsigned long kernel_page_size_kb = 0;
    unsigned long mmu_page_size_kb = 0;
    bool kernel_page_size_available = false;
    bool mmu_page_size_available = false;
    bool thp_eligible = false;
    bool thp_eligible_available = false;
    std::string vm_flags;
};

struct SmapsDetail {
    SmapsMetrics total;
    std::vector<SmapsMapping> mappings;
    // Per-mapping values come from one full smaps walk. When available, every
    // process-level aggregate comes from one later smaps_rollup walk so PSS and
    // its backing split are internally coherent.
    bool process_totals_from_rollup = false;
};

// Prefer smaps_rollup, then fall back to smaps for older Linux kernels.
bool read_smaps_metrics(pid_t pid, SmapsMetrics* metrics, std::string* reason,
                        std::string* source = nullptr);

// Parse full smaps in one stream so every mapping belongs to one kernel walk.
// The stream overload keeps parser tests independent of a live /proc tree.
bool parse_smaps_detail(std::istream& input, SmapsDetail* detail, std::string* reason,
                        SmapsReadBudget* budget = nullptr,
                        SmapsCancellationCheck cancelled = nullptr);
bool read_smaps_detail(pid_t pid, SmapsDetail* detail, std::string* reason,
                       SmapsReadBudget* budget = nullptr,
                       SmapsCancellationCheck cancelled = nullptr);

// Categories are intentionally generic. For example, executable anonymous
// memory can contain JIT code but is not attributed to a specific runtime.
std::string classify_mapping(const SmapsMapping& mapping,
                             const std::string& executable_path);

// A total is available only when every contributing process exposed the field.
void merge_smaps_metrics(SmapsMetrics* total, const SmapsMetrics& memory, bool first);

bool uss_available(const SmapsMetrics& metrics);
unsigned long uss_kb(const SmapsMetrics& metrics);

}  // namespace memftprt::linux_procfs
