/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Linux /proc memory-accounting and full smaps parsing.
 */

#include "proc_memory.h"

#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace memftprt::linux_procfs {
namespace {

void add_smaps_field(SmapsMetrics* metrics, const std::string& key, unsigned long value) {
#define ADD_FIELD(name, field)              \
    if (key == (name)) {                    \
        metrics->field##_kb += value;       \
        metrics->field##_available = true;  \
        return;                             \
    }
    if (key == "Size") {
        metrics->size_kb += value;
        return;
    }
    ADD_FIELD("Rss", rss)
    ADD_FIELD("Pss", pss)
    ADD_FIELD("Pss_Anon", pss_anon)
    ADD_FIELD("Pss_File", pss_file)
    ADD_FIELD("Pss_Shmem", pss_shmem)
    ADD_FIELD("Shared_Clean", shared_clean)
    ADD_FIELD("Shared_Dirty", shared_dirty)
    ADD_FIELD("Private_Clean", private_clean)
    ADD_FIELD("Private_Dirty", private_dirty)
    ADD_FIELD("Shared_Hugetlb", shared_hugetlb)
    ADD_FIELD("Private_Hugetlb", private_hugetlb)
    ADD_FIELD("Anonymous", anonymous)
    ADD_FIELD("Referenced", referenced)
    ADD_FIELD("LazyFree", lazy_free)
    ADD_FIELD("Locked", locked)
    ADD_FIELD("AnonHugePages", anon_huge_pages)
    ADD_FIELD("ShmemPmdMapped", shmem_pmd_mapped)
    ADD_FIELD("FilePmdMapped", file_pmd_mapped)
    ADD_FIELD("Swap", swap)
    ADD_FIELD("SwapPss", swap_pss)
#undef ADD_FIELD
}

bool parse_unsigned(const char* text, int base, std::uint64_t* value) {
    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(text, &end, base);
    if (errno != 0 || end == text || *end != '\0') return false;
    *value = static_cast<std::uint64_t>(parsed);
    return true;
}

bool parse_mapping_header(const std::string& line, SmapsMapping* mapping) {
    std::istringstream fields(line);
    std::string range;
    std::string offset;
    std::string inode;
    if (!(fields >> range >> mapping->permissions >> offset >> mapping->device >> inode)) {
        return false;
    }
    const std::size_t dash = range.find('-');
    if (dash == std::string::npos || dash == 0 || dash + 1 == range.size()) return false;
    const std::string start = range.substr(0, dash);
    const std::string end = range.substr(dash + 1);
    if (!parse_unsigned(start.c_str(), 16, &mapping->start_address) ||
        !parse_unsigned(end.c_str(), 16, &mapping->end_address) ||
        !parse_unsigned(offset.c_str(), 16, &mapping->offset) ||
        !parse_unsigned(inode.c_str(), 10, &mapping->inode) ||
        mapping->start_address >= mapping->end_address) {
        return false;
    }
    std::getline(fields, mapping->pathname);
    const std::size_t first = mapping->pathname.find_first_not_of(" \t");
    mapping->pathname = first == std::string::npos ? "" : mapping->pathname.substr(first);
    return true;
}

bool parse_kb_line(const std::string& line, std::string* key, unsigned long* value) {
    const std::size_t colon = line.find(':');
    if (colon == std::string::npos) return false;
    *key = line.substr(0, colon);
    const char* value_text = line.c_str() + colon + 1;
    errno = 0;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value_text, &end, 10);
    if (errno != 0 || end == value_text) return false;
    *value = parsed;
    return true;
}

bool read_smaps_file(const std::string& path, SmapsMetrics* metrics) {
    std::ifstream input(path);
    if (!input) return false;

    std::string line;
    while (std::getline(input, line)) {
        std::string key;
        unsigned long value = 0;
        if (parse_kb_line(line, &key, &value)) add_smaps_field(metrics, key, value);
    }
    return !input.bad() && metrics->rss_available && metrics->pss_available;
}

bool starts_with(const std::string& value, const char* prefix) {
    return value.rfind(prefix, 0) == 0;
}

std::string without_deleted_suffix(const std::string& path) {
    constexpr const char* suffix = " (deleted)";
    const std::size_t length = std::char_traits<char>::length(suffix);
    if (path.size() >= length && path.compare(path.size() - length, length, suffix) == 0) {
        return path.substr(0, path.size() - length);
    }
    return path;
}

}  // namespace

bool read_smaps_metrics(pid_t pid, SmapsMetrics* metrics, std::string* reason,
                        std::string* source) {
    const std::string proc_root = "/proc/" + std::to_string(pid) + "/";
    *metrics = SmapsMetrics {};
    if (read_smaps_file(proc_root + "smaps_rollup", metrics)) {
        if (source != nullptr) *source = "smaps_rollup";
        return true;
    }

    *metrics = SmapsMetrics {};
    if (read_smaps_file(proc_root + "smaps", metrics)) {
        if (source != nullptr) *source = "smaps";
        return true;
    }

    if (reason != nullptr) {
        *reason = "could not read complete smaps data for process " + std::to_string(pid);
    }
    return false;
}

