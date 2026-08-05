/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * TSV, machine-readable JSON, and self-contained HTML serializers. All three
 * consume the same MemoryReport snapshot to keep their numbers consistent.
 */

#include "memory_report.h"

#include <iomanip>
#include <sstream>

namespace memftprt {
namespace {

const char* TSV_HEADER =
    "pid\tppid\tcommand\tvsize_kb\trss_kb\tpss_kb\tuss_kb\tpss_anon_kb\t"
    "pss_file_kb\tpss_shmem_kb\tshared_clean_kb\tshared_dirty_kb\t"
    "private_clean_kb\tprivate_dirty_kb\tswap_kb\tswap_pss_kb";

std::string sanitize_tsv(std::string value) {
    for (char& character : value) {
        if (character == '\t' || character == '\n' || character == '\r') character = ' ';
    }
    return value;
}

std::string json_escape(const std::string& value) {
    std::ostringstream output;
    for (unsigned char character : value) {
        switch (character) {
            case '"': output << "\\\""; break;
            case '\\': output << "\\\\"; break;
            case '\b': output << "\\b"; break;
            case '\f': output << "\\f"; break;
            case '\n': output << "\\n"; break;
            case '\r': output << "\\r"; break;
            case '\t': output << "\\t"; break;
            default:
                if (character < 0x20) {
                    output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                           << static_cast<unsigned>(character) << std::dec << std::setfill(' ');
                } else {
                    output << static_cast<char>(character);
                }
        }
    }
    return output.str();
}

std::string html_escape(const std::string& value) {
    std::ostringstream output;
    for (char character : value) {
        switch (character) {
            case '&': output << "&amp;"; break;
            case '<': output << "&lt;"; break;
            case '>': output << "&gt;"; break;
            case '"': output << "&quot;"; break;
            case '\'': output << "&#39;"; break;
            default: output << character;
        }
    }
    return output.str();
}

void write_optional_tsv(std::ostream& output, const OptionalKb& metric) {
    if (metric.available) output << metric.value;
}

void write_tsv_row(std::ostream& output, const ProcessMemory& row) {
    output << row.pid << '\t' << row.ppid << '\t' << sanitize_tsv(row.command) << '\t'
           << row.vsize_kb << '\t';
    const OptionalKb* metrics[] = {
        &row.rss_kb,          &row.pss_kb,           &row.uss_kb,
        &row.pss_anon_kb,     &row.pss_file_kb,      &row.pss_shmem_kb,
        &row.shared_clean_kb, &row.shared_dirty_kb,  &row.private_clean_kb,
        &row.private_dirty_kb, &row.swap_kb,         &row.swap_pss_kb,
    };
    for (std::size_t index = 0; index < sizeof(metrics) / sizeof(metrics[0]); ++index) {
        write_optional_tsv(output, *metrics[index]);
        if (index + 1 != sizeof(metrics) / sizeof(metrics[0])) output << '\t';
    }
    output << '\n';
}

void write_json_memory(std::ostream& output, const ProcessMemory& row, const char* indent) {
    output << indent << "{\n";
    output << indent << "  \"pid\": " << row.pid << ",\n";
    output << indent << "  \"ppid\": " << row.ppid << ",\n";
    output << indent << "  \"command\": \"" << json_escape(row.command) << "\",\n";
    output << indent << "  \"memory_kb\": {\n";
    output << indent << "    \"vsize\": " << row.vsize_kb << ",\n";
    const auto metric = [&](const char* name, const OptionalKb& value, bool comma = true) {
        output << indent << "    \"" << name << "\": ";
        if (value.available) output << value.value; else output << "null";
        output << (comma ? ",\n" : "\n");
    };
    metric("rss", row.rss_kb);
    metric("pss", row.pss_kb);
    metric("uss", row.uss_kb);
    metric("pss_anon", row.pss_anon_kb);
    metric("pss_file", row.pss_file_kb);
    metric("pss_shmem", row.pss_shmem_kb);
    metric("shared_clean", row.shared_clean_kb);
    metric("shared_dirty", row.shared_dirty_kb);
    metric("private_clean", row.private_clean_kb);
    metric("private_dirty", row.private_dirty_kb);
    metric("swap", row.swap_kb);
    metric("swap_pss", row.swap_pss_kb, false);
    output << indent << "  }\n" << indent << '}';
}

std::string html_metric(const OptionalKb& metric) {
    return metric.available ? std::to_string(metric.value) : "&mdash;";
}

void write_html_row(std::ostream& output, const ProcessMemory& row) {
    output << "<tr><td>" << row.pid << "</td><td>" << row.ppid << "</td><td class=command>"
           << html_escape(row.command) << "</td><td>" << row.vsize_kb << "</td><td>"
           << html_metric(row.rss_kb) << "</td><td>" << html_metric(row.pss_kb)
           << "</td><td>" << html_metric(row.uss_kb) << "</td><td>"
           << html_metric(row.pss_anon_kb) << "</td><td>" << html_metric(row.pss_file_kb)
           << "</td><td>" << html_metric(row.pss_shmem_kb) << "</td><td>"
           << html_metric(row.swap_kb) << "</td></tr>\n";
}

}  // namespace

std::string render_tsv(const MemoryReport& report) {
    std::ostringstream output;
    output << TSV_HEADER << '\n';
    for (const ProcessMemory& process : report.processes) write_tsv_row(output, process);
    ProcessMemory total = report.total;
    total.pid = 0;
    total.ppid = 0;
    output << "TOTAL\t\t" << report.processes.size() << " processes\t" << total.vsize_kb << '\t';
    const OptionalKb* metrics[] = {
        &total.rss_kb,          &total.pss_kb,           &total.uss_kb,
        &total.pss_anon_kb,     &total.pss_file_kb,      &total.pss_shmem_kb,
        &total.shared_clean_kb, &total.shared_dirty_kb,  &total.private_clean_kb,
        &total.private_dirty_kb,&total.swap_kb,          &total.swap_pss_kb,
    };
    for (std::size_t index = 0; index < sizeof(metrics) / sizeof(metrics[0]); ++index) {
        write_optional_tsv(output, *metrics[index]);
        if (index + 1 != sizeof(metrics) / sizeof(metrics[0])) output << '\t';
    }
    output << '\n';
    return output.str();
}

std::string render_json(const MemoryReport& report) {
    std::ostringstream output;
    output << "{\n";
    output << "  \"schema_version\": " << report.schema_version << ",\n";
    output << "  \"unit\": \"KB\",\n";
    output << "  \"capture\": {\n";
    output << "    \"root_pid\": " << report.root_pid << ",\n";
    output << "    \"launcher_pid\": ";
    if (report.launcher_pid > 0 && report.launcher_pid != report.root_pid) {
        output << report.launcher_pid;
    } else {
        output << "null";
    }
    output << ",\n    \"timestamp_ns\": " << report.timestamp_ns << ",\n";
    output << "    \"freeze_backend\": \"" << json_escape(report.freeze_backend) << "\",\n";
    output << "    \"cgroup_path\": ";
    if (report.cgroup_path.empty()) {
        output << "null";
    } else {
        output << '"' << json_escape(report.cgroup_path) << '"';
    }
    output << "\n  },\n";
    output << "  \"runtime_status\": {";
    bool first_runtime = true;
    for (const auto& runtime : report.runtime_status) {
        output << (first_runtime ? "\n" : ",\n") << "    \"" << runtime.first << "\": \""
               << json_escape(runtime.second) << '"';
        first_runtime = false;
    }
    if (!first_runtime) output << '\n';
    output << "  },\n";
    output << "  \"processes\": [\n";
    for (std::size_t index = 0; index < report.processes.size(); ++index) {
        write_json_memory(output, report.processes[index], "    ");
        output << (index + 1 == report.processes.size() ? "\n" : ",\n");
    }
    output << "  ],\n  \"total\": ";
    write_json_memory(output, report.total, "  ");
    output << "\n}\n";
    return output.str();
}

std::string render_html(const MemoryReport& report) {
    std::ostringstream output;
    output << "<!doctype html><html lang=en><head><meta charset=utf-8>"
              "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
              "<title>memftprt capture " << report.root_pid << "</title><style>"
              ":root{color-scheme:light dark;font-family:ui-sans-serif,system-ui,sans-serif}"
              "body{max-width:1200px;margin:2rem auto;padding:0 1rem;line-height:1.45}"
              ".meta,.cards{display:flex;flex-wrap:wrap;gap:1rem}.card{border:1px solid #8886;"
              "border-radius:.6rem;padding:.8rem 1rem;min-width:9rem}.value{font-size:1.5rem;"
              "font-weight:700}table{border-collapse:collapse;width:100%;font-variant-numeric:"
              "tabular-nums}th,td{border-bottom:1px solid #8886;padding:.45rem;text-align:right}"
              "th{position:sticky;top:0;background:Canvas}.command{text-align:left}"
              ".scroll{overflow:auto}.note{border-left:.3rem solid #4b8;padding:.2rem 1rem}"
              "code{background:#8882;padding:.1rem .25rem;border-radius:.2rem}</style></head><body>";
    output << "<h1>memftprt memory capture</h1><div class=meta><div><b>Root PID:</b> "
           << report.root_pid << "</div><div><b>Freeze:</b> "
           << html_escape(report.freeze_backend) << "</div><div><b>Processes:</b> "
           << report.processes.size() << "</div></div>";
    if (report.launcher_pid > 0 && report.launcher_pid != report.root_pid) {
        output << "<p><b>Original launcher:</b> " << report.launcher_pid << "</p>";
    }
    if (!report.cgroup_path.empty()) {
        output << "<p><b>Cgroup:</b> <code>" << html_escape(report.cgroup_path) << "</code></p>";
    }
    output << "<h2>Process-tree totals</h2><div class=cards>";
    const auto card = [&](const char* label, const std::string& value) {
        output << "<div class=card><div>" << label << "</div><div class=value>" << value
               << " KB</div></div>";
    };
    card("Virtual size", std::to_string(report.total.vsize_kb));
    card("RSS", html_metric(report.total.rss_kb));
    card("PSS", html_metric(report.total.pss_kb));
    card("USS", html_metric(report.total.uss_kb));
    card("Swap", html_metric(report.total.swap_kb));
    output << "</div><div class=note><p><b>How to read this:</b> VSize is reserved address "
              "space, RSS counts resident mappings, PSS divides shared pages among their users, "
              "and USS is private resident memory. For a multi-process application, total PSS is "
              "usually the best physical-ownership estimate. Runtime values such as JVM NMT "
              "overlap these OS values and must not be added to them.</p></div>";
    output << "<h2>Processes</h2><div class=scroll><table><thead><tr><th>PID</th><th>PPID</th>"
              "<th class=command>Command</th><th>VSize KB</th><th>RSS KB</th><th>PSS KB</th>"
              "<th>USS KB</th><th>Anon PSS</th><th>File PSS</th><th>Shmem PSS</th>"
              "<th>Swap KB</th></tr></thead><tbody>";
    for (const ProcessMemory& process : report.processes) write_html_row(output, process);
    output << "</tbody></table></div>";
    if (!report.runtime_status.empty()) {
        output << "<h2>Runtime probes</h2><ul>";
        for (const auto& runtime : report.runtime_status) {
            output << "<li>PID " << runtime.first << ": " << html_escape(runtime.second) << "</li>";
        }
        output << "</ul>";
    }
    output << "<p>Generated by memftprt. Empty or &mdash; values were unavailable from the "
              "running kernel and do not mean zero.</p></body></html>\n";
    return output.str();
}

}  // namespace memftprt
