/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Benchmarks for the platform-neutral hot paths: smaps parsing, mapping
 * classification, report serialization, and HotSpot NMT parsing. Inputs are
 * synthetic but shaped like real captures so results are deterministic and
 * independent of the host's /proc tree.
 */

#include "linux/proc_memory.h"
#include "linux/working_set.h"
#include "memory_report.h"
#include "runtime/hotspot_nmt.h"

#include <benchmark/benchmark.h>

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace procfs = memftprt::linux_procfs;

const char* const kExecutable = "/opt/app/bin/server";

// Rotate through the mapping kinds typically seen in a JVM or native server.
std::string mapping_pathname(std::size_t index) {
    switch (index % 10) {
        case 0: return kExecutable;
        case 1: return "/usr/lib/x86_64-linux-gnu/libc.so.6";
        case 2: return "/usr/lib/jvm/lib/server/libjvm.so";
        case 3: return "[heap]";
        case 4: return "";
        case 5: return "[anon:java heap]";
        case 6: return "/dev/shm/segment-" + std::to_string(index);
        case 7: return "/var/cache/app/data file " + std::to_string(index) + ".bin";
        case 8: return "/tmp/removed-" + std::to_string(index) + " (deleted)";
        default: return "[stack]";
    }
}

const char* mapping_permissions(std::size_t index) {
    switch (index % 4) {
        case 0: return "r-xp";
        case 1: return "r--p";
        case 2: return "rw-p";
        default: return "rwxp";
    }
}

std::string make_smaps(std::size_t mapping_count) {
    std::string out;
    out.reserve(mapping_count * 820);
    char header[160];
    for (std::size_t i = 0; i < mapping_count; ++i) {
        const unsigned long long start = 0x7f0000000000ULL + i * 0x10000ULL;
        std::snprintf(header, sizeof(header), "%llx-%llx %s %08llx 08:01 %zu ", start,
                      start + 0x8000ULL, mapping_permissions(i),
                      static_cast<unsigned long long>((i % 7) * 0x1000), 1000 + i);
        out += header;
        out += mapping_pathname(i);
        out += '\n';
        const unsigned long size = 32 + (i % 13) * 4;
        const unsigned long rss = size / 2;
        out += "Size:              " + std::to_string(size) + " kB\n";
        out += "KernelPageSize:        4 kB\n";
        out += "MMUPageSize:           4 kB\n";
        out += "Rss:               " + std::to_string(rss) + " kB\n";
        out += "Pss:               " + std::to_string(rss / 2) + " kB\n";
        out += "Pss_Dirty:         " + std::to_string(rss / 4) + " kB\n";
        out += "Shared_Clean:      " + std::to_string(rss / 4) + " kB\n";
        out += "Shared_Dirty:          0 kB\n";
        out += "Private_Clean:     " + std::to_string(rss / 4) + " kB\n";
        out += "Private_Dirty:     " + std::to_string(rss / 2) + " kB\n";
        out += "Referenced:        " + std::to_string(rss) + " kB\n";
        out += "Anonymous:         " + std::to_string(rss / 2) + " kB\n";
        out += "KSM:                   0 kB\n";
        out += "LazyFree:              0 kB\n";
        out += "AnonHugePages:         0 kB\n";
        out += "ShmemPmdMapped:        0 kB\n";
        out += "FilePmdMapped:         0 kB\n";
        out += "Shared_Hugetlb:        0 kB\n";
        out += "Private_Hugetlb:       0 kB\n";
        out += "Swap:                  0 kB\n";
        out += "SwapPss:               0 kB\n";
        out += "Locked:                0 kB\n";
        out += "THPeligible:           " + std::to_string(i % 2) + "\n";
        out += "VmFlags: rd wr mr mw me ac sd\n";
    }
    return out;
}

procfs::SmapsDetail parse_or_die(const std::string& text) {
    std::istringstream input(text);
    procfs::SmapsDetail detail;
    std::string reason;
    if (!procfs::parse_smaps_detail(input, &detail, &reason)) {
        std::fprintf(stderr, "benchmark fixture failed to parse: %s\n", reason.c_str());
        std::abort();
    }
    return detail;
}

memftprt::OptionalKb kb(unsigned long value) {
    return memftprt::OptionalKb {value, true};
}

void fill_values(memftprt::MemoryValues* values, const procfs::SmapsMetrics& metrics) {
    values->vsize_kb = metrics.size_kb;
    values->rss_kb = kb(metrics.rss_kb);
    values->pss_kb = kb(metrics.pss_kb);
    values->uss_kb = kb(procfs::uss_kb(metrics));
    values->shared_clean_kb = kb(metrics.shared_clean_kb);
    values->shared_dirty_kb = kb(metrics.shared_dirty_kb);
    values->private_clean_kb = kb(metrics.private_clean_kb);
    values->private_dirty_kb = kb(metrics.private_dirty_kb);
    values->anonymous_kb = kb(metrics.anonymous_kb);
    values->referenced_kb = kb(metrics.referenced_kb);
    values->swap_kb = kb(metrics.swap_kb);
}