bool parse_smaps_detail(std::istream& input, SmapsDetail* detail, std::string* reason,
                        SmapsReadBudget* budget, SmapsCancellationCheck cancelled) {
    *detail = SmapsDetail {};
    SmapsMapping* current = nullptr;
    std::string line;
    std::size_t input_bytes = 0;
    while (std::getline(input, line)) {
        if (cancelled != nullptr && cancelled()) {
            if (reason != nullptr) *reason = "smaps capture was interrupted";
            return false;
        }
        const std::size_t line_bytes = line.size() + 1;
        if (line_bytes > MAX_SMAPS_INPUT_BYTES - input_bytes) {
            if (reason != nullptr) *reason = "smaps exceeds the 128 MiB safety limit";
            return false;
        }
        if (budget != nullptr && line_bytes > budget->remaining_input_bytes) {
            if (reason != nullptr) *reason = "process-tree smaps exceeds the 256 MiB safety limit";
            return false;
        }
        input_bytes += line_bytes;
        if (budget != nullptr) budget->remaining_input_bytes -= line_bytes;
        SmapsMapping candidate;
        if (parse_mapping_header(line, &candidate)) {
            if (detail->mappings.size() >= MAX_SMAPS_MAPPINGS) {
                if (reason != nullptr) {
                    *reason = "smaps exceeds the 100000-mapping safety limit";
                }
                return false;
            }
            if (budget != nullptr && budget->remaining_mappings == 0) {
                if (reason != nullptr) {
                    *reason = "process-tree smaps exceeds the 200000-mapping safety limit";
                }
                return false;
            }
            detail->mappings.push_back(std::move(candidate));
            if (budget != nullptr) --budget->remaining_mappings;
            current = &detail->mappings.back();
            continue;
        }
        if (current == nullptr) continue;
        if (starts_with(line, "VmFlags:")) {
            const std::size_t first = line.find_first_not_of(" \t", 8);
            current->vm_flags = first == std::string::npos ? "" : line.substr(first);
            continue;
        }

        std::string key;
        unsigned long value = 0;
        if (!parse_kb_line(line, &key, &value)) continue;
        if (key == "KernelPageSize") {
            current->kernel_page_size_kb = value;
            current->kernel_page_size_available = true;
        } else if (key == "MMUPageSize") {
            current->mmu_page_size_kb = value;
            current->mmu_page_size_available = true;
        } else if (key == "THPeligible") {
            current->thp_eligible = value != 0;
            current->thp_eligible_available = true;
        } else {
            add_smaps_field(&current->memory, key, value);
        }
    }
    if (input.bad()) {
        if (reason != nullptr) *reason = "I/O error while reading smaps";
        return false;
    }
    if (detail->mappings.empty()) {
        if (reason != nullptr) *reason = "smaps did not contain any mappings";
        return false;
    }
    for (std::size_t index = 0; index < detail->mappings.size(); ++index) {
        const SmapsMapping& mapping = detail->mappings[index];
        if (!mapping.memory.rss_available || !mapping.memory.pss_available) {
            if (reason != nullptr) {
                *reason = "smaps mapping " + std::to_string(index) +
                          " did not contain complete RSS/PSS accounting";
            }
            return false;
        }
        merge_smaps_metrics(&detail->total, mapping.memory, index == 0);
    }
    return true;
}

bool read_smaps_detail(pid_t pid, SmapsDetail* detail, std::string* reason,
                       SmapsReadBudget* budget, SmapsCancellationCheck cancelled) {
    std::ifstream input("/proc/" + std::to_string(pid) + "/smaps");
    if (!input) {
        if (reason != nullptr) {
            *reason = "could not open full smaps data for process " + std::to_string(pid) +
                      ": " + std::strerror(errno);
        }
        return false;
    }
    if (!parse_smaps_detail(input, detail, reason, budget, cancelled)) {
        if (reason != nullptr && reason->find(std::to_string(pid)) == std::string::npos) {
            *reason += " for process " + std::to_string(pid);
        }
        return false;
    }

    // Pss_Anon/File/Shmem are computed per physical folio by the kernel and
    // cannot be reconstructed from a VMA pathname. Use the complete rollup for
    // process-level values so total PSS and its split come from one kernel walk
    // instead of combining independently sampled aggregates.
    SmapsMetrics rollup;
    const std::string rollup_path = "/proc/" + std::to_string(pid) + "/smaps_rollup";
    if (read_smaps_file(rollup_path, &rollup)) {
        rollup.size_kb = detail->total.size_kb;
        detail->total = rollup;
        detail->process_totals_from_rollup = true;
    }
    return true;
}

