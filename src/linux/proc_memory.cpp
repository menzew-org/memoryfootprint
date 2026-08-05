/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Linux /proc memory-accounting backend implementation.
 */

#include "proc_memory.h"

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <string>

namespace memftprt::linux_procfs {
namespace {

void add_smaps_field(SmapsMetrics* metrics, const std::string& key, unsigned long value) {
    if (key == "Size") {
        metrics->size_kb += value;
    } else if (key == "Rss") {
        metrics->rss_kb += value;
        metrics->rss_available = true;
    } else if (key == "Pss") {
        metrics->pss_kb += value;
        metrics->pss_available = true;
    } else if (key == "Pss_Anon") {
        metrics->pss_anon_kb += value;
        metrics->pss_anon_available = true;
    } else if (key == "Pss_File") {
        metrics->pss_file_kb += value;
        metrics->pss_file_available = true;
    } else if (key == "Pss_Shmem") {
        metrics->pss_shmem_kb += value;
        metrics->pss_shmem_available = true;
    } else if (key == "Shared_Clean") {
        metrics->shared_clean_kb += value;
        metrics->shared_clean_available = true;
    } else if (key == "Shared_Dirty") {
        metrics->shared_dirty_kb += value;
        metrics->shared_dirty_available = true;
    } else if (key == "Private_Clean") {
        metrics->private_clean_kb += value;
        metrics->private_clean_available = true;
    } else if (key == "Private_Dirty") {
        metrics->private_dirty_kb += value;
        metrics->private_dirty_available = true;
    } else if (key == "Private_Hugetlb") {
        metrics->private_hugetlb_kb += value;
        metrics->private_hugetlb_available = true;
    } else if (key == "Anonymous") {
        metrics->anonymous_kb += value;
    } else if (key == "Swap") {
        metrics->swap_kb += value;
        metrics->swap_available = true;
    } else if (key == "SwapPss") {
        metrics->swap_pss_kb += value;
        metrics->swap_pss_available = true;
    }
}

bool read_smaps_file(const std::string& path, SmapsMetrics* metrics) {
    std::ifstream input(path);
    if (!input) return false;

    std::string line;
    while (std::getline(input, line)) {
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) continue;

        const std::string key = line.substr(0, colon);
        const char* value_text = line.c_str() + colon + 1;
        char* end = nullptr;
        errno = 0;
        const unsigned long value = std::strtoul(value_text, &end, 10);
        if (errno == 0 && end != value_text) add_smaps_field(metrics, key, value);
    }
    return !input.bad() && metrics->rss_available && metrics->pss_available;
}

}  // namespace

bool read_smaps_metrics(pid_t pid, SmapsMetrics* metrics, std::string* reason) {
    const std::string proc_root = "/proc/" + std::to_string(pid) + "/";
    *metrics = SmapsMetrics {};
    if (read_smaps_file(proc_root + "smaps_rollup", metrics)) return true;

    *metrics = SmapsMetrics {};
    if (read_smaps_file(proc_root + "smaps", metrics)) return true;

    if (reason != nullptr) {
        *reason = "could not read complete smaps data for process " + std::to_string(pid);
    }
    return false;
}

bool uss_available(const SmapsMetrics& metrics) {
    return metrics.private_clean_available && metrics.private_dirty_available &&
           metrics.private_hugetlb_available;
}

unsigned long uss_kb(const SmapsMetrics& metrics) {
    return metrics.private_clean_kb + metrics.private_dirty_kb + metrics.private_hugetlb_kb;
}

void merge_smaps_metrics(SmapsMetrics* total, const SmapsMetrics& memory, bool first) {
    total->size_kb += memory.size_kb;
    total->rss_kb += memory.rss_kb;
    total->pss_kb += memory.pss_kb;
    total->pss_anon_kb += memory.pss_anon_kb;
    total->pss_file_kb += memory.pss_file_kb;
    total->pss_shmem_kb += memory.pss_shmem_kb;
    total->shared_clean_kb += memory.shared_clean_kb;
    total->shared_dirty_kb += memory.shared_dirty_kb;
    total->private_clean_kb += memory.private_clean_kb;
    total->private_dirty_kb += memory.private_dirty_kb;
    total->private_hugetlb_kb += memory.private_hugetlb_kb;
    total->anonymous_kb += memory.anonymous_kb;
    total->swap_kb += memory.swap_kb;
    total->swap_pss_kb += memory.swap_pss_kb;

    // A process-tree total must not imply complete coverage when even one
    // process lacked a kernel field. Preserve that distinction for JSON nulls
    // and blank TSV cells.
#define MERGE_AVAILABILITY(field) \
    total->field = first ? memory.field : total->field && memory.field
    MERGE_AVAILABILITY(rss_available);
    MERGE_AVAILABILITY(pss_available);
    MERGE_AVAILABILITY(pss_anon_available);
    MERGE_AVAILABILITY(pss_file_available);
    MERGE_AVAILABILITY(pss_shmem_available);
    MERGE_AVAILABILITY(shared_clean_available);
    MERGE_AVAILABILITY(shared_dirty_available);
    MERGE_AVAILABILITY(private_clean_available);
    MERGE_AVAILABILITY(private_dirty_available);
    MERGE_AVAILABILITY(private_hugetlb_available);
    MERGE_AVAILABILITY(swap_available);
    MERGE_AVAILABILITY(swap_pss_available);
#undef MERGE_AVAILABILITY
}

}  // namespace memftprt::linux_procfs
