/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fast tests for platform-neutral serializers and runtime parsing.
 */

#include "../src/memory_report.h"
#include "../src/runtime/hotspot_nmt.h"

#include <iostream>
#include <string>

namespace {

bool require(bool condition, const char* message) {
    if (!condition) std::cerr << "module_test: " << message << '\n';
    return condition;
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

    const std::string json = memftprt::render_json(report);
    if (!require(json.find("\"rss\": null") != std::string::npos,
                 "JSON must preserve unavailable metrics as null") ||
        !require(json.find("\"pss\": 0") != std::string::npos,
                 "JSON must preserve an available measured zero") ||
        !require(json.find("quoted\\\"\\n<&") != std::string::npos,
                 "JSON must escape command control characters")) {
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
