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

#include <string>

#include <sys/types.h>

namespace memftprt::linux_procfs {

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
    unsigned long private_hugetlb_kb = 0;
    unsigned long anonymous_kb = 0;
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
    bool private_hugetlb_available = false;
    bool swap_available = false;
    bool swap_pss_available = false;
};

// Prefer smaps_rollup, then fall back to smaps for older Linux kernels.
bool read_smaps_metrics(pid_t pid, SmapsMetrics* metrics, std::string* reason);

// A total is available only when every contributing process exposed the field.
void merge_smaps_metrics(SmapsMetrics* total, const SmapsMetrics& memory, bool first);

bool uss_available(const SmapsMetrics& metrics);
unsigned long uss_kb(const SmapsMetrics& metrics);

}  // namespace memftprt::linux_procfs