// Build a report for a process tree where every process has the same mapping
// layout. When `with_mappings` is set, the per-mapping detail is included too.
memftprt::MemoryReport make_report(std::size_t process_count, std::size_t mappings_per_process,
                                   bool with_mappings) {
    const procfs::SmapsDetail detail = parse_or_die(make_smaps(mappings_per_process));
    memftprt::MemoryReport report;
    report.root_pid = 1000;
    report.freeze_backend = "cgroup";
    report.cgroup_path = "/sys/fs/cgroup/memftprt/target";
    report.mappings_captured = with_mappings;
    for (std::size_t p = 0; p < process_count; ++p) {
        memftprt::ProcessMemory process;
        process.pid = static_cast<std::int64_t>(1000 + p);
        process.ppid = p == 0 ? 1 : 1000;
        process.command = "server --worker=" + std::to_string(p) + " --config=\"/etc/app.conf\"";
        fill_values(&process, detail.total);
        report.processes.push_back(process);
        report.smaps_sources[process.pid] = "smaps_rollup";
        report.runtime_status[process.pid] = "not_applicable";
        if (!with_mappings) continue;
        for (const procfs::SmapsMapping& source : detail.mappings) {
            memftprt::MemoryMapping mapping;
            mapping.pid = process.pid;
            mapping.start_address = source.start_address;
            mapping.end_address = source.end_address;
            mapping.offset = source.offset;
            mapping.inode = source.inode;
            mapping.permissions = source.permissions;
            mapping.device = source.device;
            mapping.pathname = source.pathname;
            mapping.category = procfs::classify_mapping(source, kExecutable);
            mapping.kernel_page_size_kb = kb(source.kernel_page_size_kb);
            mapping.mmu_page_size_kb = kb(source.mmu_page_size_kb);
            mapping.thp_eligible = memftprt::OptionalBool {source.thp_eligible, true};
            mapping.vm_flags = source.vm_flags;
            fill_values(&mapping, source.memory);
            report.mappings.push_back(std::move(mapping));
        }
    }
    report.total = report.processes.front();
    report.total.command = "TOTAL";
    if (with_mappings) {
        for (const char* name : {"main_executable", "shared_library", "heap", "anonymous",
                                 "anonymous_executable", "shared_memory", "file",
                                 "deleted_file", "stack"}) {
            memftprt::MappingCategory category;
            category.category = name;
            category.mapping_count = mappings_per_process / 9;
            fill_values(&category, detail.total);
            report.mapping_categories.push_back(category);
        }
    }
    return report;
}

std::string make_nmt_summary(std::size_t repeats) {
    static const char* const kCategories[] = {
        "Java Heap", "Class", "Thread", "Code", "GC", "GCCardSet", "Compiler",
        "JVMCI", "Internal", "Other", "Symbol", "Native Memory Tracking",
        "Shared class space", "Arena Chunk", "Module", "Safepoint", "Synchronization",
        "Serviceability", "Metaspace", "String Deduplication", "Object Monitors"};
    std::string out = "Native Memory Tracking:\n\n"
                      "(Omitting categories weighting less than 1KB)\n\n"
                      "Total: reserved=1767503KB, committed=153723KB\n"
                      "       malloc: 24947KB #39021\n"
                      "       mmap:   reserved=1742556KB, committed=128776KB\n\n";
    for (std::size_t r = 0; r < repeats; ++r) {
        unsigned long value = 1024;
        for (const char* name : kCategories) {
            value = value * 3 + 17;
            out += "-                 ";
            out += name;
            out += " (reserved=" + std::to_string(value * 2) +
                   "KB, committed=" + std::to_string(value) + "KB)\n";
            out += "                            (malloc=" + std::to_string(value / 8) +
                   "KB #" + std::to_string(value % 997) + ")\n";
            out += "                            (mmap: reserved=" +
                   std::to_string(value) + "KB, committed=" + std::to_string(value / 2) +
                   "KB)\n\n";
            value %= 100000;
        }
    }
    return out;
}

}  // namespace

static void BM_ParseSmapsDetail(benchmark::State& state) {
    const std::string text = make_smaps(static_cast<std::size_t>(state.range(0)));
    for (auto _ : state) {
        std::istringstream input(text);
        procfs::SmapsDetail detail;
        std::string reason;
        bool ok = procfs::parse_smaps_detail(input, &detail, &reason);
        benchmark::DoNotOptimize(ok);
        benchmark::DoNotOptimize(detail.total.pss_kb);
    }
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * text.size()));
}
BENCHMARK(BM_ParseSmapsDetail)->Arg(64)->Arg(1024);

