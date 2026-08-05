/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * HotSpot Native Memory Tracking summary parser implementation.
 */

#include "hotspot_nmt.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <utility>

namespace memftprt::hotspot_nmt {
namespace {

std::string trim_copy(const std::string& value) {
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const std::size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool parse_value_kb(const std::string& line, const char* key, unsigned long* value) {
    const std::size_t position = line.find(key);
    if (position == std::string::npos) return false;

    const char* start = line.c_str() + position + std::strlen(key);
    char* end = nullptr;
    errno = 0;
    const unsigned long parsed = std::strtoul(start, &end, 10);
    if (errno != 0 || end == start) return false;
    if (std::strncmp(end, "KB", 2) != 0 && *end != ',' && *end != ')') return false;
    *value = parsed;
    return true;
}

}  // namespace

std::vector<Category> parse_summary(const std::string& raw_output) {
    std::vector<Category> categories;
    std::istringstream lines(raw_output);
    std::string line;
    while (std::getline(lines, line)) {
        const std::string trimmed = trim_copy(line);
        std::string name;
        if (trimmed.rfind("Total:", 0) == 0) {
            name = "Total";
        } else if (!trimmed.empty() && trimmed[0] == '-') {
            const std::size_t parenthesis = trimmed.find('(');
            if (parenthesis != std::string::npos) {
                name = trim_copy(trimmed.substr(1, parenthesis - 1));
            }
        }

        Category category;
        category.name = std::move(name);
        if (category.name.empty() ||
            !parse_value_kb(trimmed, "reserved=", &category.reserved_kb) ||
            !parse_value_kb(trimmed, "committed=", &category.committed_kb)) {
            continue;
        }
        categories.push_back(std::move(category));
    }
    return categories;
}

}  // namespace memftprt::hotspot_nmt