std::string classify_mapping(const SmapsMapping& mapping,
                             const std::string& executable_path) {
    const std::string& path = mapping.pathname;
    if (path == "[heap]") return "heap";
    if (path == "[stack]" || starts_with(path, "[stack:")) return "stack";
    if (path == "[vdso]" || path == "[vvar]" || path == "[vsyscall]" ||
        starts_with(path, "[vvar_")) {
        return "kernel_special";
    }
    if (starts_with(path, "/dev/shm/") || starts_with(path, "/SYSV") ||
        starts_with(path, "memfd:") || starts_with(path, "/memfd:") ||
        starts_with(path, "[anon_shmem")) {
        return "shared_memory";
    }
    if (!executable_path.empty() &&
        without_deleted_suffix(path) == without_deleted_suffix(executable_path)) {
        return "main_executable";
    }
    const bool executable = mapping.permissions.find('x') != std::string::npos;
    const bool anonymous_name = path.empty() || starts_with(path, "[anon:");
    if (executable && anonymous_name) return "anonymous_executable";
    if (path.size() >= 10 && path.compare(path.size() - 10, 10, " (deleted)") == 0) {
        return "deleted_file";
    }
    if (!path.empty() && path[0] == '/') {
        const std::string normalized = without_deleted_suffix(path);
        const std::size_t slash = normalized.find_last_of('/');
        const std::string filename =
            slash == std::string::npos ? normalized : normalized.substr(slash + 1);
        const std::size_t suffix = filename.rfind(".so");
        const bool unversioned = suffix != std::string::npos && suffix + 3 == filename.size();
        const bool versioned = suffix != std::string::npos && suffix + 4 < filename.size() &&
                               filename[suffix + 3] == '.' &&
                               std::isdigit(static_cast<unsigned char>(filename[suffix + 4]));
        if (unversioned || versioned) return "shared_library";
    }
    if (starts_with(path, "/dev/")) return "device";
    if (!path.empty() && path[0] == '/') return "file";
    if (anonymous_name) return "anonymous";
    return "other";
}

bool uss_available(const SmapsMetrics& metrics) {
    return metrics.private_clean_available && metrics.private_dirty_available &&
           metrics.private_hugetlb_available;
}

unsigned long uss_kb(const SmapsMetrics& metrics) {
    return metrics.private_clean_kb + metrics.private_dirty_kb + metrics.private_hugetlb_kb;
}

void merge_smaps_metrics(SmapsMetrics* total, const SmapsMetrics& memory, bool first) {
#define ADD_VALUE(field) total->field##_kb += memory.field##_kb
    ADD_VALUE(size);
    ADD_VALUE(rss);
    ADD_VALUE(pss);
    ADD_VALUE(pss_anon);
    ADD_VALUE(pss_file);
    ADD_VALUE(pss_shmem);
    ADD_VALUE(shared_clean);
    ADD_VALUE(shared_dirty);
    ADD_VALUE(private_clean);
    ADD_VALUE(private_dirty);
    ADD_VALUE(shared_hugetlb);
    ADD_VALUE(private_hugetlb);
    ADD_VALUE(anonymous);
    ADD_VALUE(referenced);
    ADD_VALUE(lazy_free);
    ADD_VALUE(locked);
    ADD_VALUE(anon_huge_pages);
    ADD_VALUE(shmem_pmd_mapped);
    ADD_VALUE(file_pmd_mapped);
    ADD_VALUE(swap);
    ADD_VALUE(swap_pss);
#undef ADD_VALUE

    // A process-tree total must not imply complete coverage when even one
    // process lacked a kernel field.
#define MERGE_AVAILABILITY(field) \
    total->field##_available = first ? memory.field##_available \
                                     : total->field##_available && memory.field##_available
    MERGE_AVAILABILITY(rss);
    MERGE_AVAILABILITY(pss);
    MERGE_AVAILABILITY(pss_anon);
    MERGE_AVAILABILITY(pss_file);
    MERGE_AVAILABILITY(pss_shmem);
    MERGE_AVAILABILITY(shared_clean);
    MERGE_AVAILABILITY(shared_dirty);
    MERGE_AVAILABILITY(private_clean);
    MERGE_AVAILABILITY(private_dirty);
    MERGE_AVAILABILITY(shared_hugetlb);
    MERGE_AVAILABILITY(private_hugetlb);
    MERGE_AVAILABILITY(anonymous);
    MERGE_AVAILABILITY(referenced);
    MERGE_AVAILABILITY(lazy_free);
    MERGE_AVAILABILITY(locked);
    MERGE_AVAILABILITY(anon_huge_pages);
    MERGE_AVAILABILITY(shmem_pmd_mapped);
    MERGE_AVAILABILITY(file_pmd_mapped);
    MERGE_AVAILABILITY(swap);
    MERGE_AVAILABILITY(swap_pss);
#undef MERGE_AVAILABILITY
}

}  // namespace memftprt::linux_procfs