static void BM_ClassifyMappings(benchmark::State& state) {
    const procfs::SmapsDetail detail = parse_or_die(make_smaps(1000));
    for (auto _ : state) {
        for (const procfs::SmapsMapping& mapping : detail.mappings) {
            std::string category = procfs::classify_mapping(mapping, kExecutable);
            benchmark::DoNotOptimize(category);
        }
    }
}
BENCHMARK(BM_ClassifyMappings);

static void BM_MergeSmapsMetrics(benchmark::State& state) {
    const procfs::SmapsDetail detail = parse_or_die(make_smaps(1000));
    for (auto _ : state) {
        procfs::SmapsMetrics total;
        for (std::size_t i = 0; i < detail.mappings.size(); ++i) {
            procfs::merge_smaps_metrics(&total, detail.mappings[i].memory, i == 0);
        }
        benchmark::DoNotOptimize(total);
    }
}
BENCHMARK(BM_MergeSmapsMetrics);

static void BM_MappingIdentities(benchmark::State& state) {
    const procfs::SmapsDetail detail = parse_or_die(make_smaps(1000));
    for (auto _ : state) {
        auto identities = memftprt::linux_working_set::mapping_identities(detail);
        benchmark::DoNotOptimize(identities);
    }
}
BENCHMARK(BM_MappingIdentities);

static void BM_RenderTsv(benchmark::State& state) {
    const memftprt::MemoryReport report =
        make_report(static_cast<std::size_t>(state.range(0)), 64, false);
    for (auto _ : state) {
        std::string out = memftprt::render_tsv(report);
        benchmark::DoNotOptimize(out);
    }
}
BENCHMARK(BM_RenderTsv)->Arg(8)->Arg(128);

static void BM_RenderJson(benchmark::State& state) {
    const memftprt::MemoryReport report =
        make_report(static_cast<std::size_t>(state.range(0)), 64, false);
    for (auto _ : state) {
        std::string out = memftprt::render_json(report);
        benchmark::DoNotOptimize(out);
    }
}
BENCHMARK(BM_RenderJson)->Arg(8)->Arg(128);

static void BM_RenderHtml(benchmark::State& state) {
    const memftprt::MemoryReport report =
        make_report(static_cast<std::size_t>(state.range(0)), 64, false);
    for (auto _ : state) {
        std::string out = memftprt::render_html(report);
        benchmark::DoNotOptimize(out);
    }
}
BENCHMARK(BM_RenderHtml)->Arg(8)->Arg(128);

static void BM_RenderJsonWithMappings(benchmark::State& state) {
    const memftprt::MemoryReport report = make_report(4, 256, true);
    for (auto _ : state) {
        std::string out = memftprt::render_json(report);
        benchmark::DoNotOptimize(out);
    }
}
BENCHMARK(BM_RenderJsonWithMappings);

static void BM_RenderMappingsTsv(benchmark::State& state) {
    const memftprt::MemoryReport report = make_report(4, 256, true);
    for (auto _ : state) {
        std::string out = memftprt::render_mappings_tsv(report);
        benchmark::DoNotOptimize(out);
    }
}
BENCHMARK(BM_RenderMappingsTsv);

static void BM_RenderMappingCategoriesTsv(benchmark::State& state) {
    const memftprt::MemoryReport report = make_report(4, 256, true);
    for (auto _ : state) {
        std::string out = memftprt::render_mapping_categories_tsv(report);
        benchmark::DoNotOptimize(out);
    }
}
BENCHMARK(BM_RenderMappingCategoriesTsv);

static void BM_SanitizeTsvField(benchmark::State& state) {
    std::string field;
    for (int i = 0; i < 64; ++i) {
        field += "java -Xmx2g\t-Dapp.name=\"service\"\n";
        field.push_back(static_cast<char>(0xff));
        field += "\xc3\xa9t\xc3\xa9 ";
    }
    for (auto _ : state) {
        std::string out = memftprt::sanitize_tsv_field(field);
        benchmark::DoNotOptimize(out);
    }
}
BENCHMARK(BM_SanitizeTsvField);

static void BM_ParseNmtSummary(benchmark::State& state) {
    const std::string summary = make_nmt_summary(static_cast<std::size_t>(state.range(0)));
    for (auto _ : state) {
        auto categories = memftprt::hotspot_nmt::parse_summary(summary);
        benchmark::DoNotOptimize(categories);
    }
}
BENCHMARK(BM_ParseNmtSummary)->Arg(1)->Arg(16);

BENCHMARK_MAIN();
