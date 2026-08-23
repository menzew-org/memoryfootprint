/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * TSV, machine-readable JSON, and self-contained HTML serializers. All
 * formats consume one immutable report assembled while the target is frozen.
 */

#include "memory_report.h"

#include <array>
#include <cstdint>
#include <iomanip>
#include <sstream>

namespace memftprt {
namespace {

const char* TSV_HEADER =
    "pid\tppid\tcommand\tvsize_kb\trss_kb\tpss_kb\tuss_kb\tpss_anon_kb\t"
    "pss_file_kb\tpss_shmem_kb\tshared_clean_kb\tshared_dirty_kb\t"
    "private_clean_kb\tprivate_dirty_kb\tswap_kb\tswap_pss_kb\t"
    "anonymous_kb\treferenced_kb\tlazy_free_kb\tlocked_kb\tanon_huge_pages_kb\t"
    "shmem_pmd_mapped_kb\tfile_pmd_mapped_kb\tshared_hugetlb_kb\t"
    "private_hugetlb_kb";

using MetricList = std::array<const OptionalKb*, 21>;

MetricList metrics_in_report_order(const MemoryValues& row) {
    // The first twelve entries are the stable TSV compatibility prefix and
    // must not move.
    return {
        &row.rss_kb,
        &row.pss_kb,
        &row.uss_kb,
        &row.pss_anon_kb,
        &row.pss_file_kb,
        &row.pss_shmem_kb,
        &row.shared_clean_kb,
        &row.shared_dirty_kb,
        &row.private_clean_kb,
        &row.private_dirty_kb,
        &row.swap_kb,
        &row.swap_pss_kb,
        &row.anonymous_kb,
        &row.referenced_kb,
        &row.lazy_free_kb,
        &row.locked_kb,
        &row.anon_huge_pages_kb,
        &row.shmem_pmd_mapped_kb,
        &row.file_pmd_mapped_kb,
        &row.shared_hugetlb_kb,
        &row.private_hugetlb_kb,
    };
}

// procfs strings are byte sequences, while JSON and the HTML document promise
// UTF-8. Preserve valid sequences and replace each malformed byte sequence with
// U+FFFD so a hostile process name or mapping path cannot corrupt an artifact.
std::string sanitize_utf8(const std::string& value) {
    constexpr const char* replacement = "\xef\xbf\xbd";
    std::string result;
    result.reserve(value.size());
    std::size_t index = 0;
    while (index < value.size()) {
        const unsigned char lead = static_cast<unsigned char>(value[index]);
        if (lead < 0x80) {
            result.push_back(static_cast<char>(lead));
            ++index;
            continue;
        }

        std::size_t continuation_count = 0;
        std::uint32_t codepoint = 0;
        std::uint32_t minimum = 0;
        if (lead >= 0xc2 && lead <= 0xdf) {
            continuation_count = 1;
            codepoint = lead & 0x1fU;
            minimum = 0x80;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            continuation_count = 2;
            codepoint = lead & 0x0fU;
            minimum = 0x800;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            continuation_count = 3;
            codepoint = lead & 0x07U;
            minimum = 0x10000;
        } else {
            result.append(replacement);
            ++index;
            continue;
        }

        bool valid = index + continuation_count < value.size();
        for (std::size_t offset = 1; valid && offset <= continuation_count; ++offset) {
            const unsigned char continuation =
                static_cast<unsigned char>(value[index + offset]);
            if ((continuation & 0xc0U) != 0x80U) {
                valid = false;
                break;
            }
            codepoint = (codepoint << 6U) | (continuation & 0x3fU);
        }
        valid = valid && codepoint >= minimum && codepoint <= 0x10ffffU &&
                !(codepoint >= 0xd800U && codepoint <= 0xdfffU);
        if (!valid) {
            result.append(replacement);
            ++index;
            continue;
        }
        result.append(value, index, continuation_count + 1);
        index += continuation_count + 1;
    }
    return result;
}

std::string sanitize_tsv(std::string value) {
    value = sanitize_utf8(value);
    for (char& character : value) {
        if (character == '\t' || character == '\n' || character == '\r') character = ' ';
    }
    return value;
}

std::string json_escape(const std::string& value) {
    std::ostringstream output;
    for (unsigned char character : sanitize_utf8(value)) {
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
    for (char character : sanitize_utf8(value)) {
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

std::string hexadecimal(std::uint64_t value) {
    std::ostringstream output;
    output << "0x" << std::hex << value;
    return output.str();
}

void write_optional_tsv(std::ostream& output, const OptionalKb& metric) {
    if (metric.available) output << metric.value;
}

void write_values_tsv(std::ostream& output, const MemoryValues& values) {
    output << values.vsize_kb << '\t';
    const MetricList metrics = metrics_in_report_order(values);
    for (std::size_t index = 0; index < metrics.size(); ++index) {
        write_optional_tsv(output, *metrics[index]);
        if (index + 1 != metrics.size()) output << '\t';
    }
}

void write_tsv_row(std::ostream& output, const ProcessMemory& row) {
    output << row.pid << '\t' << row.ppid << '\t' << sanitize_tsv(row.command) << '\t';
    write_values_tsv(output, row);
    output << '\n';
}

void write_json_values(std::ostream& output, const MemoryValues& row, const char* indent) {
    output << indent << "\"vsize\": " << row.vsize_kb << ",\n";
    const auto metric = [&](const char* name, const OptionalKb& value, bool comma = true) {
        output << indent << '\"' << name << "\": ";
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
    metric("swap_pss", row.swap_pss_kb);
    metric("anonymous", row.anonymous_kb);
    metric("referenced", row.referenced_kb);
    metric("lazy_free", row.lazy_free_kb);
    metric("locked", row.locked_kb);
    metric("anon_huge_pages", row.anon_huge_pages_kb);
    metric("shmem_pmd_mapped", row.shmem_pmd_mapped_kb);
    metric("file_pmd_mapped", row.file_pmd_mapped_kb);
    metric("shared_hugetlb", row.shared_hugetlb_kb);
    metric("private_hugetlb", row.private_hugetlb_kb, false);
}

void write_json_process(std::ostream& output, const ProcessMemory& row, const char* indent) {
    output << indent << "{\n";
    output << indent << "  \"pid\": " << row.pid << ",\n";
    output << indent << "  \"ppid\": " << row.ppid << ",\n";
    output << indent << "  \"command\": \"" << json_escape(row.command) << "\",\n";
    output << indent << "  \"memory_kb\": {\n";
    const std::string values_indent = std::string(indent) + "    ";
    write_json_values(output, row, values_indent.c_str());
    output << indent << "  }\n" << indent << '}';
}

void write_json_mapping(std::ostream& output, const MemoryMapping& mapping,
                        const char* indent) {
    output << indent << "{\n";
    output << indent << "  \"pid\": " << mapping.pid << ",\n";
    output << indent << "  \"start_address\": \"" << hexadecimal(mapping.start_address)
           << "\",\n";
    output << indent << "  \"end_address\": \"" << hexadecimal(mapping.end_address)
           << "\",\n";
    output << indent << "  \"permissions\": \"" << json_escape(mapping.permissions)
           << "\",\n";
    output << indent << "  \"offset\": \"" << hexadecimal(mapping.offset) << "\",\n";
    output << indent << "  \"device\": \"" << json_escape(mapping.device) << "\",\n";
    output << indent << "  \"inode\": " << mapping.inode << ",\n";
    output << indent << "  \"pathname\": \"" << json_escape(mapping.pathname) << "\",\n";
    output << indent << "  \"category\": \"" << json_escape(mapping.category) << "\",\n";
    output << indent << "  \"kernel_page_size_kb\": ";
    if (mapping.kernel_page_size_kb.available) output << mapping.kernel_page_size_kb.value;
    else output << "null";
    output << ",\n" << indent << "  \"mmu_page_size_kb\": ";
    if (mapping.mmu_page_size_kb.available) output << mapping.mmu_page_size_kb.value;
    else output << "null";
    output << ",\n" << indent << "  \"thp_eligible\": ";
    if (mapping.thp_eligible.available) output << (mapping.thp_eligible.value ? "true" : "false");
    else output << "null";
    output << ",\n" << indent << "  \"vm_flags\": \"" << json_escape(mapping.vm_flags)
           << "\",\n";
    output << indent << "  \"memory_kb\": {\n";
    const std::string values_indent = std::string(indent) + "    ";
    write_json_values(output, mapping, values_indent.c_str());
    output << indent << "  }\n" << indent << '}';
}

std::string html_metric(const OptionalKb& metric) {
    return metric.available ? std::to_string(metric.value) : "&mdash;";
}

void write_html_process_row(std::ostream& output, const ProcessMemory& row) {
    output << "<tr><td>" << row.pid << "</td><td>" << row.ppid << "</td><td class=command>"
           << html_escape(row.command) << "</td><td>" << row.vsize_kb << "</td><td>"
           << html_metric(row.rss_kb) << "</td><td>" << html_metric(row.pss_kb)
           << "</td><td>" << html_metric(row.uss_kb) << "</td><td>"
           << html_metric(row.pss_anon_kb) << "</td><td>" << html_metric(row.pss_file_kb)
           << "</td><td>" << html_metric(row.pss_shmem_kb) << "</td><td>"
           << html_metric(row.referenced_kb) << "</td><td>" << html_metric(row.locked_kb)
           << "</td><td>" << html_metric(row.swap_kb) << "</td></tr>\n";
}

}  // namespace

std::string sanitize_tsv_field(const std::string& value) {
    return sanitize_tsv(value);
}

std::string render_tsv(const MemoryReport& report) {
    std::ostringstream output;
    output << TSV_HEADER << '\n';
    for (const ProcessMemory& process : report.processes) write_tsv_row(output, process);
    ProcessMemory total = report.total;
    total.pid = 0;
    total.ppid = 0;
    output << "TOTAL\t\t" << report.processes.size() << " processes\t";
    write_values_tsv(output, total);
    output << '\n';
    return output.str();
}

std::string render_mappings_tsv(const MemoryReport& report) {
    std::ostringstream output;
    output << "pid\tstart_address\tend_address\tpermissions\toffset\tdevice\tinode\t"
              "pathname\tcategory\tsize_kb\trss_kb\tpss_kb\tuss_kb\tpss_anon_kb\t"
              "pss_file_kb\tpss_shmem_kb\tshared_clean_kb\tshared_dirty_kb\t"
              "private_clean_kb\tprivate_dirty_kb\tswap_kb\tswap_pss_kb\tanonymous_kb\t"
              "referenced_kb\tlazy_free_kb\tlocked_kb\tanon_huge_pages_kb\t"
              "shmem_pmd_mapped_kb\tfile_pmd_mapped_kb\tshared_hugetlb_kb\t"
              "private_hugetlb_kb\tkernel_page_size_kb\tmmu_page_size_kb\t"
              "thp_eligible\tvm_flags\n";
    for (const MemoryMapping& mapping : report.mappings) {
        output << mapping.pid << '\t' << hexadecimal(mapping.start_address) << '\t'
               << hexadecimal(mapping.end_address) << '\t'
               << sanitize_tsv(mapping.permissions) << '\t' << hexadecimal(mapping.offset)
               << '\t' << sanitize_tsv(mapping.device) << '\t' << mapping.inode << '\t'
               << sanitize_tsv(mapping.pathname) << '\t' << sanitize_tsv(mapping.category)
               << '\t';
        write_values_tsv(output, mapping);
        output << '\t';
        write_optional_tsv(output, mapping.kernel_page_size_kb);
        output << '\t';
        write_optional_tsv(output, mapping.mmu_page_size_kb);
        output << '\t';
        if (mapping.thp_eligible.available) {
            output << (mapping.thp_eligible.value ? "true" : "false");
        }
        output << '\t' << sanitize_tsv(mapping.vm_flags) << '\n';
    }
    return output.str();
}

std::string render_mapping_categories_tsv(const MemoryReport& report) {
    std::ostringstream output;
    output << "category\tmapping_count\tsize_kb\trss_kb\tpss_kb\tuss_kb\t"
              "referenced_kb\tlocked_kb\tswap_kb\n";
    for (const MappingCategory& category : report.mapping_categories) {
        output << sanitize_tsv(category.category) << '\t' << category.mapping_count << '\t'
               << category.vsize_kb << '\t';
        write_optional_tsv(output, category.rss_kb);
        output << '\t';
        write_optional_tsv(output, category.pss_kb);
        output << '\t';
        write_optional_tsv(output, category.uss_kb);
        output << '\t';
        write_optional_tsv(output, category.referenced_kb);
        output << '\t';
        write_optional_tsv(output, category.locked_kb);
        output << '\t';
        write_optional_tsv(output, category.swap_kb);
        output << '\n';
    }
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
    if (report.cgroup_path.empty()) output << "null";
    else output << '\"' << json_escape(report.cgroup_path) << '\"';
    output << ",\n    \"reference_scope\": \"" << json_escape(report.reference_scope)
           << "\"\n  },\n";
    output << "  \"capabilities\": {\n";
    output << "    \"mapping_detail\": \""
           << (report.mappings_captured ? "captured" : "not_requested") << "\",\n";
    output << "    \"working_set\": \""
           << (report.working_set.has_value() ? "captured" : "not_requested") << "\",\n";
    output << "    \"clear_refs\": \""
           << (report.working_set.has_value() ? "used" : "not_requested") << "\",\n";
    output << "    \"working_set_validation\": \""
           << (report.working_set.has_value() ? report.working_set->validation_scope
                                              : "not_requested")
           << "\",\n";
    output << "    \"pidfd\": \"" << json_escape(report.pidfd_status) << "\",\n";
    output << "    \"smaps_source_by_pid\": {";
    bool first_source = true;
    for (const auto& source : report.smaps_sources) {
        output << (first_source ? "\n" : ",\n") << "      \"" << source.first << "\": \""
               << json_escape(source.second) << '\"';
        first_source = false;
    }
    if (!first_source) output << '\n';
    output << "    }\n  },\n";
    output << "  \"working_set\": ";
    if (!report.working_set.has_value()) {
        output << "null,\n";
    } else {
        const WorkingSetInfo& working_set = *report.working_set;
        output << "{\n"
               << "    \"requested_window_seconds\": "
               << working_set.requested_window_seconds << ",\n"
               << "    \"actual_window_ns\": " << working_set.actual_window_ns << ",\n"
               << "    \"started_timestamp_ns\": " << working_set.started_timestamp_ns
               << ",\n"
               << "    \"ended_timestamp_ns\": " << working_set.ended_timestamp_ns << ",\n"
               << "    \"validation_scope\": \""
               << json_escape(working_set.validation_scope) << "\",\n"
               << "    \"process_identity_matches_at_boundaries\": "
               << (working_set.process_identity_matches_at_boundaries ? "true" : "false")
               << ",\n"
               << "    \"mapping_topology_matches_at_boundaries\": "
               << (working_set.mapping_topology_matches_at_boundaries ? "true" : "false")
               << "\n"
               << "  },\n";
    }
    output << "  \"runtime_status\": {";
    bool first_runtime = true;
    for (const auto& runtime : report.runtime_status) {
        output << (first_runtime ? "\n" : ",\n") << "    \"" << runtime.first << "\": \""
               << json_escape(runtime.second) << '\"';
        first_runtime = false;
    }
    if (!first_runtime) output << '\n';
    output << "  },\n";
    output << "  \"processes\": [\n";
    for (std::size_t index = 0; index < report.processes.size(); ++index) {
        write_json_process(output, report.processes[index], "    ");
        output << (index + 1 == report.processes.size() ? "\n" : ",\n");
    }
    output << "  ],\n  \"total\": ";
    write_json_process(output, report.total, "  ");
    output << ",\n  \"mapping_categories\": ";
    if (!report.mappings_captured) {
        output << "null,\n";
    } else {
        output << "[\n";
        for (std::size_t index = 0; index < report.mapping_categories.size(); ++index) {
            const MappingCategory& category = report.mapping_categories[index];
            output << "    {\n      \"category\": \"" << json_escape(category.category)
                   << "\",\n      \"mapping_count\": " << category.mapping_count
                   << ",\n      \"memory_kb\": {\n";
            write_json_values(output, category, "        ");
            output << "      }\n    }"
                   << (index + 1 == report.mapping_categories.size() ? "\n" : ",\n");
        }
        output << "  ],\n";
    }
    output << "  \"mappings\": ";
    if (!report.mappings_captured) {
        output << "null\n";
    } else {
        output << "[\n";
        for (std::size_t index = 0; index < report.mappings.size(); ++index) {
            write_json_mapping(output, report.mappings[index], "    ");
            output << (index + 1 == report.mappings.size() ? "\n" : ",\n");
        }
        output << "  ]\n";
    }
    output << "}\n";
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
           << report.processes.size() << "</div><div><b>Reference scope:</b> "
           << html_escape(report.reference_scope) << "</div></div>";
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
    card(report.working_set.has_value() ? "Referenced in window" : "Referenced",
         html_metric(report.total.referenced_kb));
    card("Swap", html_metric(report.total.swap_kb));
    output << "</div><div class=note><p><b>How to read this:</b> VSize is reserved address "
              "space, RSS counts resident mappings, PSS divides shared pages among their users, "
              "and USS is private resident memory. For a multi-process application, total PSS is "
              "usually the best physical-ownership estimate. Runtime values such as JVM NMT "
              "overlap these OS values and must not be added to them.</p>";
    if (report.working_set.has_value()) {
        output << "<p><b>Working set:</b> Referenced values are a kernel estimate of pages "
                  "accessed after <code>clear_refs</code> during a "
               << report.working_set->requested_window_seconds
               << "-second observation window; they are not an exact access trace. Process and "
                  "mapping identities are compared at the two frozen boundaries, so transient "
                  "churn entirely inside the window is not observable.</p>";
    }
    output << "</div><h2>Capture capabilities</h2><ul><li>Mapping detail: "
           << (report.mappings_captured ? "captured" : "not requested")
           << "</li><li>Working set: "
           << (report.working_set.has_value() ? "captured" : "not requested")
           << "</li><li>clear_refs: "
           << (report.working_set.has_value() ? "used" : "not requested")
           << "</li><li>Working-set validation: "
           << (report.working_set.has_value() ? "boundary snapshots" : "not requested")
           << "</li><li>pidfd: " << html_escape(report.pidfd_status) << "</li>";
    for (const auto& source : report.smaps_sources) {
        output << "<li>PID " << source.first << " accounting source: "
               << html_escape(source.second) << "</li>";
    }
    output << "</ul>";
    output << "<h2>Processes</h2><div class=scroll><table><thead><tr><th>PID</th><th>PPID</th>"
              "<th class=command>Command</th><th>VSize KB</th><th>RSS KB</th><th>PSS KB</th>"
              "<th>USS KB</th><th>Anon PSS</th><th>File PSS</th><th>Shmem PSS</th>"
              "<th>Referenced KB</th><th>Locked KB</th><th>Swap KB</th></tr></thead><tbody>";
    for (const ProcessMemory& process : report.processes) write_html_process_row(output, process);
    output << "</tbody></table></div>";
    if (report.mappings_captured) {
        output << "<h2>Mapping categories</h2><div class=scroll><table><thead><tr>"
                  "<th class=command>Category</th><th>Mappings</th><th>Size KB</th><th>RSS KB</th>"
                  "<th>PSS KB</th><th>USS KB</th><th>Referenced KB</th></tr></thead><tbody>";
        for (const MappingCategory& category : report.mapping_categories) {
            output << "<tr><td class=command>" << html_escape(category.category) << "</td><td>"
                   << category.mapping_count << "</td><td>" << category.vsize_kb << "</td><td>"
                   << html_metric(category.rss_kb) << "</td><td>"
                   << html_metric(category.pss_kb) << "</td><td>"
                   << html_metric(category.uss_kb) << "</td><td>"
                   << html_metric(category.referenced_kb) << "</td></tr>";
        }
        output << "</tbody></table></div><details><summary>Show all " << report.mappings.size()
               << " mappings</summary><div class=scroll><table><thead><tr><th>PID</th>"
                  "<th class=command>Range</th><th>Perms</th><th class=command>Category</th>"
                  "<th class=command>Path</th><th>Size KB</th><th>RSS KB</th><th>PSS KB</th>"
                  "<th>USS KB</th><th>Referenced KB</th></tr></thead><tbody>";
        for (const MemoryMapping& mapping : report.mappings) {
            output << "<tr><td>" << mapping.pid << "</td><td class=command><code>"
                   << hexadecimal(mapping.start_address) << "-" << hexadecimal(mapping.end_address)
                   << "</code></td><td>" << html_escape(mapping.permissions)
                   << "</td><td class=command>" << html_escape(mapping.category)
                   << "</td><td class=command>" << html_escape(mapping.pathname)
                   << "</td><td>" << mapping.vsize_kb << "</td><td>"
                   << html_metric(mapping.rss_kb) << "</td><td>"
                   << html_metric(mapping.pss_kb) << "</td><td>"
                   << html_metric(mapping.uss_kb) << "</td><td>"
                   << html_metric(mapping.referenced_kb) << "</td></tr>";
        }
        output << "</tbody></table></div></details>";
    }
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
