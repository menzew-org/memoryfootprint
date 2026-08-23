/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fast tests for platform-neutral serializers and runtime parsing.
 */

#include "../src/artifact_store.h"
#include "../src/memory_report.h"
#include "../src/linux/cgroup_utils.h"
#include "../src/linux/proc_memory.h"
#include "../src/runtime/hotspot_nmt.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

namespace {

bool require(bool condition, const char* message) {
    if (!condition) std::cerr << "module_test: " << message << '\n';
    return condition;
}

bool always_cancel() {
    return true;
}

bool write_fixture(const std::filesystem::path& path, const std::string& contents) {
    std::ofstream output(path);
    output << contents;
    return static_cast<bool>(output);
}

}  // namespace

int main() {
    memftprt::MemoryReport report;
    report.root_pid = 42;
    report.freeze_backend = "signals";
    memftprt::ProcessMemory process;
    process.pid = 42;
    process.ppid = 1;
    process.command = "quoted\"\n<&";
    process.vsize_kb = 100;
    process.pss_kb = memftprt::OptionalKb {0, true};
    report.processes.push_back(process);
    report.total = process;
    report.smaps_sources[42] = "smaps_rollup";

    const std::string json = memftprt::render_json(report);
    if (!require(json.find("\"rss\": null") != std::string::npos,
                 "JSON must preserve unavailable metrics as null") ||
        !require(json.find("\"pss\": 0") != std::string::npos,
                 "JSON must preserve an available measured zero") ||
        !require(json.find("quoted\\\"\\n<&") != std::string::npos,
                 "JSON must escape command control characters")) {
        return 1;
    }
    if (!require(json.find("\"mapping_categories\": null") != std::string::npos,
                 "JSON must distinguish unrequested mapping detail") ||
        !require(json.find("\"reference_scope\": \"kernel_current\"") !=
                     std::string::npos,
                 "JSON must state the meaning of Referenced values")) {
        return 1;
    }

    const std::string html = memftprt::render_html(report);
    if (!require(html.find("quoted&quot;\n&lt;&amp;") != std::string::npos,
                 "HTML must escape target-provided command text") ||
        !require(html.find("quoted\"\n<&") == std::string::npos,
                 "HTML must not contain raw target markup")) {
        return 1;
    }

    const std::string tsv = memftprt::render_tsv(report);
    if (!require(tsv.find("quoted\" <&") != std::string::npos,
                 "TSV must flatten command newlines")) {
        return 1;
    }

    std::string invalid_command = "invalid-";
    invalid_command.push_back(static_cast<char>(0xff));
    report.processes[0].command = invalid_command;
    report.total.command = invalid_command;
    const std::string replacement = "\xef\xbf\xbd";
    const std::string utf8_json = memftprt::render_json(report);
    const std::string utf8_html = memftprt::render_html(report);
    const std::string utf8_tsv = memftprt::render_tsv(report);
    if (!require(utf8_json.find(static_cast<char>(0xff)) == std::string::npos &&
                     utf8_json.find(replacement) != std::string::npos,
                 "JSON must replace invalid UTF-8 from procfs") ||
        !require(utf8_html.find(static_cast<char>(0xff)) == std::string::npos &&
                     utf8_html.find(replacement) != std::string::npos,
                 "HTML must replace invalid UTF-8 from procfs") ||
        !require(utf8_tsv.find(static_cast<char>(0xff)) == std::string::npos &&
                     utf8_tsv.find(replacement) != std::string::npos,
                 "TSV must replace invalid UTF-8 from procfs")) {
        return 1;
    }
    report.processes[0].command = process.command;
    report.total.command = process.command;

    const std::string smaps_text =
        "00400000-00402000 r-xp 00000000 08:01 12 /opt/test app\n"
        "Size:                  8 kB\n"
        "KernelPageSize:        4 kB\n"
        "MMUPageSize:           4 kB\n"
        "Rss:                   8 kB\n"
        "Pss:                   8 kB\n"
        "Shared_Clean:          0 kB\n"
        "Shared_Dirty:          0 kB\n"
        "Private_Clean:         4 kB\n"
        "Private_Dirty:         4 kB\n"
        "Shared_Hugetlb:        0 kB\n"
        "Private_Hugetlb:       0 kB\n"
        "Anonymous:             4 kB\n"
        "Referenced:            4 kB\n"
        "LazyFree:              0 kB\n"
        "AnonHugePages:         0 kB\n"
        "ShmemPmdMapped:        0 kB\n"
        "FilePmdMapped:         0 kB\n"
        "Swap:                  0 kB\n"
        "SwapPss:               0 kB\n"
        "Locked:                0 kB\n"
        "THPeligible:           1\n"
        "VmFlags: rd ex mr mw me\n"
        "7f000000-7f002000 rwxp 00000000 00:00 0\n"
        "Size:                  8 kB\n"
        "KernelPageSize:        4 kB\n"
        "MMUPageSize:           4 kB\n"
        "Rss:                   4 kB\n"
        "Pss:                   4 kB\n"
        "Shared_Clean:          0 kB\n"
        "Shared_Dirty:          0 kB\n"
        "Private_Clean:         0 kB\n"
        "Private_Dirty:         4 kB\n"
        "Shared_Hugetlb:        0 kB\n"
        "Private_Hugetlb:       0 kB\n"
        "Anonymous:             4 kB\n"
        "Referenced:            4 kB\n"
        "LazyFree:              0 kB\n"
        "AnonHugePages:         0 kB\n"
        "ShmemPmdMapped:        0 kB\n"
        "FilePmdMapped:         0 kB\n"
        "Swap:                  0 kB\n"
        "SwapPss:               0 kB\n"
        "Locked:                0 kB\n"
        "THPeligible:           0\n"
        "UnknownFutureField:    9 kB\n"
        "VmFlags: rd wr ex mr mw me\n";
    std::istringstream smaps_input(smaps_text);
    memftprt::linux_procfs::SmapsDetail detail;
    std::string smaps_reason;
    if (!require(memftprt::linux_procfs::parse_smaps_detail(
                     smaps_input, &detail, &smaps_reason),
                 "full smaps fixture must parse") ||
        !require(detail.mappings.size() == 2, "smaps parser must retain every mapping") ||
        !require(detail.total.pss_kb == 12 && detail.total.referenced_kb == 8,
                 "smaps parser must aggregate mapping accounting") ||
        !require(!detail.total.pss_file_available && !detail.total.pss_anon_available &&
                     !detail.total.pss_shmem_available,
                 "full smaps must not guess kernel PSS backing-type totals") ||
        !require(detail.mappings[0].pathname == "/opt/test app",
                 "smaps parser must preserve paths containing spaces") ||
        !require(detail.mappings[0].thp_eligible_available &&
                     detail.mappings[0].thp_eligible,
                 "smaps parser must retain THP eligibility")) {
        return 1;
    }

    std::istringstream incomplete_smaps(
        smaps_text +
        "7f100000-7f102000 rw-p 00000000 00:00 0\n"
        "Size:                  8 kB\n");
    memftprt::linux_procfs::SmapsDetail incomplete_detail;
    std::string incomplete_reason;
    if (!require(!memftprt::linux_procfs::parse_smaps_detail(
                     incomplete_smaps, &incomplete_detail, &incomplete_reason),
                 "smaps parser must reject an incomplete final mapping") ||
        !require(incomplete_reason.find("complete RSS/PSS") != std::string::npos,
                 "incomplete smaps must explain the missing accounting")) {
        return 1;
    }
    if (!require(memftprt::linux_procfs::classify_mapping(
                     detail.mappings[0], "/opt/test app") == "main_executable",
                 "classifier must recognize the process executable") ||
        !require(memftprt::linux_procfs::classify_mapping(
                     detail.mappings[1], "/opt/test app") == "anonymous_executable",
                 "classifier must label executable anonymous memory generically")) {
        return 1;
    }

    memftprt::linux_procfs::SmapsMapping library_mapping;
    library_mapping.pathname = "/usr/lib/libexample.so.2";
    memftprt::linux_procfs::SmapsMapping socket_mapping;
    socket_mapping.pathname = "/tmp/data.socket";
    memftprt::linux_procfs::SmapsMapping source_mapping;
    source_mapping.pathname = "/opt/app/something.source";
    if (!require(memftprt::linux_procfs::classify_mapping(library_mapping, "") ==
                     "shared_library",
                 "classifier must recognize a versioned shared object") ||
        !require(memftprt::linux_procfs::classify_mapping(socket_mapping, "") == "file",
                 "classifier must not treat .socket as a shared object") ||
        !require(memftprt::linux_procfs::classify_mapping(source_mapping, "") == "file",
                 "classifier must not treat .source as a shared object")) {
        return 1;
    }

    memftprt::linux_procfs::SmapsReadBudget tiny_budget;
    tiny_budget.remaining_input_bytes = 16;
    std::istringstream budget_input(smaps_text);
    memftprt::linux_procfs::SmapsDetail budget_detail;
    std::string budget_reason;
    if (!require(!memftprt::linux_procfs::parse_smaps_detail(
                     budget_input, &budget_detail, &budget_reason, &tiny_budget),
                 "smaps parser must enforce the shared process-tree byte budget") ||
        !require(budget_reason.find("process-tree") != std::string::npos,
                 "tree-budget failures must be identifiable")) {
        return 1;
    }

    std::istringstream cancelled_input(smaps_text);
    memftprt::linux_procfs::SmapsDetail cancelled_detail;
    std::string cancelled_reason;
    if (!require(!memftprt::linux_procfs::parse_smaps_detail(
                     cancelled_input, &cancelled_detail, &cancelled_reason, nullptr,
                     always_cancel),
                 "smaps parser must stop when capture cancellation is requested") ||
        !require(cancelled_reason.find("interrupted") != std::string::npos,
                 "cancelled smaps parsing must report interruption")) {
        return 1;
    }

    memftprt::linux_procfs::SmapsDetail live_detail;
    std::string live_reason;
    if (!require(memftprt::linux_procfs::read_smaps_detail(
                     getpid(), &live_detail, &live_reason),
                 "live smaps detail for the module test must be readable")) {
        return 1;
    }
    if (live_detail.process_totals_from_rollup && live_detail.total.pss_available &&
        live_detail.total.pss_anon_available && live_detail.total.pss_file_available &&
        live_detail.total.pss_shmem_available &&
        !require(live_detail.total.pss_kb == live_detail.total.pss_anon_kb +
                                                live_detail.total.pss_file_kb +
                                                live_detail.total.pss_shmem_kb,
                 "process PSS and its backing split must use one rollup snapshot")) {
        return 1;
    }

    const std::filesystem::path cgroup_fixture =
        std::filesystem::temp_directory_path() /
        ("memftprt-cgroup-module-" + std::to_string(getpid()));
    std::error_code fixture_error;
    std::filesystem::remove_all(cgroup_fixture, fixture_error);
    std::filesystem::create_directories(cgroup_fixture / "child" / "grandchild",
                                        fixture_error);
    if (!require(!fixture_error && write_fixture(cgroup_fixture / "cgroup.procs", "101\n") &&
                     write_fixture(cgroup_fixture / "child" / "cgroup.procs", "102\n") &&
                     write_fixture(cgroup_fixture / "child" / "grandchild" / "cgroup.procs",
                                   "103\n"),
                 "cgroup test fixture must be created")) {
        return 1;
    }
    std::set<pid_t> cgroup_members;
    std::string cgroup_reason;
    if (!require(memftprt::linux_cgroup::read_subtree_processes(
                     cgroup_fixture, &cgroup_members, &cgroup_reason),
                 "readable cgroup fixture must be enumerated") ||
        !require(cgroup_members == std::set<pid_t>({101, 102, 103}),
                 "cgroup enumeration must include nested members")) {
        return 1;
    }
    std::filesystem::remove(cgroup_fixture / "child" / "grandchild" / "cgroup.procs",
                            fixture_error);
    cgroup_members.clear();
    cgroup_reason.clear();
    if (!require(!memftprt::linux_cgroup::read_subtree_processes(
                     cgroup_fixture, &cgroup_members, &cgroup_reason),
                 "missing nested membership data must fail closed")) {
        return 1;
    }
    if (!require(write_fixture(cgroup_fixture / "child" / "grandchild" / "cgroup.procs",
                               "103\n"),
                 "cgroup membership fixture must be restored")) {
        return 1;
    }
    std::filesystem::create_directory_symlink(cgroup_fixture / "child",
                                              cgroup_fixture / "linked-child", fixture_error);
    cgroup_members.clear();
    cgroup_reason.clear();
    if (!require(!fixture_error, "cgroup symlink fixture must be created") ||
        !require(!memftprt::linux_cgroup::read_subtree_processes(
                     cgroup_fixture, &cgroup_members, &cgroup_reason),
                 "symbolic links in a cgroup subtree must fail closed")) {
        return 1;
    }
    std::filesystem::remove_all(cgroup_fixture, fixture_error);

    const std::filesystem::path artifact_root =
        std::filesystem::temp_directory_path() /
        ("memftprt-artifact-module-" + std::to_string(getpid()));
    const std::filesystem::path artifact_fixture = artifact_root / "nested" / "output";
    std::filesystem::remove_all(artifact_root, fixture_error);
    memftprt::ArtifactStore store(artifact_fixture);
    std::string artifact_reason;
    if (!require(store.prepare(&artifact_reason), "artifact directory must prepare") ||
        !require(store.write_text(store.path("payload.txt"), "payload\n", &artifact_reason),
                 "artifact publication must succeed") ||
        !require(store.remove_file(store.path("payload.txt"), false, &artifact_reason),
                 "durable artifact removal must succeed") ||
        !require(store.remove_file(store.path("missing.txt"), true, &artifact_reason),
                 "missing artifact removal must be idempotent")) {
        return 1;
    }
    std::filesystem::remove_all(artifact_root, fixture_error);

    memftprt::MemoryMapping mapping;
    mapping.pid = 42;
    mapping.start_address = detail.mappings[0].start_address;
    mapping.end_address = detail.mappings[0].end_address;
    mapping.pathname = detail.mappings[0].pathname;
    mapping.category = "main_executable";
    mapping.vsize_kb = 8;
    mapping.pss_kb = memftprt::OptionalKb {8, true};
    report.mappings_captured = true;
    report.mappings.push_back(mapping);
    memftprt::MappingCategory category;
    category.category = "main_executable";
    category.mapping_count = 1;
    category.vsize_kb = 8;
    category.pss_kb = memftprt::OptionalKb {8, true};
    report.mapping_categories.push_back(category);
    memftprt::WorkingSetInfo working_set;
    working_set.requested_window_seconds = 2;
    working_set.actual_window_ns = 2000000000ULL;
    working_set.process_identity_matches_at_boundaries = true;
    working_set.mapping_topology_matches_at_boundaries = true;
    report.working_set = working_set;
    report.reference_scope = "since_clear_refs";
    const std::string detailed_json = memftprt::render_json(report);
    const std::string mappings_tsv = memftprt::render_mappings_tsv(report);
    if (!require(detailed_json.find("\"working_set\": {") != std::string::npos,
                 "JSON must serialize working-set metadata") ||
        !require(detailed_json.find("\"validation_scope\": \"boundary_snapshots\"") !=
                     std::string::npos,
                 "JSON must disclose boundary-only working-set validation") ||
        !require(detailed_json.find("\"category\": \"main_executable\"") !=
                     std::string::npos,
                 "JSON must serialize mapping categories") ||
        !require(mappings_tsv.find("/opt/test app\tmain_executable") != std::string::npos,
                 "mapping TSV must serialize path and category")) {
        return 1;
    }

    const std::string nmt =
        "Total: reserved=2048KB, committed=1024KB\n"
        "- Java Heap (reserved=1536KB, committed=768KB)\n"
        "unrelated prose\n";
    const auto categories = memftprt::hotspot_nmt::parse_summary(nmt);
    if (!require(categories.size() == 2, "NMT parser must retain recognized categories") ||
        !require(categories[0].name == "Total" && categories[0].reserved_kb == 2048 &&
                     categories[0].committed_kb == 1024,
                 "NMT parser must read total values") ||
        !require(categories[1].name == "Java Heap" && categories[1].reserved_kb == 1536 &&
                     categories[1].committed_kb == 768,
                 "NMT parser must read category values")) {
        return 1;
    }
    return 0;
}
